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

Usage:
    python3 laptop_camera_server.py --device 0 --width 320 --height 240 --port 8080
"""
import argparse
import socket
import threading
import time

import cv2
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def make_handler(get_latest_jpeg):
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
                self.send_response(503)
                self.end_headers()
                return

            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.send_header("Pragma", "no-cache")
            self.end_headers()
            self.wfile.write(data)

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="0",
                         help="Camera index (e.g. 0) or /dev/videoN path")
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--fps", type=int, default=15)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--quality", type=int, default=80, help="JPEG quality 1-100")
    args = parser.parse_args()

    device = int(args.device) if args.device.isdigit() else args.device
    # Force V4L2 explicitly — letting OpenCV auto-pick a backend can land
    # on GStreamer, which on some systems fights with setting FOURCC/
    # resolution together and fails to start the pipeline.
    if isinstance(device, int):
        cap = cv2.VideoCapture(device, cv2.CAP_V4L2)
    else:
        cap = cv2.VideoCapture(device, cv2.CAP_V4L2)
    if not cap.isOpened():
        raise SystemExit(f"[laptop_camera_server] Could not open camera: {device}")

    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    cap.set(cv2.CAP_PROP_FPS, args.fps)

    # Confirm the settings actually took, since some drivers silently
    # ignore unsupported combinations instead of failing outright.
    actual_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if (actual_w, actual_h) != (args.width, args.height):
        print(f"[laptop_camera_server] Warning: camera gave {actual_w}x{actual_h}, "
              f"not the requested {args.width}x{args.height}")

    latest = {"jpeg": None}
    lock = threading.Lock()
    encode_params = [int(cv2.IMWRITE_JPEG_QUALITY), args.quality]

    def capture_loop():
        while True:
            ok, frame = cap.read()
            if not ok:
                time.sleep(0.05)
                continue
            ok, buf = cv2.imencode(".jpg", frame, encode_params)
            if ok:
                with lock:
                    latest["jpeg"] = buf.tobytes()

    def get_latest_jpeg():
        with lock:
            return latest["jpeg"]

    t = threading.Thread(target=capture_loop, daemon=True)
    t.start()

    # Wait briefly for the first frame so early requests don't 503.
    for _ in range(50):
        if get_latest_jpeg() is not None:
            break
        time.sleep(0.1)

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
    except Exception:
        ip = "<this-laptop-ip>"

    handler = make_handler(get_latest_jpeg)
    server = ThreadingHTTPServer(("0.0.0.0", args.port), handler)

    print(f"[laptop_camera_server] Capturing {device} at {args.width}x{args.height}")
    print(f"[laptop_camera_server] Orange Pi should poll: http://{ip}:{args.port}/latest.jpg")
    print("[laptop_camera_server] Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()


if __name__ == "__main__":
    main()
