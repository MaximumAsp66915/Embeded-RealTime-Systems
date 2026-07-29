#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# pi_stream_result.sh
#
# Run this ON THE ORANGE PI, alongside person_detector.py. Serves the
# annotated /dev/shm/surveillance/frame.jpg as a simple auto-refreshing
# web page the laptop can open in any browser.
#
# Earlier versions tried to build a real MJPEG video stream (via a Python
# multipart server, then via ffmpeg + FIFO). Both kept freezing on the
# first frame because FFmpeg's MJPEG demuxer is finicky about live/piped
# MJPEG sources. This version sidesteps all of that: it's just a plain
# HTTP server that returns the current frame.jpg bytes on every request,
# refreshed client-side by JavaScript every ~150ms. No video codec
# involved, so nothing to freeze.
#
# Usage:
#   ./pi_stream_result.sh [frame_path] [port]
#
# Example:
#   ./pi_stream_result.sh /dev/shm/surveillance/frame.jpg 8090
# ---------------------------------------------------------------------------
set -euo pipefail

FRAME_PATH="${1:-/dev/shm/surveillance/frame.jpg}"
PORT="${2:-8090}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"

echo "==============================================================="
echo " Serving $FRAME_PATH as a live-refreshing page on port $PORT"
echo " On the laptop, open in a browser (or run laptop_view_result.sh):"
echo ""
echo "     http://${PI_IP:-<THIS_PI_IP>}:${PORT}/"
echo ""
echo " Press Ctrl+C to stop."
echo "==============================================================="

exec python3 "$SCRIPT_DIR/frame_viewer_server.py" --frame "$FRAME_PATH" --port "$PORT"
