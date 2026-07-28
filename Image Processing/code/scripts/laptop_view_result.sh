#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# laptop_view_result.sh
#
# Run this ON THE LAPTOP to watch the processed (annotated) video coming
# back from the Orange Pi. Opens the live-refreshing page in your default
# web browser.
#
# If you're viewing the laptop's own screen remotely through VNC, just run
# this on the laptop as usual — the browser window will appear on the
# laptop's desktop and show up inside your VNC session like any other
# window.
#
# Usage:
#   ./laptop_view_result.sh <orange_pi_ip> [port]
#
# Example:
#   ./laptop_view_result.sh 192.168.0.170 8090
# ---------------------------------------------------------------------------
set -euo pipefail

PI_IP="${1:?Usage: ./laptop_view_result.sh <orange_pi_ip> [port]}"
PORT="${2:-8090}"
URL="http://${PI_IP}:${PORT}/"

echo "Opening $URL ..."

if command -v xdg-open >/dev/null 2>&1; then
  xdg-open "$URL"
elif command -v open >/dev/null 2>&1; then
  open "$URL"
else
  echo "Could not auto-detect a browser opener. Open this URL manually:"
  echo "  $URL"
fi
