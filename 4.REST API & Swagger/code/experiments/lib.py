"""
experiments/lib.py — shared helpers for the Step 4 experiment scripts.

Every experiment script hits the C server's /api/v1/telemetry (and
sometimes /persons) endpoint over HTTPS with a self-signed cert, so this
centralizes: the requests session setup (TLS verification off, matching
the gateway's own C_SERVER_VERIFY_TLS=false reasoning — same trusted-
network coursework setup, not a production TLS chain), one retrying GET
helper, and a tiny CSV writer so each experiment script stays focused on
its own sampling logic instead of re-implementing this every time.
"""
import csv
import time
import warnings
from dataclasses import dataclass, fields
from pathlib import Path
from typing import Optional

import requests
from requests.packages.urllib3.exceptions import InsecureRequestWarning

warnings.simplefilter("ignore", InsecureRequestWarning)


def base_url(host: str, port: int = 443) -> str:
    return f"https://{host}:{port}"


def get_json(url: str, timeout: float = 5.0) -> Optional[dict]:
    """GET url, return parsed JSON on success, None on any failure (timeout,
    connection refused, non-200, bad JSON) — experiments should keep
    sampling through transient failures rather than crash, especially for
    2-4 which deliberately induces one."""
    try:
        resp = requests.get(url, verify=False, timeout=timeout)
        if resp.status_code != 200:
            return None
        return resp.json()
    except requests.RequestException:
        return None


@dataclass
class TelemetrySample:
    elapsed_s: float
    cpu_temp_c: float
    cpu_percent: float
    mem_free_mb: float
    mem_total_mb: float
    ok: bool  # False if the request failed — row still recorded, values will be 0


def sample_telemetry(host: str, port: int, start_time: float) -> TelemetrySample:
    data = get_json(f"{base_url(host, port)}/api/v1/telemetry")
    elapsed = time.time() - start_time
    if data is None:
        return TelemetrySample(elapsed, 0.0, 0.0, 0.0, 0.0, ok=False)
    return TelemetrySample(
        elapsed_s=round(elapsed, 2),
        cpu_temp_c=data.get("cpu_temp_c", 0.0),
        cpu_percent=data.get("cpu_percent", 0.0),
        mem_free_mb=data.get("mem_free_mb", 0.0),
        mem_total_mb=data.get("mem_total_mb", 0.0),
        ok=True,
    )


def write_csv(path: Path, rows: list):
    """Writes a list of dataclass instances (all the same type) to a CSV,
    header from the dataclass's own field names."""
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    field_names = [f.name for f in fields(rows[0])]
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(field_names)
        for row in rows:
            writer.writerow([getattr(row, name) for name in field_names])
    print(f"[lib] wrote {len(rows)} rows -> {path}")


def read_csv_rows(path: Path) -> list:
    with open(path, newline="") as f:
        return list(csv.DictReader(f))
