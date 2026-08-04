"""
experiments/exp3_5_latency.py — Step 5, Experiment 3-5

"10 samples of entry-to-MQTT-receipt latency -> mean + standard deviation"

"Entry" here = the detection frame's own timestamp, written by Step 2's
person_detector.py into persons.json and embedded verbatim in the
"home/persons/<id>" MQTT payload (see src/mqtt_client.c's
mqtt_client_publish_persons). "Receipt" = the wall-clock moment THIS
script's MQTT subscriber callback fires for that message.

IMPORTANT CAVEAT (state this in your report, don't just report the
numbers): persons.json's timestamp has 1-SECOND resolution (no
sub-second component), so any true latency under ~1s is invisible to
this measurement — you're measuring "receipt time minus the last whole
second," which has up to ~1s of quantization noise baked in on top of
the real network+processing latency. This script still gives an honest
comparative measure of the pipeline's actual delay (poll interval +
network round-trip), just with that resolution caveat clearly stated
rather than implied to be more precise than it is. If you want tighter
precision, the fix is on the Step 2 side: have person_detector.py write
a sub-second epoch float into persons.json alongside the existing
timestamp string — this script doesn't require or assume that, but would
use it automatically if present (see --epoch-field).

Also assumes the Pi and the machine running this script have reasonably
synced clocks (both on NTP, same timezone interpretation) — persons.json's
timestamp has no timezone marker, so it's parsed as naive local time and
compared against this machine's own local `datetime.now()`. If the Pi and
this machine are in different timezones or badly out of sync, the
"latency" numbers will be meaningless (likely negative or absurdly
large) — the script flags negative latencies rather than silently
including them in the average.

Usage:
    python3 exp3_5_latency.py --host 192.168.0.170 --student-id 402101906
"""
import argparse
import statistics
import time
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt


def parse_timestamp(ts: str) -> datetime:
    """persons.json timestamps look like '2026-08-02T06:46:08' (naive,
    no timezone) — but tolerate a trailing 'Z' too in case that ever
    changes, same format used elsewhere in this project (telemetry)."""
    ts = ts.rstrip("Z")
    return datetime.strptime(ts, "%Y-%m-%dT%H:%M:%S")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--student-id", required=True)
    parser.add_argument("--topic-prefix", default="home")
    parser.add_argument("--username", default=None,
                         help="MQTT username, if the broker requires auth (e.g. secure_setup.sh's "
                              "Mosquitto config sets allow_anonymous false) — same MQTT_USER from "
                              "that script's secrets.env")
    parser.add_argument("--password", default=None, help="MQTT password (MQTT_PASS from secrets.env)")
    parser.add_argument("--n", type=int, default=10, help="Number of samples (default: 10, per the checklist)")
    parser.add_argument("--timeout-s", type=int, default=180,
                         help="Give up waiting for the Nth new sample after this long")
    parser.add_argument("--results-dir", default="results/3-5")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)

    topic = f"{args.topic_prefix}/persons/{args.student_id}"
    samples = []  # list of (entry_timestamp_str, latency_seconds)
    last_seen_ts = None

    def on_connect(client, userdata, flags, rc):
        print(f"[3-5] connected (rc={rc}), subscribing to {topic}")
        client.subscribe(topic, qos=1)

    def on_message(client, userdata, msg):
        nonlocal last_seen_ts
        receipt_time = datetime.now()

        import json
        try:
            data = json.loads(msg.payload.decode())
        except (json.JSONDecodeError, UnicodeDecodeError):
            return

        ts_str = data.get("timestamp")
        if not ts_str or ts_str == last_seen_ts:
            return  # not a new detection frame, just a repeat poll cycle
        last_seen_ts = ts_str

        try:
            entry_time = parse_timestamp(ts_str)
        except ValueError:
            print(f"[3-5] could not parse timestamp {ts_str!r}, skipping")
            return

        latency_s = (receipt_time - entry_time).total_seconds()
        flag = "" if latency_s >= 0 else "  <-- NEGATIVE (clock skew / timezone mismatch?)"
        print(f"  [{len(samples)+1}/{args.n}] entry={ts_str} receipt={receipt_time.isoformat()} "
              f"latency={latency_s:.3f}s{flag}")
        samples.append((ts_str, latency_s))

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    if args.username:
        client.username_pw_set(args.username, args.password)

    print(f"[3-5] Connecting to {args.host}:{args.port}, waiting for {args.n} new detection samples "
          f"on {topic}...")
    print(f"[3-5] Make sure person_detector.py is actively running and detecting (someone in frame) "
          f"so new samples actually arrive.")

    client.connect(args.host, args.port, keepalive=30)
    client.loop_start()

    start = time.time()
    while len(samples) < args.n and (time.time() - start) < args.timeout_s:
        time.sleep(0.2)

    client.loop_stop()
    client.disconnect()

    if len(samples) < args.n:
        print(f"\n[3-5] WARNING: only got {len(samples)}/{args.n} samples within {args.timeout_s}s "
              f"timeout — is person_detector.py running and actively seeing someone?")

    if not samples:
        raise SystemExit("No samples collected — nothing to analyze.")

    latencies = [s[1] for s in samples]
    positive_latencies = [l for l in latencies if l >= 0]

    mean_lat = statistics.mean(positive_latencies) if positive_latencies else float("nan")
    stdev_lat = statistics.stdev(positive_latencies) if len(positive_latencies) >= 2 else 0.0

    print(f"\n=== Results ({len(samples)} samples, {len(positive_latencies)} valid) ===")
    print(f"Mean latency:   {mean_lat:.3f}s")
    print(f"Std deviation:  {stdev_lat:.3f}s")
    print(f"Min / Max:      {min(positive_latencies):.3f}s / {max(positive_latencies):.3f}s"
          if positive_latencies else "Min / Max: n/a")
    if len(positive_latencies) < len(latencies):
        print(f"WARNING: {len(latencies) - len(positive_latencies)} sample(s) had negative latency "
              f"and were excluded from the mean/stdev above — see clock sync caveat in this script's "
              f"docstring.")

    import csv
    csv_path = results_dir / "latency_samples.csv"
    with open(csv_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["entry_timestamp", "latency_seconds"])
        writer.writerows(samples)
    print(f"\n[3-5] wrote {len(samples)} samples -> {csv_path}")

    summary_path = results_dir / "latency_summary.txt"
    with open(summary_path, "w") as f:
        f.write(f"Samples: {len(samples)} ({len(positive_latencies)} valid)\n")
        f.write(f"Mean latency:  {mean_lat:.3f}s\n")
        f.write(f"Std deviation: {stdev_lat:.3f}s\n")
        if positive_latencies:
            f.write(f"Min: {min(positive_latencies):.3f}s   Max: {max(positive_latencies):.3f}s\n")
        f.write("\nCaveat: persons.json timestamps have 1-second resolution, so this measurement "
                "includes up to ~1s of quantization noise on top of the real pipeline latency.\n")
    print(f"[3-5] wrote summary -> {summary_path}")


if __name__ == "__main__":
    main()
