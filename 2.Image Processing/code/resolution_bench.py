#!/usr/bin/env python3
"""
resolution_bench.py

Smart Surveillance System — helper for Experiment 3-3
(3 input resolutions x FPS / CPU temp / memory / accuracy note).

This script does NOT run the detector itself — it assumes person_detector.py
is already running with a given resolution set in config.ini. What it does
is sample system + detector stats every few seconds and append them to a
CSV, so you get a clean table to average per resolution.

Typical flow per resolution:
    1. Edit config.ini -> [camera] width/height to the resolution you're
       testing (e.g. 320x240, 640x480, 1280x720).
    2. Restart person_detector.py.
    3. Run: python3 resolution_bench.py --label 640x480 --duration 300
    4. Repeat for the other two resolutions.

Output: resolution_results.csv with columns:
    timestamp, label, fps, cpu_temp_c, mem_used_mb, persons_detected

Afterwards, average fps/cpu_temp/mem_used per label for your table, and
note the accuracy behavior qualitatively (or cross-reference with
detection_logger.py samples taken at the same resolution) to conclude on
the optimal setting.
"""

import argparse
import csv
import json
import os
import time

import psutil  # pip install psutil

PERSONS_JSON_DEFAULT = "/dev/shm/surveillance/persons.json"
CSV_PATH_DEFAULT = "resolution_results.csv"
THERMAL_ZONE = "/sys/class/thermal/thermal_zone0/temp"


def read_cpu_temp_celsius() -> float:
    """Reads SoC temperature directly from the kernel thermal zone.
    Value is in millidegrees C."""
    try:
        with open(THERMAL_ZONE, "r") as f:
            return int(f.read().strip()) / 1000.0
    except FileNotFoundError:
        return float("nan")


def read_persons(path: str):
    with open(path, "r") as f:
        return json.load(f)


def find_detector_process():
    """Best-effort lookup of the running person_detector.py process for a
    per-process memory reading. Falls back to system-wide memory if not found."""
    for proc in psutil.process_iter(["pid", "name", "cmdline"]):
        cmdline = " ".join(proc.info.get("cmdline") or [])
        if "person_detector.py" in cmdline:
            return proc
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True,
                         help="Name for this resolution, e.g. 320x240")
    parser.add_argument("--duration", type=int, default=300,
                         help="Total seconds to sample for (default 300 = 5 min)")
    parser.add_argument("--interval", type=float, default=5.0,
                         help="Seconds between samples")
    parser.add_argument("--persons-json", default=PERSONS_JSON_DEFAULT)
    parser.add_argument("--csv", default=CSV_PATH_DEFAULT)
    args = parser.parse_args()

    proc = find_detector_process()
    if proc is None:
        print("[resolution_bench] Warning: could not find a running "
              "person_detector.py process — memory readings will fall back "
              "to system-wide used memory instead of per-process.")

    file_exists = os.path.exists(args.csv)
    with open(args.csv, "a", newline="") as csvfile:
        writer = csv.writer(csvfile)
        if not file_exists:
            writer.writerow(["timestamp", "label", "fps", "cpu_temp_c",
                              "mem_used_mb", "persons_detected"])

        elapsed = 0
        while elapsed < args.duration:
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            temp = read_cpu_temp_celsius()

            fps = float("nan")
            persons = float("nan")
            if os.path.exists(args.persons_json):
                data = read_persons(args.persons_json)
                fps = data.get("fps", float("nan"))
                persons = data.get("count", float("nan"))

            if proc is not None and proc.is_running():
                mem_mb = proc.memory_info().rss / (1024 * 1024)
            else:
                mem_mb = psutil.virtual_memory().used / (1024 * 1024)

            writer.writerow([timestamp, args.label, fps, round(temp, 1),
                              round(mem_mb, 1), persons])
            csvfile.flush()

            print(f"[{elapsed:>4}s] fps={fps} temp={temp:.1f}C "
                  f"mem={mem_mb:.1f}MB persons={persons}")

            time.sleep(args.interval)
            elapsed += args.interval

    print(f"Done. Results appended to {args.csv}")


if __name__ == "__main__":
    main()
