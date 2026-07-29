#!/usr/bin/env python3
"""
detection_logger.py

Smart Surveillance System — helper for Experiments 3-1 and 3-2.

These two experiments are inherently manual: you need a human to say how
many people were ACTUALLY in frame at a given moment, then compare that to
what the detector reported. This script just automates the tedious part —
polling persons.json and appending a timestamped row to a CSV — so you can
sit in front of the camera, glance at the terminal, and type the ground
truth without missing samples.

Usage:
    python3 detection_logger.py --label daylight --samples 20 --interval 2
    python3 detection_logger.py --label spoof_test --samples 10 --interval 2

Each run appends to results.csv (created if missing) with columns:
    timestamp, label, detected_count, actual_count, notes

Workflow for 3-1 (lighting conditions):
    Run once per condition with --label daylight / low_light / backlight.
    After collecting all three, open results.csv and build:
      - a Correct/Total table per condition (Correct = detected == actual)

Workflow for 3-2 (spoof test):
    Hold up a printed photo or phone screen showing a person in frame,
    run with --label spoof_test, and see whether detected_count reports
    a false positive. Add your analysis + proposed fix as notes or directly
    in the report (e.g. depth/liveness check, IR sensor, motion-based
    filtering to reject static images).
"""

import argparse
import csv
import json
import os
import time

PERSONS_JSON_DEFAULT = "/dev/shm/surveillance/persons.json"
CSV_PATH_DEFAULT = "results.csv"


def read_persons(path: str):
    with open(path, "r") as f:
        return json.load(f)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True,
                         help="Condition name, e.g. daylight / low_light / backlight / spoof_test")
    parser.add_argument("--samples", type=int, default=20,
                         help="How many samples to collect")
    parser.add_argument("--interval", type=float, default=2.0,
                         help="Seconds between samples")
    parser.add_argument("--persons-json", default=PERSONS_JSON_DEFAULT)
    parser.add_argument("--csv", default=CSV_PATH_DEFAULT)
    args = parser.parse_args()

    if not os.path.exists(args.persons_json):
        raise SystemExit(
            f"Could not find {args.persons_json} — is person_detector.py running?"
        )

    file_exists = os.path.exists(args.csv)
    with open(args.csv, "a", newline="") as csvfile:
        writer = csv.writer(csvfile)
        if not file_exists:
            writer.writerow(["timestamp", "label", "detected_count", "actual_count", "notes"])

        print(f"Collecting {args.samples} samples for label='{args.label}', "
              f"every {args.interval}s. Look at the camera scene and answer "
              f"the prompt each time.")

        for i in range(args.samples):
            data = read_persons(args.persons_json)
            detected = data["count"]

            actual_raw = input(
                f"[{i+1}/{args.samples}] detected={detected}  "
                f"-> how many people are ACTUALLY in frame right now? "
            ).strip()
            actual = int(actual_raw) if actual_raw.isdigit() else ""

            notes = input("  optional note (enter to skip): ").strip()

            writer.writerow([data["timestamp"], args.label, detected, actual, notes])
            csvfile.flush()

            time.sleep(args.interval)

    print(f"Done. Results appended to {args.csv}")


if __name__ == "__main__":
    main()
