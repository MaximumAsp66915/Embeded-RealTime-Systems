"""
experiments/exp2_2_memory.py — Step 4, Experiment 2-2

"Memory usage over 5 min of continuous streaming, sampled every 5s ->
graph + leak analysis"

Usage:
    # Start streaming first (laptop_stream_webcam.sh + open /api/v1/stream
    # somewhere so the C server is actually serving continuous frames for
    # the whole 5 minutes), THEN run this:
    python3 exp2_2_memory.py sample --host 192.168.0.170

    python3 exp2_2_memory.py plot --results-dir results/2-2
"""
import argparse
import time
from pathlib import Path

from lib import sample_telemetry, write_csv, read_csv_rows


def cmd_sample(args):
    rows = []
    start = time.time()
    n_samples = (args.duration_s // args.interval_s) + 1

    print(f"[2-2] Sampling memory for {args.duration_s}s every {args.interval_s}s "
          f"({n_samples} samples). Make sure streaming is already running.")

    for i in range(n_samples):
        target_t = start + i * args.interval_s
        now = time.time()
        if now < target_t:
            time.sleep(target_t - now)

        sample = sample_telemetry(args.host, args.port, start)
        rows.append(sample)
        used_mb = sample.mem_total_mb - sample.mem_free_mb
        status = "ok" if sample.ok else "FAILED"
        print(f"  [{sample.elapsed_s:6.1f}s] mem_free={sample.mem_free_mb:7.1f}MB "
              f"mem_used={used_mb:7.1f}MB [{status}]")

    out_path = Path(args.results_dir) / "memory_samples.csv"
    write_csv(out_path, rows)


def cmd_plot(args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    results_dir = Path(args.results_dir)
    csv_path = results_dir / "memory_samples.csv"
    if not csv_path.exists():
        raise SystemExit(f"No {csv_path} — run `sample` first")

    rows = read_csv_rows(csv_path)
    ok_rows = [r for r in rows if r["ok"] == "True"]
    elapsed = [float(r["elapsed_s"]) for r in ok_rows]
    mem_total = float(ok_rows[0]["mem_total_mb"]) if ok_rows else 0.0
    mem_used = [mem_total - float(r["mem_free_mb"]) for r in ok_rows]

    fig, ax = plt.subplots(figsize=(9, 5))
    ax.plot(elapsed, mem_used, marker="o", markersize=3, color="tab:red")
    ax.set_xlabel("Elapsed time (s)")
    ax.set_ylabel("Memory used (MB)")
    ax.set_title("Experiment 2-2: Memory usage during 5 min of continuous streaming")
    ax.grid(True, alpha=0.3)

    out_png = results_dir / "exp2_2_memory.png"
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    print(f"[2-2] wrote graph -> {out_png}")

    # --- Leak analysis: simple linear regression slope of mem_used vs time ---
    # A real leak shows up as a sustained positive slope (memory climbing
    # and never coming back down) rather than noise oscillating around a
    # flat mean. This is a rough screening tool, not proof either way —
    # note that explicitly in the report rather than treating a small
    # slope as a definitive verdict.
    n = len(elapsed)
    if n >= 2:
        mean_t = sum(elapsed) / n
        mean_m = sum(mem_used) / n
        num = sum((elapsed[i] - mean_t) * (mem_used[i] - mean_m) for i in range(n))
        den = sum((elapsed[i] - mean_t) ** 2 for i in range(n))
        slope_mb_per_s = num / den if den != 0 else 0.0
        slope_mb_per_min = slope_mb_per_s * 60
    else:
        slope_mb_per_min = 0.0

    first_used = mem_used[0] if mem_used else 0.0
    last_used = mem_used[-1] if mem_used else 0.0
    peak_used = max(mem_used) if mem_used else 0.0

    print("\n=== Leak analysis ===")
    print(f"First sample used:  {first_used:.1f} MB")
    print(f"Last sample used:   {last_used:.1f} MB")
    print(f"Peak used:          {peak_used:.1f} MB")
    print(f"Net change:         {last_used - first_used:+.1f} MB over {elapsed[-1] if elapsed else 0:.0f}s")
    print(f"Linear trend slope: {slope_mb_per_min:+.3f} MB/min")

    if abs(slope_mb_per_min) < 0.5:
        verdict = "No meaningful upward trend — consistent with no leak over this window."
    elif slope_mb_per_min >= 0.5:
        verdict = ("Sustained upward trend detected. Worth a longer run (30-60 min) to "
                   "confirm this isn't just measurement noise from a short window before "
                   "concluding there's an actual leak.")
    else:
        verdict = "Downward trend — memory usage decreasing, not indicative of a leak."
    print(f"Verdict: {verdict}")

    summary_path = results_dir / "exp2_2_leak_analysis.txt"
    with open(summary_path, "w") as f:
        f.write(
            f"First sample used:  {first_used:.1f} MB\n"
            f"Last sample used:   {last_used:.1f} MB\n"
            f"Peak used:          {peak_used:.1f} MB\n"
            f"Net change:         {last_used - first_used:+.1f} MB\n"
            f"Linear trend slope: {slope_mb_per_min:+.3f} MB/min\n"
            f"Verdict: {verdict}\n"
        )
    print(f"[2-2] wrote leak analysis -> {summary_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_sample = sub.add_parser("sample", help="Sample memory usage for 5 minutes")
    p_sample.add_argument("--host", required=True)
    p_sample.add_argument("--port", type=int, default=443)
    p_sample.add_argument("--duration-s", type=int, default=300)
    p_sample.add_argument("--interval-s", type=int, default=5)
    p_sample.add_argument("--results-dir", default="results/2-2")
    p_sample.set_defaults(func=cmd_sample)

    p_plot = sub.add_parser("plot", help="Plot the memory graph + leak analysis")
    p_plot.add_argument("--results-dir", default="results/2-2")
    p_plot.set_defaults(func=cmd_plot)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
