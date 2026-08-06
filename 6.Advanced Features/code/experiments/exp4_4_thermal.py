"""
experiments/exp4_4_thermal.py — Step 6, Experiment 4-4

"Simulate high temp with Linux tools -> screenshots/logs of adaptive response"

Run this ON THE ORANGE PI ITSELF (not from a laptop) — it uses `stress`
(a standard Linux CPU-load generator) to drive the SoC temperature up for
real, and reads both the REST API (https://localhost) and control.json
directly off disk (only possible locally) to show the thermal manager's
actual response: FPS/resolution dropping when it crosses
thermal_trigger_c, and recovering once it drops back below
thermal_recover_c.

Requires `stress`:
    sudo apt install stress

Usage:
    python3 exp4_4_thermal.py run --duration-s 300 --stress-workers 4
    python3 exp4_4_thermal.py plot --results-dir results/4-4

The `run` duration should comfortably cover: baseline (stress not yet
started) -> stress running long enough to actually cross
thermal_trigger_c on your board (this varies — Orange Pi Zero-class
boards can take a couple of minutes under load) -> stress stopped ->
cooldown back below thermal_recover_c. 300s is a reasonable starting
point; extend it if your board runs cooler/slower to heat up than that.
"""
import argparse
import csv
import json
import subprocess
import time
from pathlib import Path

import requests
from requests.packages.urllib3.exceptions import InsecureRequestWarning
import warnings

warnings.simplefilter("ignore", InsecureRequestWarning)


def read_control_file(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def cmd_run(args):
    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)

    stress_check = subprocess.run(["which", "stress"], capture_output=True)
    if stress_check.returncode != 0:
        raise SystemExit("`stress` not found. Install it first: sudo apt install stress")

    rows = []
    start = time.time()
    stress_proc = None
    stress_start_s = args.baseline_s
    stress_end_s = args.baseline_s + args.stress_duration_s

    print(f"[4-4] Baseline for {args.baseline_s}s, then `stress` for {args.stress_duration_s}s "
          f"({args.stress_workers} workers), then cooldown for the rest of {args.duration_s}s total.")

    while time.time() - start < args.duration_s:
        elapsed = time.time() - start

        if stress_proc is None and elapsed >= stress_start_s:
            print(f"  [{elapsed:7.1f}s] Starting stress ({args.stress_workers} CPU workers)...")
            stress_proc = subprocess.Popen(
                ["stress", "--cpu", str(args.stress_workers), "--timeout", str(args.stress_duration_s)]
            )
        if stress_proc is not None and elapsed >= stress_end_s and stress_proc.poll() is None:
            stress_proc.terminate()

        try:
            resp = requests.get(f"https://localhost/api/v1/telemetry", verify=False, timeout=3)
            telemetry = resp.json() if resp.status_code == 200 else {}
        except requests.RequestException:
            telemetry = {}

        control = read_control_file(args.control_path)

        row = {
            "elapsed_s": round(elapsed, 2),
            "cpu_temp_c": telemetry.get("cpu_temp_c", ""),
            "cpu_percent": telemetry.get("cpu_percent", ""),
            "throttled": control.get("throttled", ""),
            "target_fps": control.get("target_fps", ""),
            "processing_scale": control.get("processing_scale", ""),
            "stress_active": stress_proc is not None and stress_proc.poll() is None,
        }
        rows.append(row)

        if len(rows) == 1 or rows[-2]["throttled"] != row["throttled"]:
            print(f"  [{elapsed:7.1f}s] temp={row['cpu_temp_c']}C throttled={row['throttled']} "
                  f"target_fps={row['target_fps']} scale={row['processing_scale']}")

        time.sleep(args.poll_interval_s)

    if stress_proc is not None and stress_proc.poll() is None:
        stress_proc.terminate()

    csv_path = results_dir / "thermal_samples.csv"
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"\n[4-4] wrote {len(rows)} samples -> {csv_path}")


def cmd_plot(args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    results_dir = Path(args.results_dir)
    csv_path = results_dir / "thermal_samples.csv"
    if not csv_path.exists():
        raise SystemExit(f"No {csv_path} — run `run` first")

    with open(csv_path, newline="") as f:
        rows = list(csv.DictReader(f))

    elapsed = [float(r["elapsed_s"]) for r in rows]
    temps = [float(r["cpu_temp_c"]) if r["cpu_temp_c"] else None for r in rows]
    throttled = [1 if r["throttled"] == "True" else 0 for r in rows]
    stress_active = [1 if r["stress_active"] == "True" else 0 for r in rows]

    fig, ax1 = plt.subplots(figsize=(11, 5))
    ax1.plot(elapsed, temps, color="tab:red", label="CPU temp (°C)")
    ax1.set_xlabel("Elapsed time (s)")
    ax1.set_ylabel("CPU temp (°C)", color="tab:red")
    valid_temps = [t for t in temps if t is not None]
    if valid_temps:
        ax1.fill_between(elapsed, 0, [max(valid_temps)] * len(elapsed),
                          where=stress_active, alpha=0.08, color="orange", label="stress active")
    ax1.grid(True, alpha=0.3)

    ax2 = ax1.twinx()
    ax2.step(elapsed, throttled, color="tab:blue", where="post", label="throttled")
    ax2.set_ylabel("Throttled (0/1)", color="tab:blue")
    ax2.set_ylim(-0.1, 1.1)

    fig.suptitle("Experiment 4-4: CPU temp vs adaptive thermal throttling")
    fig.tight_layout()

    out_png = results_dir / "exp4_4_thermal.png"
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    print(f"[4-4] wrote graph -> {out_png}")

    # Find the throttle/recover transition points for the report text.
    throttle_at = None
    recover_at = None
    for i in range(1, len(throttled)):
        if throttled[i - 1] == 0 and throttled[i] == 1 and throttle_at is None:
            throttle_at = elapsed[i]
        if throttle_at is not None and throttled[i - 1] == 1 and throttled[i] == 0:
            recover_at = elapsed[i]
    print(f"\n=== Transitions ===")
    print(f"Throttled at: {throttle_at}s" if throttle_at else "No throttle event observed")
    print(f"Recovered at: {recover_at}s" if recover_at else "No recovery event observed (or still throttled at end)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run")
    p_run.add_argument("--duration-s", type=int, default=300)
    p_run.add_argument("--baseline-s", type=int, default=20, help="Seconds before starting stress")
    p_run.add_argument("--stress-duration-s", type=int, default=180)
    p_run.add_argument("--stress-workers", type=int, default=4)
    p_run.add_argument("--poll-interval-s", type=float, default=2.0)
    p_run.add_argument("--control-path", default="/dev/shm/surveillance/control.json")
    p_run.add_argument("--results-dir", default="results/4-4")
    p_run.set_defaults(func=cmd_run)

    p_plot = sub.add_parser("plot")
    p_plot.add_argument("--results-dir", default="results/4-4")
    p_plot.set_defaults(func=cmd_plot)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
