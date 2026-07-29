#!/usr/bin/env python3
"""
person_detector.py

Smart Surveillance System — Step 2: Image Processing

Reads a camera, UDP stream, or video file, detects people in each frame,
draws bounding boxes + a live counter, overlays student ID / timestamp / FPS,
and publishes the result so the C code (web server, REST API, MQTT client,
email logic) can consume it without needing to know anything about OpenCV.

IPC mechanism:
    <shared_dir>/frame.jpg     -> latest annotated JPEG frame
    <shared_dir>/persons.json  -> {"count": int, "timestamp": str, "fps": float}

Both files are written atomically (write to a temp file, then os.rename)
so the C side never reads a half-written file.

Usage:
    python3 person_detector.py [--config config.ini]
"""

import argparse
import configparser
import json
import os
import sys
import time
from datetime import datetime

import cv2


# ------------------------------------------------------------------------- #
# Config loading
# ------------------------------------------------------------------------- #

def load_config(path: str) -> configparser.ConfigParser:
    cfg = configparser.ConfigParser()
    if not os.path.exists(path):
        sys.exit(f"[person_detector] Config file not found: {path}")
    cfg.read(path)
    return cfg


# ------------------------------------------------------------------------- #
# Detector backends
# ------------------------------------------------------------------------- #

class HogDetector:
    """OpenCV's built-in HOG + SVM pedestrian detector.
    Lightweight, CPU-only, no external model files needed."""

    def __init__(self):
        self.hog = cv2.HOGDescriptor()
        self.hog.setSVMDetector(cv2.HOGDescriptor_getDefaultPeopleDetector())

    def detect(self, frame):
        # winStride smaller than the (8,8) default trades CPU time for
        # more candidate detection windows, which reduces frame-to-frame
        # flicker on a low-power board. scale closer to 1.0 makes the
        # multi-scale pyramid finer (again: more candidates, steadier
        # detection, more CPU). hitThreshold slightly negative makes the
        # SVM a bit more permissive instead of only firing on very
        # confident matches.
        # Positional args on purpose: the "group/final threshold" parameter
        # is named differently across OpenCV versions/bindings
        # (finalThreshold vs groupThreshold), and older builds sometimes
        # don't expose keyword args for this overload at all. Order is
        # stable: img, hitThreshold, winStride, padding, scale,
        # groupThreshold/finalThreshold, useMeanshiftGrouping.
        boxes, weights = self.hog.detectMultiScale(
            frame, 0, (4, 4), (8, 8), 1.03, 1, False
        )
        return [tuple(b) for b in boxes]


class MobileNetSsdDetector:
    """Heavier, more accurate alternative. Requires model files on disk."""

    PERSON_CLASS_ID = 15  # index of "person" in standard MobileNet-SSD labels

    def __init__(self, prototxt: str, model: str, confidence: float):
        if not (os.path.exists(prototxt) and os.path.exists(model)):
            sys.exit(
                "[person_detector] mobilenet_ssd backend selected but model "
                f"files are missing:\n  {prototxt}\n  {model}"
            )
        self.net = cv2.dnn.readNetFromCaffe(prototxt, model)
        self.confidence = confidence

    def detect(self, frame):
        h, w = frame.shape[:2]
        blob = cv2.dnn.blobFromImage(
            cv2.resize(frame, (300, 300)), 0.007843, (300, 300), 127.5
        )
        self.net.setInput(blob)
        detections = self.net.forward()

        boxes = []
        for i in range(detections.shape[2]):
            conf = detections[0, 0, i, 2]
            class_id = int(detections[0, 0, i, 1])
            if conf > self.confidence and class_id == self.PERSON_CLASS_ID:
                box = detections[0, 0, i, 3:7] * [w, h, w, h]
                (x1, y1, x2, y2) = box.astype("int")
                boxes.append((x1, y1, x2 - x1, y2 - y1))
        return boxes


def build_detector(cfg: configparser.ConfigParser):
    backend = cfg.get("detector", "backend", fallback="hog")
    if backend == "hog":
        return HogDetector()
    elif backend == "mobilenet_ssd":
        return MobileNetSsdDetector(
            cfg.get("detector", "mobilenet_prototxt"),
            cfg.get("detector", "mobilenet_model"),
            cfg.getfloat("detector", "mobilenet_confidence", fallback=0.5),
        )
    else:
        sys.exit(f"[person_detector] Unknown detector backend: {backend}")


# ------------------------------------------------------------------------- #
# Atomic IPC writers
# ------------------------------------------------------------------------- #

class SharedOutput:
    """Writes the annotated frame + person count atomically so consumers
    never see a partially-written file."""

    def __init__(self, shared_dir: str, frame_filename: str,
                 persons_filename: str, jpeg_quality: int):
        os.makedirs(shared_dir, exist_ok=True)
        self.frame_path = os.path.join(shared_dir, frame_filename)
        self.persons_path = os.path.join(shared_dir, persons_filename)
        self.jpeg_quality = jpeg_quality

    def _atomic_write_bytes(self, path: str, data: bytes):
        tmp_path = path + ".tmp"
        with open(tmp_path, "wb") as f:
            f.write(data)
        os.replace(tmp_path, path)  # atomic on POSIX

    def write_frame(self, frame):
        ok, buf = cv2.imencode(
            ".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), self.jpeg_quality]
        )
        if ok:
            self._atomic_write_bytes(self.frame_path, buf.tobytes())

    def write_persons(self, count: int, fps: float):
        payload = {
            "count": count,
            "timestamp": datetime.now().isoformat(timespec="seconds"),
            "fps": round(fps, 2),
        }
        data = json.dumps(payload).encode("utf-8")
        self._atomic_write_bytes(self.persons_path, data)


# ------------------------------------------------------------------------- #
# Main loop & Video Capture
# ------------------------------------------------------------------------- #

class HttpPollCapture:
    """
    Drop-in replacement for cv2.VideoCapture that fetches a single JPEG
    over HTTP on every read() call instead of opening a network video
    stream. Used because cv2.VideoCapture(url, cv2.CAP_FFMPEG) reliably
    froze on the first frame with live network MJPEG sources in this
    setup (a known FFmpeg-MJPEG-demuxer limitation) — this sidesteps the
    video codec path entirely: every .read() is just a fresh HTTP GET.
    """

    def __init__(self, url: str, timeout: float = 2.0):
        import urllib.request
        self._urllib = urllib.request
        self.url = url
        self.timeout = timeout
        self._opened = False
        # Confirm the endpoint is reachable before declaring success.
        try:
            with self._urllib.urlopen(url, timeout=timeout) as resp:
                self._opened = resp.status == 200
        except Exception as exc:
            print(f"[person_detector] Initial HTTP poll to {url} failed: {exc}")
            self._opened = False

    def isOpened(self):
        return self._opened

    def read(self):
        import numpy as np
        try:
            with self._urllib.urlopen(self.url, timeout=self.timeout) as resp:
                data = resp.read()
            arr = np.frombuffer(data, dtype=np.uint8)
            frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            if frame is None:
                return False, None
            return True, frame
        except Exception as exc:
            print(f"[person_detector] HTTP poll failed: {exc}")
            return False, None

    def release(self):
        pass


def open_capture(cfg: configparser.ConfigParser):
    source_type = cfg.get("camera", "source_type", fallback="udp")

    if source_type == "video":
        path = cfg.get("camera", "video_path")
        cap = cv2.VideoCapture(path)
        if not cap.isOpened():
            sys.exit(f"[person_detector] Could not open video file: {path}")
            
    elif source_type == "camera":
        index = cfg.getint("camera", "camera_index", fallback=0)
        cap = cv2.VideoCapture(index)
        if not cap.isOpened():
            sys.exit(f"[person_detector] Could not open camera index: {index}")
        width = cfg.getint("camera", "width", fallback=640)
        height = cfg.getint("camera", "height", fallback=480)
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)

    elif source_type == "http_poll":
        # Plain HTTP JPEG polling — see HttpPollCapture above. Point
        # http_url at a laptop_camera_server.py instance's /latest.jpg.
        default_url = "http://0.0.0.0:8080/latest.jpg"
        http_url = cfg.get("camera", "http_url", fallback=default_url)
        print(f"[person_detector] Polling frames over HTTP from: {http_url}")
        cap = HttpPollCapture(http_url)
        if not cap.isOpened():
            sys.exit(f"[person_detector] Could not reach {http_url}. Ensure laptop_stream_webcam.sh is running.")

    else:
        # Default or fallback to UDP stream (Method A)
        default_udp = "udp://0.0.0.0:5000?fifo_size=1000000&overrun_nonfatal=1"
        udp_url = cfg.get("camera", "udp_url", fallback=default_udp)
        print(f"[person_detector] Opening UDP stream using FFmpeg backend: {udp_url}")
        
        cap = cv2.VideoCapture(udp_url, cv2.CAP_FFMPEG)
        if not cap.isOpened():
            sys.exit(f"[person_detector] Could not open UDP stream on {udp_url}. Ensure laptop is streaming.")
            
    return cap


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="config.ini")
    args = parser.parse_args()

    cfg = load_config(args.config)
    student_id = cfg.get("general", "student_id", fallback="UNKNOWN_ID")
    show_window = cfg.getboolean("display", "show_window", fallback=False)

    cap = open_capture(cfg)
    detector = build_detector(cfg)
    output = SharedOutput(
        cfg.get("output", "shared_dir", fallback="/dev/shm/surveillance"),
        cfg.get("output", "frame_filename", fallback="frame.jpg"),
        cfg.get("output", "persons_filename", fallback="persons.json"),
        cfg.getint("output", "jpeg_quality", fallback=80),
    )

    print(f"[person_detector] Started. Student ID={student_id}, "
          f"backend={cfg.get('detector', 'backend')}, "
          f"shared_dir={cfg.get('output', 'shared_dir')}")

    prev_time = time.time()
    fps = 0.0
    fps_alpha = 0.9

    # --- Detection smoothing state ---
    # HOG has no memory across frames — a single frame where the pose,
    # lighting, or motion blur throws it off will report zero detections
    # even though the person never left. Holding the last-known boxes for
    # a short grace period (rather than instantly zeroing out) removes
    # most of that flicker without pretending we detected something we
    # didn't for more than a fraction of a second.
    HOLD_FRAMES = 5  # ~0.5s at ~10fps; tune to taste
    held_boxes = []
    frames_since_detection = HOLD_FRAMES + 1

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("[person_detector] Frame read failed / stream interrupted. Retrying...")
                time.sleep(0.05)
                continue

            boxes = detector.detect(frame)

            if boxes:
                held_boxes = boxes
                frames_since_detection = 0
            else:
                frames_since_detection += 1

            if frames_since_detection <= HOLD_FRAMES:
                display_boxes = held_boxes
            else:
                display_boxes = []

            count = len(display_boxes)

            for (x, y, w, h) in display_boxes:
                cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 255, 0), 2)

            # --- FPS calculation ---
            now = time.time()
            instant_fps = 1.0 / max(now - prev_time, 1e-6)
            fps = fps_alpha * fps + (1 - fps_alpha) * instant_fps
            prev_time = now

            # --- Overlays ---
            timestamp_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            overlay_lines = [
                f"ID: {student_id}",
                f"{timestamp_str}",
                f"Persons: {count}",
                f"FPS: {fps:.1f}",
            ]
            y0 = 20
            for i, line in enumerate(overlay_lines):
                y = y0 + i * 20
                cv2.putText(frame, line, (10, y), cv2.FONT_HERSHEY_SIMPLEX,
                            0.6, (0, 0, 0), 3, cv2.LINE_AA)   # outline
                cv2.putText(frame, line, (10, y), cv2.FONT_HERSHEY_SIMPLEX,
                            0.6, (0, 255, 255), 1, cv2.LINE_AA)  # fill

            # --- Publish for the C side ---
            output.write_frame(frame)
            output.write_persons(count, fps)

            if show_window:
                cv2.imshow("Smart Surveillance - Person Detection", frame)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break

    except KeyboardInterrupt:
        print("\n[person_detector] Interrupted, shutting down.")
    finally:
        cap.release()
        if show_window:
            cv2.destroyAllWindows()


if __name__ == "__main__":
    main()