#!/usr/bin/env python3
"""
laptop_camera_server.py

Runs ON THE LAPTOP. Captures the webcam directly with OpenCV (local
capture is reliable — no network involved on this side) and serves the
latest frame as a plain JPEG over HTTP:

    GET /latest.jpg -> current frame, JPEG bytes, no caching

This mirrors frame_viewer_server.py on the Pi side. We use plain HTTP
polling instead of a real MJPEG/video stream because both FFmpeg's own
MJPEG muxer/demuxer and a hand-written multipart server proved unreliable
over the network in this setup (freezing on the first frame). A simple
"here are the latest bytes, ask again in a bit" HTTP endpoint has no
protocol state to get stuck in.

CAMERA RETRY BEHAVIOR (this is the part that changed):
The HTTP server now starts immediately, regardless of whether a webcam is
plugged in yet. A separate background thread tries to open the camera and,
if that fails, keeps retrying every --retry-interval seconds (default 5)
instead of the whole script exiting. The same retry kicks in if the camera
disconnects mid-run (e.g. someone unplugs the USB webcam) — a run of
consecutive read failures triggers a release + reopen attempt.

While no camera is available, GET /latest.jpg returns 503 with a small
JSON body explaining why, so the Orange Pi side gets a clear, immediate
answer instead of a hung connection or a dead port.

Usage:
    python3 laptop_camera_server.py --device 0 --width 320 --height 240 --port 8080
    python3 laptop_camera_server.py --device 0 --retry-interval 5
"""
import argparse
import socket
import threading
import time

import cv2
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# How many consecutive failed cap.read() calls we tolerate before assuming
# the camera dropped and attempting a full reopen instead of just retrying
# the read.
CONSECUTIVE_READ_FAILURES_BEFORE_REOPEN = 30


def make_handler(get_latest_jpeg, get_camera_status):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def do_GET(self):
            if not self.path.startswith("/latest.jpg"):
                self.send_response(404)
                self.end_headers()
                return

            data = get_latest_jpeg()
            if data is None:
                status = get_camera_status()
                body = ('{"error": "camera not ready", "status": "%s"}' % status).encode()
                self.send_response(503)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Retry-After", "2")
                self.end_headers()
                self.wfile.write(body)
                return

            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.send_header("Pragma", "no-cache")
            self.end_headers()
            self.wfile.write(data)

    return Handler


def open_camera(device, width, height, fps):
    """Single attempt to open + configure the camera. Returns the
    VideoCapture on success, or None on failure. Never raises."""
    try:
        cap = cv2.VideoCapture(device, cv2.CAP_V4L2)
        if not cap.isOpened():
            cap.release()
            return None

        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        cap.set(cv2.CAP_PROP_FPS, fps)

        actual_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        actual_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        if (actual_w, actual_h) != (width, height):
            print(f"[laptop_camera_server] Warning: camera gave {actual_w}x{actual_h}, "
                  f"not the requested {width}x{height}")
        return cap
    except Exception as exc:
        print(f"[laptop_camera_server] open_camera raised: {exc}")
        return None


def capture_loop(device, width, height, fps, quality, retry_interval,
                  latest, lock, status):
    encode_params = [int(cv2.IMWRITE_JPEG_QUALITY), quality]
    cap = None
    attempt = 0

    while True:
        # --- (Re)connect phase ---
        if cap is None:
            attempt += 1
            status["value"] = f"connecting (attempt {attempt})"
            cap = open_camera(device, width, height, fps)
            if cap is None:
                print(f"[laptop_camera_server] Camera '{device}' not available "
                      f"(attempt {attempt}), retrying in {retry_interval}s...")
                with lock:
                    latest["jpeg"] = None
                time.sleep(retry_interval)
                continue
            else:
                print(f"[laptop_camera_server] Camera '{device}' opened successfully.")
                status["value"] = "streaming"
                attempt = 0

        # --- Read phase ---
        consecutive_failures = 0
        while True:
            ok, frame = cap.read()
            if not ok:
                consecutive_failures += 1
                if consecutive_failures >= CONSECUTIVE_READ_FAILURES_BEFORE_REOPEN:
                    print("[laptop_camera_server] Too many failed reads in a row — "
                          "assuming the camera disconnected. Reopening...")
                    cap.release()
                    cap = None
                    status["value"] = "disconnected, reconnecting"
                    with lock:
                        latest["jpeg"] = None
                    break  # back to (re)connect phase
                time.sleep(0.05)
                continue

            consecutive_failures = 0
            ok_enc, buf = cv2.imencode(".jpg", frame, encode_params)
            if ok_enc:
                with lock:
                    latest["jpeg"] = buf.tobytes()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="0",
                         help="Camera index (e.g. 0) or /dev/videoN path")
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--fps", type=int, default=15)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--quality", type=int, default=80, help="JPEG quality 1-100")
    parser.add_argument("--retry-interval", type=float, default=5.0,
                         help="Seconds to wait between camera (re)connect attempts")
    args = parser.parse_args()

    device = int(args.device) if args.device.isdigit() else args.device

    latest = {"jpeg": None}
    lock = threading.Lock()
    status = {"value": "starting"}

    def get_latest_jpeg():
        with lock:
            return latest["jpeg"]

    def get_camera_status():
        return status["value"]

    # Camera thread: handles its own retry/reconnect loop and never raises
    # out to main(), so the HTTP server below is unaffected either way.
    t = threading.Thread(
        target=capture_loop,
        args=(device, args.width, args.height, args.fps, args.quality,
              args.retry_interval, latest, lock, status),
        daemon=True,
    )
    t.start()

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
    except Exception:
        ip = "<this-laptop-ip>"

    handler = make_handler(get_latest_jpeg, get_camera_status)
    server = ThreadingHTTPServer(("0.0.0.0", args.port), handler)

    print(f"[laptop_camera_server] HTTP server listening on :{args.port} "
          f"(starts immediately, independent of camera state)")
    print(f"[laptop_camera_server] Orange Pi should poll: http://{ip}:{args.port}/latest.jpg")
    print(f"[laptop_camera_server] Waiting for camera '{device}', "
          f"retrying every {args.retry_interval}s until it's found.")
    print("[laptop_camera_server] Ctrl+C to stop.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
