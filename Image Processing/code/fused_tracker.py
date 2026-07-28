#!/usr/bin/env python3
"""
fused_tracker.py

Adds two things on top of a raw per-frame detector (HogDetector or
MobileNetSsdDetector from person_detector.py):

1. Multi-cue detection fusion
   The raw detector (full-body HOG/SSD) is combined with Haar-cascade
   detectors for face, profile-face, and upper-body. Any one of these
   firing counts as "a person is here" — so if the full-body detector
   misses (common at a distance, or with partial/side poses), a face or
   upper-body hit still keeps the person detected.

2. Persistent tracking ("track memory")
   Every confirmed person gets a lightweight CSRT tracker. On frames
   where the detectors miss entirely, the tracker keeps predicting the
   box instead of the person disappearing. A track is only dropped after
   it fails to find visual support (no detector hit AND tracker itself
   loses confidence) for `max_missed_frames` in a row. This is what
   fixes "detected for one frame then instantly lost".

Usage from person_detector.py:

    from fused_tracker import FusedTrackedDetector
    detector = FusedTrackedDetector(base_detector, cfg)
    boxes = detector.detect(frame)   # same (x, y, w, h) list interface
"""

import os
import time

import cv2
import numpy as np


def _iou(box_a, box_b):
    """Intersection-over-union of two (x, y, w, h) boxes."""
    ax, ay, aw, ah = box_a
    bx, by, bw, bh = box_b
    ax2, ay2 = ax + aw, ay + ah
    bx2, by2 = bx + bw, by + bh

    ix1, iy1 = max(ax, bx), max(ay, by)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0, ix2 - ix1), max(0, iy2 - iy1)
    inter = iw * ih
    if inter == 0:
        return 0.0
    union = aw * ah + bw * bh - inter
    return inter / union if union > 0 else 0.0


def _resolve_cascade_path(filename):
    """Locate a haarcascade XML file. Prefers the copy bundled in this
    project's cascades/ folder (works regardless of how the installed
    OpenCV build packages, or fails to package, its data files), and
    falls back to cv2.data.haarcascades if present there instead."""
    here = os.path.dirname(os.path.abspath(__file__))
    bundled = os.path.join(here, "cascades", filename)
    if os.path.exists(bundled):
        return bundled

    try:
        candidate = os.path.join(cv2.data.haarcascades, filename)
        if os.path.exists(candidate):
            return candidate
    except AttributeError:
        pass

    raise FileNotFoundError(
        f"Could not find cascade file '{filename}'. Expected it bundled at "
        f"'{bundled}' (ship it in a cascades/ folder next to fused_tracker.py) "
        f"or inside cv2.data.haarcascades."
    )


class CascadeCueDetector:
    """Secondary detectors that fire on face / upper body, so a person can
    still be "seen" even when the primary full-body detector misses."""

    def __init__(self, cfg=None):
        self.frontal_face = cv2.CascadeClassifier(
            _resolve_cascade_path("haarcascade_frontalface_default.xml"))
        self.profile_face = cv2.CascadeClassifier(
            _resolve_cascade_path("haarcascade_profileface.xml"))
        self.upper_body = cv2.CascadeClassifier(
            _resolve_cascade_path("haarcascade_upperbody.xml"))

        for name, clf in (("frontal_face", self.frontal_face),
                           ("profile_face", self.profile_face),
                           ("upper_body", self.upper_body)):
            if clf.empty():
                raise RuntimeError(
                    f"[fused_tracker] Cascade '{name}' loaded but is empty — "
                    "the XML file is likely corrupt or truncated. Re-download "
                    "the cascades/ files."
                )

        c = cfg["fusion"] if cfg and "fusion" in cfg else {}
        self.face_scale_factor_up = float(c.get("upper_body_box_expand", 3.2)) \
            if isinstance(c, dict) else cfg.getfloat("fusion", "upper_body_box_expand", fallback=3.2)

    def detect(self, gray):
        """Returns a list of ('cue_type', (x, y, w, h)) boxes in original
        frame coordinates. Face boxes are expanded into an approximate
        full-body box so they can be matched/merged against full-body
        detections and tracks on a comparable scale."""
        results = []

        faces = self.frontal_face.detectMultiScale(
            gray, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30)
        )
        for (x, y, w, h) in faces:
            results.append(("face", self._expand_face_to_body(x, y, w, h, gray.shape)))

        profiles = self.profile_face.detectMultiScale(
            gray, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30)
        )
        for (x, y, w, h) in profiles:
            results.append(("face", self._expand_face_to_body(x, y, w, h, gray.shape)))

        upper = self.upper_body.detectMultiScale(
            gray, scaleFactor=1.05, minNeighbors=4, minSize=(40, 40)
        )
        for (x, y, w, h) in upper:
            results.append(("upper_body", self._expand_upper_to_body(x, y, w, h, gray.shape)))

        return results

    def _expand_face_to_body(self, x, y, w, h, frame_shape):
        """A detected face is roughly the top ~1/8 of a standing person.
        Expand downward/outward so this box is comparable to a full-body
        box for IoU matching against tracks/full-body detections."""
        H, W = frame_shape[:2]
        body_h = int(h * 7.5)
        body_w = int(w * 2.5)
        cx = x + w // 2
        nx = max(0, cx - body_w // 2)
        ny = max(0, y - int(h * 0.5))
        nw = min(body_w, W - nx)
        nh = min(body_h, H - ny)
        return (nx, ny, nw, nh)

    def _expand_upper_to_body(self, x, y, w, h, frame_shape):
        """Upper-body cascade box roughly covers head-to-waist; expand
        downward to approximate a full standing body."""
        H, W = frame_shape[:2]
        body_h = int(h * 2.0)
        nh = min(body_h, H - y)
        return (x, y, w, nh)


def _create_cv2_tracker():
    """OpenCV exposes tracker constructors differently depending on
    version/build (plain cv2.TrackerCSRT_create vs cv2.legacy.* vs missing
    entirely if opencv-contrib isn't installed). Try the best available
    option and fall back gracefully instead of crashing."""
    candidates = [
        lambda: cv2.TrackerCSRT_create(),
        lambda: cv2.legacy.TrackerCSRT_create(),
        lambda: cv2.TrackerKCF_create(),
        lambda: cv2.legacy.TrackerKCF_create(),
        lambda: cv2.TrackerMOSSE_create(),
        lambda: cv2.legacy.TrackerMOSSE_create(),
        lambda: cv2.TrackerMIL_create(),
        lambda: cv2.legacy.TrackerMIL_create(),
    ]
    for make in candidates:
        try:
            return make()
        except (AttributeError, cv2.error):
            continue
    return None


class _StaticHoldTracker:
    """Last-resort fallback when no cv2 tracker backend is available at
    all (e.g. opencv-contrib not installed on the board). Just holds the
    last known box in place rather than crashing. Much cruder than real
    tracking, but still bridges single-frame detector misses."""

    def __init__(self):
        self._box = None

    def init(self, frame, box):
        self._box = tuple(int(v) for v in box)
        return True

    def update(self, frame):
        if self._box is None:
            return False, (0, 0, 0, 0)
        return True, self._box


_tracker_backend_name_printed = False


class Track:
    """One tracked person: a box + a CSRT tracker + a miss counter."""

    _next_id = 1

    def __init__(self, frame, box):
        self.id = Track._next_id
        Track._next_id += 1
        self.box = tuple(int(v) for v in box)
        self.missed_frames = 0
        self.age = 0
        self.tracker = self._make_tracker(frame, self.box)

    def _make_tracker(self, frame, box):
        global _tracker_backend_name_printed
        tracker = _create_cv2_tracker()
        if tracker is None:
            if not _tracker_backend_name_printed:
                print("[fused_tracker] WARNING: no cv2 object-tracker backend "
                      "available on this OpenCV build (opencv-contrib "
                      "missing?). Falling back to a static-hold tracker — "
                      "boxes will freeze in place during missed-detection "
                      "frames instead of following motion. "
                      "Install 'opencv-contrib-python' for real tracking.")
                _tracker_backend_name_printed = True
            tracker = _StaticHoldTracker()
        tracker.init(frame, box)
        return tracker

    def update_with_detection(self, box):
        self.box = tuple(int(v) for v in box)
        self.missed_frames = 0
        # Re-seed the tracker on every confirmed detection so it doesn't
        # slowly drift away from the real person over a long track.
        # (init() is cheap relative to detection cost.)

    def reseed(self, frame):
        self.tracker = self._make_tracker(frame, self.box)

    def update_with_tracker(self, frame):
        ok, box = self.tracker.update(frame)
        if ok:
            self.box = tuple(int(v) for v in box)
        return ok


class FusedTrackedDetector:
    """
    Wraps a base detector (HogDetector or MobileNetSsdDetector) and adds
    cascade-cue fusion + persistent tracking. Drop-in replacement:
    exposes the same .detect(frame) -> list[(x, y, w, h)] interface.
    """

    def __init__(self, base_detector, cfg=None):
        self.base_detector = base_detector
        self.cue_detector = CascadeCueDetector(cfg)

        def _get(section, key, fallback, cast=float):
            if cfg is None:
                return fallback
            try:
                if cast is float:
                    return cfg.getfloat(section, key, fallback=fallback)
                if cast is int:
                    return cfg.getint(section, key, fallback=fallback)
            except Exception:
                return fallback
            return fallback

        # How many consecutive frames a track is allowed to survive with
        # NO detector support at all (full-body OR face OR upper-body),
        # relying purely on the visual tracker, before being dropped.
        self.max_missed_frames = int(_get("fusion", "max_missed_frames", 15, int))
        # IoU threshold to say "this detection belongs to this track"
        self.match_iou_threshold = _get("fusion", "match_iou_threshold", 0.25)
        # IoU threshold to merge overlapping cues (face + full-body on
        # the same person) into a single detection before matching.
        self.merge_iou_threshold = _get("fusion", "merge_iou_threshold", 0.3)
        # Re-init the CSRT tracker on every confirmed detection so it
        # doesn't drift during long missed-detection stretches.
        self.reseed_on_detection = True

        # --- Performance controls ---
        # Running HOG + 3 Haar cascades on every single frame at full
        # resolution is what was tanking FPS. Two levers to fix that:
        #
        # 1. detect_every_n_frames: only run the (expensive) detectors on
        #    every Nth frame. On the frames in between, tracks are simply
        #    advanced by their (cheap) visual tracker. Persons already
        #    being tracked don't need re-detection every single frame —
        #    only newly-appearing persons do, and those can wait a few
        #    frames to be picked up.
        self.detect_every_n_frames = int(_get("fusion", "detect_every_n_frames", 3, int))
        # 2. detection_scale: downscale the frame before feeding it to
        #    HOG/cascades (both scale roughly with pixel count), then
        #    scale detected boxes back up to full resolution. Tracking
        #    still runs on the full-res frame for accuracy.
        self.detection_scale = _get("fusion", "detection_scale", 0.5)

        self._frame_counter = 0
        self.tracks = []

    # ------------------------------------------------------------------ #

    def _merge_boxes(self, boxes):
        """Greedy merge of overlapping boxes from different cues so one
        person doesn't produce 2-3 overlapping detections."""
        merged = []
        used = [False] * len(boxes)
        for i, box_i in enumerate(boxes):
            if used[i]:
                continue
            group = [box_i]
            used[i] = True
            for j in range(i + 1, len(boxes)):
                if used[j]:
                    continue
                if _iou(box_i, boxes[j]) > self.merge_iou_threshold:
                    group.append(boxes[j])
                    used[j] = True
            xs = [b[0] for b in group]
            ys = [b[1] for b in group]
            xe = [b[0] + b[2] for b in group]
            ye = [b[1] + b[3] for b in group]
            merged.append((min(xs), min(ys), max(xe) - min(xs), max(ye) - min(ys)))
        return merged

    def _best_match(self, box, candidates):
        """Returns (index, iou) of the candidate box with highest IoU
        overlap against `box`, or (-1, 0.0) if nothing overlaps enough."""
        best_iou, best_idx = 0.0, -1
        for idx, cand in enumerate(candidates):
            iou = _iou(box, cand)
            if iou > best_iou:
                best_iou, best_idx = iou, idx
        return best_idx, best_iou

    def detect(self, frame):
        self._frame_counter += 1
        run_full_detection = ((self._frame_counter - 1) % max(1, self.detect_every_n_frames) == 0)

        full_body_boxes = []
        cue_boxes = []

        if run_full_detection:
            scale = self.detection_scale
            if scale < 1.0:
                small = cv2.resize(frame, None, fx=scale, fy=scale,
                                    interpolation=cv2.INTER_LINEAR)
            else:
                small = frame

            gray_small = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)

            raw_full_body = list(self.base_detector.detect(small))
            raw_cues = [box for (_, box) in self.cue_detector.detect(gray_small)]

            inv = 1.0 / scale if scale > 0 else 1.0

            def _upscale(b):
                x, y, w, h = b
                return (int(x * inv), int(y * inv), int(w * inv), int(h * inv))

            full_body_boxes = [_upscale(b) for b in raw_full_body]
            cue_boxes = [_upscale(b) for b in raw_cues]

        # --- Match full-body detections to existing tracks first. ---
        # Cues (face/upper-body) are only allowed to CONFIRM/refresh an
        # already-existing track (keeping it alive across a frame where
        # the full-body detector missed) — they are never allowed to
        # spawn a brand new track by themselves. That's what was causing
        # a hand or stray patch triggering the face/upper-body cascade to
        # get counted as a whole separate person.
        remaining_full_body = list(full_body_boxes)
        remaining_cues = list(cue_boxes)

        for track in self.tracks:
            fb_idx, fb_iou = self._best_match(track.box, remaining_full_body)
            if fb_idx != -1 and fb_iou >= self.match_iou_threshold:
                det = remaining_full_body.pop(fb_idx)
                track.update_with_detection(det)
                if self.reseed_on_detection:
                    track.reseed(frame)
                # A full-body hit also "uses up" any cue that overlaps it,
                # so that cue doesn't later get merged into a new track.
                cue_idx, cue_iou = self._best_match(det, remaining_cues)
                if cue_idx != -1 and cue_iou >= self.merge_iou_threshold:
                    remaining_cues.pop(cue_idx)
                track.age += 1
                continue

            cue_idx, cue_iou = self._best_match(track.box, remaining_cues)
            if cue_idx != -1 and cue_iou >= self.match_iou_threshold:
                det = remaining_cues.pop(cue_idx)
                track.update_with_detection(det)
                if self.reseed_on_detection:
                    track.reseed(frame)
                track.age += 1
                continue

            # No detector support this frame (or detection was skipped
            # entirely this frame) — lean on the tracker to bridge the
            # gap instead of dropping the person.
            ok = track.update_with_tracker(frame)
            if ok:
                track.missed_frames += 1
            else:
                track.missed_frames = self.max_missed_frames + 1
            track.age += 1

        # Drop tracks that have gone unsupported for too long.
        self.tracks = [t for t in self.tracks if t.missed_frames <= self.max_missed_frames]

        # New tracks are only ever started from full-body detections —
        # never from a bare face/upper-body cue on its own.
        for det in remaining_full_body:
            self.tracks.append(Track(frame, det))

        return [t.box for t in self.tracks]
