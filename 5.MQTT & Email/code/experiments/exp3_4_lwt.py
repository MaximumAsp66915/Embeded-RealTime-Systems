"""
experiments/exp3_4_lwt.py — Step 5, Experiment 3-4

"Stop the broker, restart after 3 min -> show the LWT message"

Run this FROM A LAPTOP/PC (playing the role of "the PC that gets
notified if the board drops"), subscribed to the status topic before you
touch the broker at all.

Usage:
    python3 exp3_4_lwt.py --host 192.168.0.170 --student-id 402101906 --duration-s 500

Then, while it's running:
    1. Let it run ~20-30s so you can see normal "online"/persons/telemetry
       traffic arriving.
    2. On the Pi (or wherever the broker runs):  sudo systemctl stop mosquitto
       -> expect an "offline" message to arrive almost immediately. This
       is Mosquitto delivering the configured Last Will as part of its own
       shutdown sequence for every still-connected client — this is the
       actual LWT mechanism firing, not a timeout you have to wait out.
    3. Wait ~3 minutes with the broker down.
    4. sudo systemctl start mosquitto
       -> expect: this script's own subscriber reconnects automatically
       (paho's built-in reconnect), and separately the notifier daemon on
       the Pi reconnects and republishes a fresh "online" — watch for
       both events in the log.
    5. Let the script run to --duration-s and exit on its own.

Everything received is written to results/3-4/lwt_log.csv with wall-clock
timestamps, so you can correlate against exactly when you ran each
`systemctl` command for the report.
"""
import argparse
import csv
import time
from pathlib import Path

import paho.mqtt.client as mqtt


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, help="MQTT broker host (the Pi, unless you point notifier.conf elsewhere)")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--student-id", required=True)
    parser.add_argument("--topic-prefix", default="home")
    parser.add_argument("--username", default=None,
                         help="MQTT username, if the broker requires auth (e.g. secure_setup.sh's "
                              "Mosquitto config sets allow_anonymous false) — same MQTT_USER from "
                              "that script's secrets.env")
    parser.add_argument("--password", default=None, help="MQTT password (MQTT_PASS from secrets.env)")
    parser.add_argument("--duration-s", type=int, default=500,
                         help="Total time to keep subscribing — should comfortably exceed "
                              "the ~3min outage plus margin on both sides (default: 500s)")
    parser.add_argument("--results-dir", default="results/3-4")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)
    log_path = results_dir / "lwt_log.csv"

    rows = []
    start = time.time()

    status_topic = f"{args.topic_prefix}/status/{args.student_id}"
    persons_topic = f"{args.topic_prefix}/persons/{args.student_id}"
    telemetry_topic = f"{args.topic_prefix}/telemetry/{args.student_id}"

    def log_row(event: str, topic: str = "", payload: str = ""):
        elapsed = time.time() - start
        rows.append({"elapsed_s": round(elapsed, 2), "event": event, "topic": topic, "payload": payload})
        print(f"  [{elapsed:7.1f}s] {event:20s} topic={topic!r:40s} payload={payload!r}")

    def on_connect(client, userdata, flags, rc):
        log_row("connected", "", f"rc={rc}")
        client.subscribe(status_topic, qos=1)
        client.subscribe(persons_topic, qos=1)
        client.subscribe(telemetry_topic, qos=1)

    def on_disconnect(client, userdata, rc):
        log_row("disconnected", "", f"rc={rc}")

    def on_message(client, userdata, msg):
        log_row("message", msg.topic, msg.payload.decode(errors="replace"))

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    if args.username:
        client.username_pw_set(args.username, args.password)

    # paho's own automatic-reconnect backoff — this is what lets our
    # subscriber here come back on its own once the broker is back up,
    # separate from (but analogous to) the notifier daemon's own
    # reconnect on the Pi side.
    client.reconnect_delay_set(min_delay=1, max_delay=10)

    print(f"[3-4] Connecting to {args.host}:{args.port}, subscribing to status/persons/telemetry "
          f"for student_id={args.student_id}...")
    print(f"[3-4] Will run for {args.duration_s}s total. Stop/restart the broker whenever you're ready.")

    client.connect(args.host, args.port, keepalive=30)
    client.loop_start()

    try:
        while time.time() - start < args.duration_s:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[3-4] interrupted early")
    finally:
        client.loop_stop()
        client.disconnect()

    with open(log_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["elapsed_s", "event", "topic", "payload"])
        writer.writeheader()
        writer.writerows(rows)
    print(f"\n[3-4] wrote {len(rows)} events -> {log_path}")

    offline_events = [r for r in rows if r["event"] == "message" and r["payload"] == "offline"]
    online_events = [r for r in rows if r["event"] == "message" and r["payload"] == "online"]
    print(f"\n=== Summary ===")
    print(f"'offline' messages received: {len(offline_events)} "
          f"(at {[r['elapsed_s'] for r in offline_events]})")
    print(f"'online' messages received:  {len(online_events)} "
          f"(at {[r['elapsed_s'] for r in online_events]})")
    if not offline_events:
        print("No 'offline' message observed — either the broker shutdown didn't happen during "
              "this run, or your Mosquitto version/config doesn't deliver LWTs on its own "
              "shutdown (some setups only fire the LWT on an actual keepalive TIMEOUT, not a "
              "clean-ish broker stop — worth checking mosquitto's own logs during the stop if so).")


if __name__ == "__main__":
    main()
