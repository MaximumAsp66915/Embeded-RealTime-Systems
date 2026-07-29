#!/usr/bin/env python3
"""
frame_viewer_server.py

Runs ON THE ORANGE PI. Serves the annotated frame.jpg that
person_detector.py writes to /dev/shm/surveillance/frame.jpg as:

  - GET /latest.jpg  -> the raw current JPEG bytes (no caching)
  - GET /            -> an HTML page that auto-refreshes the image via JS

This deliberately avoids MJPEG muxers/demuxers entirely (no ffmpeg
involved on this leg) because they kept freezing on the first frame —
FFmpeg's MJPEG demuxer doesn't reliably parse hand-written or even
some ffmpeg-piped live multipart streams over an unstable chain of
probing/buffering. A plain "fetch the latest image on a timer" approach
has none of those failure modes: every request just reads whatever
bytes are currently on disk.

Usage:
    python3 frame_viewer_server.py --frame /dev/shm/surveillance/frame.jpg --port 8090

Then on the laptop, open in a normal web browser (Chrome/Firefox):
    http://<ORANGE_PI_IP>:8090/
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PAGE = b"""<!doctype html>
<html>
<head><title>Live detection feed</title>
<style>
  body { background:#111; margin:0; display:flex; align-items:center; justify-content:center; height:100vh; }
  img { max-width:100%; max-height:100%; }
</style>
</head>
<body>
  <img id="f" src="/latest.jpg">
  <script>
    const img = document.getElementById('f');
    setInterval(() => { img.src = '/latest.jpg?t=' + Date.now(); }, 150);
  </script>
</body>
</html>
"""


def make_handler(frame_path: str):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def do_GET(self):
            if self.path == "/" or self.path.startswith("/index"):
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(PAGE)))
                self.end_headers()
                self.wfile.write(PAGE)
                return

            if self.path.startswith("/latest.jpg"):
                try:
                    with open(frame_path, "rb") as f:
                        data = f.read()
                except FileNotFoundError:
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
                return

            self.send_response(404)
            self.end_headers()

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--frame", default="/dev/shm/surveillance/frame.jpg")
    parser.add_argument("--port", type=int, default=8090)
    args = parser.parse_args()

    handler = make_handler(args.frame)
    server = ThreadingHTTPServer(("0.0.0.0", args.port), handler)

    print(f"[frame_viewer_server] Serving {args.frame}")
    print(f"[frame_viewer_server] Open http://<this-pi-ip>:{args.port}/ in a browser")
    print("[frame_viewer_server] Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
