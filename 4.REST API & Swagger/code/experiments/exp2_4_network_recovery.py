"""
experiments/exp2_4_network_recovery.py — Step 4, Experiment 2-4

"Kill network mid-stream, reconnect after 2 min -> explain behavior,
show logs, describe recovery"

The network kill/reconnect itself has to be done by hand (unplug
Ethernet, disable Wi-Fi, pull the cable — whatever matches your actual
setup) — no script can do that part for you. What this script automates
is everything around it: watching the live MJPEG stream so you get an
exact "stream broke at T" / "stream recovered at T" timestamp instead of
eyeballing it, a lightweight parallel liveness poll for a cleaner uptime/
downtime timeline graph, and pulling the C server's own journalctl logs
afterward (via the /api/v1/services/{name}/logs bonus endpoint) so you
can show what the server actually logged during the outage.

Recommended run sequence for the actual experiment:

    Terminal 1 (keep running the whole time):
        python3 exp2_4_network_recovery.py monitor --host 192.168.0.170 --duration-s 400

    Terminal 2 (keep running the whole time):
        python3 exp2_4_network_recovery.py stream-watch --host 192.168.0.170 --duration-s 400

    Now: let it run ~30s normally, then kill the network (unplug/disable
    Wi-Fi), wait, count out roughly 2 minutes, then reconnect. Let both
    scripts keep running until they hit --duration-s and exit on their
    own.

    Afterward:
        python3 exp2_4_network_recovery.py fetch-logs --host 192.168.0.170
        python3 exp2_4_network_recovery.py plot --results-dir results/2-4
"""
import argparse
import csv
import time
from pathlib import Path

import requests
from lib import base_url, get_json


def cmd_monitor(args):
    """Polls /api/v1/persons every ~1s and logs up/down transitions —
    this is the lightweight liveness signal for the timeline graph."""
    out_path = Path(args.results_dir) / "monitor_log.csv"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    rows = []
    start = time.time()
    last_state = None  # None = not yet known, True = up, False = down

    print(f"[2-4] Monitoring liveness for {args.duration_s}s (poll every ~1s). "
          f"Kill the network whenever you're ready — this will keep running.")

    while time.time() - start < args.duration_s:
        t0 = time.perf_counter()
        data = get_json(f"{base_url(args.host, args.port)}/api/v1/persons", timeout=2.0)
        latency_ms = (time.perf_counter() - t0) * 1000
        elapsed = time.time() - start
        up = data is not None

        rows.append({"elapsed_s": round(elapsed, 2), "up": up, "latency_ms": round(latency_ms, 1)})

        if up != last_state:
            state_str = "UP" if up else "DOWN"
            print(f"  [{elapsed:7.1f}s] *** state change -> {state_str} ***")
            last_state = up

        time.sleep(max(0.0, 1.0 - (time.perf_counter() - t0)))

    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["elapsed_s", "up", "latency_ms"])
        writer.writeheader()
        writer.writerows(rows)
    print(f"[2-4] wrote {len(rows)} rows -> {out_path}")


def cmd_stream_watch(args):
    """Keeps a live connection to /api/v1/stream open and logs exactly
    when it breaks and when a reconnect attempt succeeds again."""
    out_path = Path(args.results_dir) / "stream_watch_log.csv"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    events = []
    start = time.time()
    url = f"{base_url(args.host, args.port)}/api/v1/stream"

    print(f"[2-4] Watching live MJPEG stream at {url} for up to {args.duration_s}s total "
          f"(across reconnect attempts). Kill the network whenever you're ready.")

    def log_event(kind: str):
        elapsed = time.time() - start
        events.append({"elapsed_s": round(elapsed, 2), "event": kind})
        print(f"  [{elapsed:7.1f}s] {kind}")

    log_event("attempting_connect")

    while time.time() - start < args.duration_s:
        try:
            with requests.get(url, verify=False, stream=True, timeout=(5, 10)) as resp:
                if resp.status_code != 200:
                    log_event(f"connect_failed_status_{resp.status_code}")
                    time.sleep(args.retry_interval_s)
                    continue

                log_event("connected")
                bytes_seen = 0
                for chunk in resp.iter_content(chunk_size=4096):
                    if chunk:
                        bytes_seen += len(chunk)
                    if time.time() - start >= args.duration_s:
                        break
                log_event(f"stream_ended_after_{bytes_seen}_bytes")

        except requests.RequestException as exc:
            log_event(f"connection_lost ({type(exc).__name__})")

        if time.time() - start < args.duration_s:
            time.sleep(args.retry_interval_s)
            log_event("retrying_connect")

    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["elapsed_s", "event"])
        writer.writeheader()
        writer.writerows(events)
    print(f"[2-4] wrote {len(events)} events -> {out_path}")


def cmd_fetch_logs(args):
    """Pulls the C server's own journalctl output after the fact, via the
    /api/v1/services/{name}/logs bonus endpoint — run this AFTER
    reconnecting, so the request can actually reach the server."""
    out_path = Path(args.results_dir) / f"{args.service}_journalctl.txt"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    url = f"{base_url(args.host, args.port)}/api/v1/services/{args.service}/logs?lines={args.lines}"
    data = get_json(url, timeout=10.0)
    if data is None:
        raise SystemExit(f"Could not fetch logs from {url} — is the server reachable again yet?")

    with open(out_path, "w") as f:
        f.write(data.get("logs", ""))
    print(f"[2-4] wrote {args.service} logs -> {out_path}")


def cmd_plot(args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    results_dir = Path(args.results_dir)
    monitor_csv = results_dir / "monitor_log.csv"
    if not monitor_csv.exists():
        raise SystemExit(f"No {monitor_csv} — run `monitor` first")

    with open(monitor_csv, newline="") as f:
        rows = list(csv.DictReader(f))

    elapsed = [float(r["elapsed_s"]) for r in rows]
    up = [1 if r["up"] == "True" else 0 for r in rows]

    fig, ax = plt.subplots(figsize=(11, 3))
    ax.fill_between(elapsed, 0, up, step="post", color="tab:green", alpha=0.4, label="up")
    ax.fill_between(elapsed, 0, [1 - u for u in up], step="post", color="tab:red", alpha=0.4, label="down")
    ax.set_ylim(-0.1, 1.1)
    ax.set_yticks([0, 1])
    ax.set_yticklabels(["down", "up"])
    ax.set_xlabel("Elapsed time (s)")
    ax.set_title("Experiment 2-4: Server reachability timeline (network kill + reconnect)")
    ax.grid(True, alpha=0.3)

    out_png = results_dir / "exp2_4_recovery_timeline.png"
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    print(f"[2-4] wrote graph -> {out_png}")

    # Detect the outage window: first transition to down, first transition
    # back to up after that.
    down_start = None
    down_end = None
    for i in range(1, len(up)):
        if up[i - 1] == 1 and up[i] == 0 and down_start is None:
            down_start = elapsed[i]
        if down_start is not None and up[i - 1] == 0 and up[i] == 1 and down_end is None:
            down_end = elapsed[i]
            break

    print("\n=== Recovery analysis ===")
    if down_start is not None:
        print(f"Outage detected starting at ~{down_start:.1f}s")
        if down_end is not None:
            print(f"Recovery detected at ~{down_end:.1f}s")
            print(f"Total downtime: ~{down_end - down_start:.1f}s")
        else:
            print("No recovery observed within the monitored window — "
                  "extend --duration-s if the reconnect happened after the script exited.")
    else:
        print("No outage detected in this log — was the network actually killed during "
              "the monitored window, or did the poll interval miss it?")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_mon = sub.add_parser("monitor", help="Poll /api/v1/persons every ~1s, log up/down transitions")
    p_mon.add_argument("--host", required=True)
    p_mon.add_argument("--port", type=int, default=443)
    p_mon.add_argument("--duration-s", type=int, default=400,
                        help="Total monitoring time — should comfortably exceed the ~2min "
                             "outage plus buffer on both sides (default: 400s)")
    p_mon.add_argument("--results-dir", default="results/2-4")
    p_mon.set_defaults(func=cmd_monitor)

    p_sw = sub.add_parser("stream-watch", help="Keep /api/v1/stream open, log break/reconnect events")
    p_sw.add_argument("--host", required=True)
    p_sw.add_argument("--port", type=int, default=443)
    p_sw.add_argument("--duration-s", type=int, default=400)
    p_sw.add_argument("--retry-interval-s", type=float, default=3.0)
    p_sw.add_argument("--results-dir", default="results/2-4")
    p_sw.set_defaults(func=cmd_stream_watch)

    p_logs = sub.add_parser("fetch-logs", help="Pull server-side journalctl logs after reconnecting")
    p_logs.add_argument("--host", required=True)
    p_logs.add_argument("--port", type=int, default=443)
    p_logs.add_argument("--service", default="surveillance-web")
    p_logs.add_argument("--lines", type=int, default=200)
    p_logs.add_argument("--results-dir", default="results/2-4")
    p_logs.set_defaults(func=cmd_fetch_logs)

    p_plot = sub.add_parser("plot", help="Plot the reachability timeline + detect outage window")
    p_plot.add_argument("--results-dir", default="results/2-4")
    p_plot.set_defaults(func=cmd_plot)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
