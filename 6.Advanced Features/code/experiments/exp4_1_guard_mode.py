"""
experiments/exp4_1_guard_mode.py — Step 6, Experiment 4-1

"Guard Mode demo -> video + report images"

The actual demo is a screen/phone recording you make yourself: toggle
Guard Mode on (dashboard button or `curl -X POST .../api/v1/guard -d
'{"enabled": true}'`), walk into frame, show the alert arriving (email
and/or MQTT). This script isn't a substitute for that video — it's here
to give you an exact, timestamped log of the MQTT alarm topic firing
during the demo, which is much better evidence for the report than
"trust me, it happened around minute 2."

Usage:
    python3 exp4_1_guard_mode.py --host 192.168.0.170 --student-id 402101906 \\
        --username board_client --password orangepi --duration-s 300

Run this BEFORE you start recording, leave it running for the whole
demo, and it'll print (and save to CSV) every alarm message with a
wall-clock timestamp — screenshot the terminal output or the CSV
alongside your video/report images.
"""
import argparse
import csv
import time
from pathlib import Path

import paho.mqtt.client as mqtt


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--student-id", required=True)
    parser.add_argument("--topic-prefix", default="home")
    parser.add_argument("--username", default=None)
    parser.add_argument("--password", default=None)
    parser.add_argument("--duration-s", type=int, default=300)
    parser.add_argument("--results-dir", default="results/4-1")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)
    log_path = results_dir / "alarm_log.csv"

    alarm_topic = f"{args.topic_prefix}/{args.student_id}/alarm"
    rows = []
    start = time.time()

    def on_connect(client, userdata, flags, rc):
        print(f"[4-1] connected (rc={rc}), subscribing to {alarm_topic}")
        client.subscribe(alarm_topic, qos=1)

    def on_message(client, userdata, msg):
        elapsed = time.time() - start
        payload = msg.payload.decode(errors="replace")
        rows.append({"elapsed_s": round(elapsed, 2), "topic": msg.topic, "payload": payload})
        print(f"  [{elapsed:7.1f}s] ALARM on {msg.topic}: {payload}")

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    if args.username:
        client.username_pw_set(args.username, args.password)

    print(f"[4-1] Watching for Guard Mode alarms for {args.duration_s}s. "
          f"Toggle Guard Mode on and trigger a detection whenever you're ready to record.")

    client.connect(args.host, args.port, keepalive=30)
    client.loop_start()

    try:
        while time.time() - start < args.duration_s:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[4-1] interrupted early")
    finally:
        client.loop_stop()
        client.disconnect()

    with open(log_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["elapsed_s", "topic", "payload"])
        writer.writeheader()
        writer.writerows(rows)
    print(f"\n[4-1] wrote {len(rows)} alarm events -> {log_path}")

    if not rows:
        print("[4-1] No alarms received — confirm Guard Mode was actually toggled ON "
              "(GET /api/v1/guard should show {\"enabled\": true}) and a detection occurred "
              "during the monitored window.")


if __name__ == "__main__":
    main()
