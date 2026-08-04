"""
experiments/exp2_1_temperature.py — Step 4, Experiment 2-1

"Sample temp every 30s for 5 min under idle / streaming-only /
streaming+detection -> 3-curve graph + min/max table + screenshot"

Two subcommands:

  sample  Runs ONE 5-minute sampling pass and writes a CSV. Run this three
          times, once per condition, changing what's actually running on
          the Pi between runs (see the --label examples below) — this
          script can't change board state for you, since "idle" vs
          "streaming" vs "streaming+detection" means actually starting/
          stopping person_detector.py and a viewer between runs.

  plot    Reads the three CSVs produced by `sample` and draws the
          3-curve comparison graph + min/max table the experiment asks
          for.

Usage:
    # 1) Idle: nothing else running except the C server itself.
    python3 exp2_1_temperature.py sample --host 192.168.0.170 --label idle

    # 2) Streaming-only: start laptop_stream_webcam.sh + open
    #    /api/v1/stream in a browser, but do NOT run person_detector.py.
    python3 exp2_1_temperature.py sample --host 192.168.0.170 --label streaming

    # 3) Streaming+detection: person_detector.py running too.
    python3 exp2_1_temperature.py sample --host 192.168.0.170 --label streaming_detection

    # 4) Combine into the report graph + table:
    python3 exp2_1_temperature.py plot --results-dir results/2-1
"""
import argparse
import time
from pathlib import Path

from lib import sample_telemetry, write_csv, read_csv_rows


def cmd_sample(args):
    rows = []
    start = time.time()
    n_samples = (args.duration_s // args.interval_s) + 1

    print(f"[2-1] Sampling '{args.label}' for {args.duration_s}s every {args.interval_s}s "
          f"({n_samples} samples)...")

    for i in range(n_samples):
        target_t = start + i * args.interval_s
        now = time.time()
        if now < target_t:
            time.sleep(target_t - now)

        sample = sample_telemetry(args.host, args.port, start)
        rows.append(sample)
        status = "ok" if sample.ok else "FAILED"
        print(f"  [{sample.elapsed_s:6.1f}s] temp={sample.cpu_temp_c:5.1f}C "
              f"cpu={sample.cpu_percent:5.1f}% mem_free={sample.mem_free_mb:6.1f}MB [{status}]")

    out_path = Path(args.results_dir) / f"{args.label}.csv"
    write_csv(out_path, rows)


def cmd_plot(args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    results_dir = Path(args.results_dir)
    labels = args.labels
    csv_paths = {label: results_dir / f"{label}.csv" for label in labels}

    missing = [label for label, p in csv_paths.items() if not p.exists()]
    if missing:
        raise SystemExit(
            f"Missing CSVs for: {missing} in {results_dir} — run `sample --label <name>` "
            f"for each condition first (expected filenames: {[f'{m}.csv' for m in missing]})"
        )

    fig, ax = plt.subplots(figsize=(9, 5))
    summary_rows = []

    for label in labels:
        rows = read_csv_rows(csv_paths[label])
        ok_rows = [r for r in rows if r["ok"] == "True"]
        elapsed = [float(r["elapsed_s"]) for r in ok_rows]
        temps = [float(r["cpu_temp_c"]) for r in ok_rows]

        ax.plot(elapsed, temps, marker="o", markersize=3, label=label.replace("_", " "))

        if temps:
            summary_rows.append({
                "condition": label,
                "min_temp_c": round(min(temps), 1),
                "max_temp_c": round(max(temps), 1),
                "avg_temp_c": round(sum(temps) / len(temps), 1),
                "samples": len(temps),
                "failed_samples": len(rows) - len(ok_rows),
            })

    ax.set_xlabel("Elapsed time (s)")
    ax.set_ylabel("CPU temperature (°C)")
    ax.set_title("Experiment 2-1: CPU temperature under load (idle / streaming / streaming+detection)")
    ax.legend()
    ax.grid(True, alpha=0.3)

    out_png = results_dir / "exp2_1_temperature.png"
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    print(f"[2-1] wrote graph -> {out_png}")

    # Min/max table, both to stdout (for the report) and a CSV.
    print("\n| Condition | Min °C | Max °C | Avg °C | Samples | Failed |")
    print("|---|---|---|---|---|---|")
    for r in summary_rows:
        print(f"| {r['condition']} | {r['min_temp_c']} | {r['max_temp_c']} | "
              f"{r['avg_temp_c']} | {r['samples']} | {r['failed_samples']} |")

    import csv as csv_module
    table_path = results_dir / "exp2_1_summary_table.csv"
    with open(table_path, "w", newline="") as f:
        writer = csv_module.DictWriter(f, fieldnames=list(summary_rows[0].keys()))
        writer.writeheader()
        writer.writerows(summary_rows)
    print(f"[2-1] wrote summary table -> {table_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_sample = sub.add_parser("sample", help="Run one 5-minute sampling pass for one condition")
    p_sample.add_argument("--host", required=True, help="Pi's IP or hostname")
    p_sample.add_argument("--port", type=int, default=443)
    p_sample.add_argument("--label", required=True,
                           help="Condition name — used as the output filename. "
                                "Use 'idle', 'streaming', 'streaming_detection' to match "
                                "the plot command's defaults.")
    p_sample.add_argument("--duration-s", type=int, default=300, help="Total sampling duration (default: 5 min)")
    p_sample.add_argument("--interval-s", type=int, default=30, help="Seconds between samples (default: 30)")
    p_sample.add_argument("--results-dir", default="results/2-1")
    p_sample.set_defaults(func=cmd_sample)

    p_plot = sub.add_parser("plot", help="Combine the 3 condition CSVs into the report graph + table")
    p_plot.add_argument("--results-dir", default="results/2-1")
    p_plot.add_argument("--labels", nargs=3, default=["idle", "streaming", "streaming_detection"],
                         help="The 3 condition labels to plot, in order (default: idle streaming streaming_detection)")
    p_plot.set_defaults(func=cmd_plot)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
