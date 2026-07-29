#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# laptop_stream_webcam.sh
#
# Run this ON THE LAPTOP. Captures the webcam locally with OpenCV (reliable
# — no network protocol involved on this side) and serves the latest frame
# as a plain JPEG over HTTP for the Orange Pi to poll.
#
# Earlier versions used ffmpeg to push an HTTP MJPEG/mpjpeg video stream,
# which cv2.VideoCapture on the Pi kept freezing on after the first frame
# (FFmpeg's MJPEG demuxer is unreliable with live network MJPEG sources —
# we hit the same failure mode on the Pi->laptop leg and fixed it there
# with plain HTTP polling instead of a real video stream). This script
# applies the same fix here: no ffmpeg, no video codec, just "here are the
# latest bytes."
#
# Usage:
#   ./laptop_stream_webcam.sh [device] [width] [height] [port]
#
# Example:
#   ./laptop_stream_webcam.sh 0 320 240 8080
#   (device can be a camera index like 0, or a path like /dev/video0)
# ---------------------------------------------------------------------------
set -euo pipefail

DEVICE="${1:-0}"
WIDTH="${2:-320}"
HEIGHT="${3:-240}"
PORT="${4:-8080}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! python3 -c "import cv2" >/dev/null 2>&1; then
  echo "python3-opencv (cv2) not found. Install it with:" >&2
  echo "  pip install opencv-python   (or: sudo apt install python3-opencv)" >&2
  exit 1
fi

LAPTOP_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"

echo "==============================================================="
echo " Serving $DEVICE at ${WIDTH}x${HEIGHT} as plain HTTP JPEG polling"
echo " Orange Pi should poll:"
echo ""
echo "     http://${LAPTOP_IP:-<THIS_LAPTOP_IP>}:${PORT}/latest.jpg"
echo ""
echo " Press Ctrl+C to stop."
echo "==============================================================="

exec python3 "$SCRIPT_DIR/laptop_camera_server.py" \
  --device "$DEVICE" --width "$WIDTH" --height "$HEIGHT" --port "$PORT"
