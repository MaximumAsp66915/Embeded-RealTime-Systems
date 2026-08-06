"""
experiments/exp4_2_blackbox.py — Step 6, Experiment 4-2

"Black box -> screenshot of stored DB events"

Two ways to get that screenshot, both covered here:

1. Through the REST API (works from any machine on the network):
     python3 exp4_2_blackbox.py --host 192.168.0.170

2. Directly against the SQLite file (run this ON THE PI, useful as a
   second, independent piece of evidence that the REST API isn't just
   making the numbers up):
     sqlite3 /opt/surveillance/notifier/blackbox.db \\
       "SELECT * FROM events ORDER BY id DESC LIMIT 20;"
     sqlite3 /opt/surveillance/notifier/blackbox.db \\
       "SELECT value FROM meta WHERE key='total_events_ever';"

This script does #1, and pretty-prints both the total count and the
recent events as a table — screenshot the terminal output directly, or
open results/4-2/events.csv in a spreadsheet for a cleaner table image.
"""
import argparse
import csv
import json
from pathlib import Path

import requests
from requests.packages.urllib3.exceptions import InsecureRequestWarning
import warnings

warnings.simplefilter("ignore", InsecureRequestWarning)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, help="The Step 4 web server's host (not the notifier)")
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--limit", type=int, default=20, help="How many recent events to show (max 200)")
    parser.add_argument("--results-dir", default="results/4-2")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)

    base = f"https://{args.host}:{args.port}"

    count_resp = requests.get(f"{base}/api/v1/blackbox/count", verify=False, timeout=5)
    events_resp = requests.get(f"{base}/api/v1/blackbox/events?limit={args.limit}", verify=False, timeout=5)

    if count_resp.status_code != 200 or events_resp.status_code != 200:
        raise SystemExit(
            f"[4-2] API returned non-200 (count={count_resp.status_code}, events={events_resp.status_code}) "
            f"— is the black box DB available yet? It's created the first time the Step 5/6 notifier "
            f"daemon runs, not before."
        )

    total = count_resp.json()["total_events"]
    events = events_resp.json()

    print(f"=== Black box: total_events_ever = {total} ===\n")
    print(f"{'id':>4}  {'type':<18} {'timestamp':<21} {'count':>5}  {'temp_c':>6}  detail")
    print("-" * 90)
    for e in events:
        print(f"{e['id']:>4}  {e['event_type']:<18} {e['timestamp']:<21} "
              f"{e['person_count']:>5}  {e['cpu_temp_c']:>6.1f}  {e['detail']}")

    csv_path = results_dir / "events.csv"
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["id", "event_type", "timestamp", "person_count", "cpu_temp_c", "detail"])
        writer.writeheader()
        writer.writerows(events)

    summary_path = results_dir / "summary.json"
    with open(summary_path, "w") as f:
        json.dump({"total_events_ever": total, "recent_events_shown": len(events)}, f, indent=2)

    print(f"\n[4-2] wrote {len(events)} events -> {csv_path}")
    print(f"[4-2] wrote summary -> {summary_path}")


if __name__ == "__main__":
    main()
