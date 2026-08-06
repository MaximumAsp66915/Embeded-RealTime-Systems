"""
experiments/exp4_3_watchdog.py — Step 6, Experiment 4-3

"Disconnect camera -> video + screenshots of watchdog reacting"

Record your own video of physically disconnecting the camera (or, for
this project's http_poll setup, killing laptop_stream_webcam.sh — same
effect: person_detector.py stops getting new frames). This script runs
alongside that recording and gives you the evidence trail: exactly when
persons.json went stale, when the watchdog's restart actually happened
(via surveillance-imgproc's own journalctl logs, pulled through the
bonus /api/v1/services/{name}/logs endpoint), and when fresh frames
resumed after the restart.

Usage:
    python3 exp4_3_watchdog.py --host 192.168.0.170 --duration-s 180

Run this BEFORE you disconnect the camera, leave it running through the
disconnect + the ~30s watchdog timeout + the restart + reconnecting the
camera, then let it finish on its own.
"""
import argparse
import csv
import time
from pathlib import Path

import requests
from requests.packages.urllib3.exceptions import InsecureRequestWarning
import warnings

warnings.simplefilter("ignore", InsecureRequestWarning)


def get_json(url):
    try:
        resp = requests.get(url, verify=False, timeout=3)
        if resp.status_code == 200:
            return resp.json()
    except requests.RequestException:
        pass
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--poll-interval-s", type=float, default=1.0)
    parser.add_argument("--duration-s", type=int, default=180)
    parser.add_argument("--results-dir", default="results/4-3")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)
    base = f"https://{args.host}:{args.port}"

    rows = []
    start = time.time()
    last_timestamp = None
    stale_since = None

    print(f"[4-3] Monitoring persons.json for {args.duration_s}s. "
          f"Disconnect the camera whenever you're ready to record.")

    while time.time() - start < args.duration_s:
        elapsed = time.time() - start
        data = get_json(f"{base}/api/v1/persons")
        ts = data.get("timestamp") if data else None

        event = ""
        if ts and ts != last_timestamp:
            if stale_since is not None:
                event = "RECOVERED - fresh frame after being stale"
                print(f"  [{elapsed:7.1f}s] {event} (was stale for ~{elapsed - stale_since:.0f}s)")
                stale_since = None
            last_timestamp = ts
        elif ts and ts == last_timestamp and stale_since is None:
            stale_since = elapsed  # timestamp hasn't moved since last cycle — start tracking staleness

        rows.append({"elapsed_s": round(elapsed, 2), "timestamp": ts or "", "event": event})
        time.sleep(args.poll_interval_s)

    with open(results_dir / "watchdog_log.csv", "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["elapsed_s", "timestamp", "event"])
        writer.writeheader()
        writer.writerows(rows)
    print(f"\n[4-3] wrote {len(rows)} samples -> {results_dir / 'watchdog_log.csv'}")

    # Pull the actual restart evidence from the source of truth: the
    # imgproc service's own journal, via the bonus REST endpoint.
    print("\n[4-3] Fetching surveillance-imgproc logs for restart evidence...")
    logs = get_json(f"{base}/api/v1/services/surveillance-imgproc/logs?lines=100")
    if logs:
        log_path = results_dir / "imgproc_journalctl.txt"
        with open(log_path, "w") as f:
            f.write(logs.get("logs", ""))
        print(f"[4-3] wrote -> {log_path}")
        print("[4-3] Look for a gap in timestamps / a fresh \"Started\" line — that's the restart. "
              "Also check the notifier's own logs (journalctl -u surveillance-notifier) for the "
              "\"[watchdog] no new frame for >Ns\" line that triggered it.")
    else:
        print("[4-3] Could not fetch imgproc logs via the API — pull them manually instead:")
        print("      journalctl -u surveillance-imgproc -n 100")


if __name__ == "__main__":
    main()
