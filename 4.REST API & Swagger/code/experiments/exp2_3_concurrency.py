"""
experiments/exp2_3_concurrency.py — Step 4, Experiment 2-3

"50 concurrent curl requests to /api/v1/telemetry -> graph temp/CPU/
memory changes + latency analysis"

Usage:
    python3 exp2_3_concurrency.py run --host 192.168.0.170
    python3 exp2_3_concurrency.py plot --results-dir results/2-3
"""
import argparse
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

from lib import base_url, get_json, sample_telemetry, write_csv, read_csv_rows


@dataclass
class RequestResult:
    request_index: int
    latency_ms: float
    status: str  # "ok" or "failed"


def fire_one(host: str, port: int, index: int) -> RequestResult:
    url = f"{base_url(host, port)}/api/v1/telemetry"
    t0 = time.perf_counter()
    data = get_json(url, timeout=15.0)
    latency_ms = (time.perf_counter() - t0) * 1000
    return RequestResult(index, round(latency_ms, 2), "ok" if data is not None else "failed")


def cmd_run(args):
    print(f"[2-3] Sampling telemetry BEFORE the burst...")
    before = sample_telemetry(args.host, args.port, time.time())

    print(f"[2-3] Firing {args.n} concurrent requests to /api/v1/telemetry "
          f"(concurrency={args.concurrency})...")
    results = []
    t_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures = [pool.submit(fire_one, args.host, args.port, i) for i in range(args.n)]
        for fut in as_completed(futures):
            results.append(fut.result())
    total_wall_s = time.perf_counter() - t_start

    results.sort(key=lambda r: r.request_index)
    write_csv(Path(args.results_dir) / "requests.csv", results)

    print(f"[2-3] Sampling telemetry AFTER the burst...")
    after = sample_telemetry(args.host, args.port, time.time())

    # Give the server a moment to settle, then sample once more to see
    # whether temp/CPU/memory return toward baseline.
    time.sleep(10)
    print(f"[2-3] Sampling telemetry 10s AFTER the burst (settling)...")
    settled = sample_telemetry(args.host, args.port, time.time())

    before_after_path = Path(args.results_dir) / "before_after.csv"
    write_csv(before_after_path, [before, after, settled])

    ok_latencies = [r.latency_ms for r in results if r.status == "ok"]
    failed = [r for r in results if r.status == "failed"]

    print("\n=== Burst summary ===")
    print(f"Total wall time for {args.n} requests at concurrency={args.concurrency}: {total_wall_s:.2f}s")
    print(f"Succeeded: {len(ok_latencies)}/{args.n}   Failed: {len(failed)}/{args.n}")
    if ok_latencies:
        ok_latencies_sorted = sorted(ok_latencies)
        p50 = ok_latencies_sorted[len(ok_latencies_sorted) // 2]
        p95 = ok_latencies_sorted[int(len(ok_latencies_sorted) * 0.95)]
        print(f"Latency (ms) — min: {min(ok_latencies):.1f}  "
              f"p50: {p50:.1f}  p95: {p95:.1f}  max: {max(ok_latencies):.1f}  "
              f"mean: {sum(ok_latencies)/len(ok_latencies):.1f}")
    print(f"\nTemp:  before={before.cpu_temp_c:.1f}C  after={after.cpu_temp_c:.1f}C  "
          f"settled(+10s)={settled.cpu_temp_c:.1f}C")
    print(f"CPU:   before={before.cpu_percent:.1f}%  after={after.cpu_percent:.1f}%  "
          f"settled(+10s)={settled.cpu_percent:.1f}%")
    print(f"MemFree: before={before.mem_free_mb:.1f}MB  after={after.mem_free_mb:.1f}MB  "
          f"settled(+10s)={settled.mem_free_mb:.1f}MB")


def cmd_plot(args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    results_dir = Path(args.results_dir)
    req_csv = results_dir / "requests.csv"
    ba_csv = results_dir / "before_after.csv"
    if not req_csv.exists() or not ba_csv.exists():
        raise SystemExit(f"Missing CSVs in {results_dir} — run `run` first")

    req_rows = read_csv_rows(req_csv)
    ok_rows = [r for r in req_rows if r["status"] == "ok"]
    latencies = sorted(float(r["latency_ms"]) for r in ok_rows)

    ba_rows = read_csv_rows(ba_csv)  # [before, after, settled]
    labels = ["before", "after burst", "settled (+10s)"]
    temps = [float(r["cpu_temp_c"]) for r in ba_rows]
    cpu = [float(r["cpu_percent"]) for r in ba_rows]
    mem_free = [float(r["mem_free_mb"]) for r in ba_rows]

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))

    # Latency distribution
    axes[0].hist(latencies, bins=20, color="tab:blue", edgecolor="black", alpha=0.7)
    axes[0].set_xlabel("Latency (ms)")
    axes[0].set_ylabel("Number of requests")
    axes[0].set_title(f"Experiment 2-3: Latency distribution ({len(latencies)} concurrent requests)")
    axes[0].grid(True, alpha=0.3)

    # Before/after/settled temp+cpu+mem
    x = range(len(labels))
    ax2 = axes[1]
    ax2.plot(x, temps, marker="o", label="CPU temp (°C)", color="tab:red")
    ax2.plot(x, cpu, marker="s", label="CPU load (%)", color="tab:orange")
    ax2.set_xticks(list(x))
    ax2.set_xticklabels(labels)
    ax2.set_ylabel("Temp (°C) / CPU (%)")
    ax2.legend(loc="upper left")
    ax2.grid(True, alpha=0.3)

    ax3 = ax2.twinx()
    ax3.plot(x, mem_free, marker="^", label="Mem free (MB)", color="tab:green")
    ax3.set_ylabel("Mem free (MB)")
    ax3.legend(loc="upper right")

    axes[1].set_title("System state before / after / settled")

    fig.tight_layout()
    out_png = results_dir / "exp2_3_concurrency.png"
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    print(f"[2-3] wrote graph -> {out_png}")

    if latencies:
        p50 = latencies[len(latencies) // 2]
        p95 = latencies[int(len(latencies) * 0.95)]
        print("\n=== Latency analysis ===")
        print(f"n={len(latencies)}  min={min(latencies):.1f}ms  p50={p50:.1f}ms  "
              f"p95={p95:.1f}ms  max={max(latencies):.1f}ms  "
              f"mean={sum(latencies)/len(latencies):.1f}ms")
        failed_count = len(req_rows) - len(ok_rows)
        if failed_count:
            print(f"WARNING: {failed_count} requests failed under load — worth noting in the "
                  f"report as a concurrency limit, not just a latency number.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run", help="Fire the concurrent burst and sample before/after/settled telemetry")
    p_run.add_argument("--host", required=True)
    p_run.add_argument("--port", type=int, default=443)
    p_run.add_argument("--n", type=int, default=50, help="Number of requests (default: 50)")
    p_run.add_argument("--concurrency", type=int, default=50, help="Max concurrent workers (default: 50 = all at once)")
    p_run.add_argument("--results-dir", default="results/2-3")
    p_run.set_defaults(func=cmd_run)

    p_plot = sub.add_parser("plot", help="Plot latency distribution + before/after/settled system state")
    p_plot.add_argument("--results-dir", default="results/2-3")
    p_plot.set_defaults(func=cmd_plot)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
