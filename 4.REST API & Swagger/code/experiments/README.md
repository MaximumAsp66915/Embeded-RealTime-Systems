# Step 4 Experiments (2-1 through 2-4)

Run these from a laptop/PC on the same network as the Pi — they hit the
C server's REST API over HTTPS, same as `curl -sk https://<pi-ip>/...`.

## Setup

```bash
cd experiments
pip install -r requirements.txt
```

All scripts take `--host <pi-ip>` (required) and `--port` (default 443,
matching `server.conf`'s `https_port`). They tolerate the self-signed
cert the same way the gateway does (TLS verification off — trusted-
network coursework setup).

Each script has `sample`/`run`/`monitor` + `plot` (or similar)
subcommands: the first collects real data into a CSV under `results/`,
the second turns the CSV(s) into the graph/table the experiment asks
for. They're separate so you can re-run just the plotting step while
tweaking a graph, without re-collecting 5 minutes of real samples.

---

## 2-1: Temperature under 3 load conditions

```bash
# Run once per condition — YOU change what's running on the Pi between
# each command, the script just samples for 5 minutes and records it.

# Condition 1: idle — nothing else running but the C server itself.
python3 exp2_1_temperature.py sample --host 192.168.0.170 --label idle

# Condition 2: streaming-only — start laptop_stream_webcam.sh and open
# /api/v1/stream in a browser tab, but do NOT run person_detector.py.
python3 exp2_1_temperature.py sample --host 192.168.0.170 --label streaming

# Condition 3: streaming + detection — person_detector.py running too.
python3 exp2_1_temperature.py sample --host 192.168.0.170 --label streaming_detection

# Combine into the report graph + min/max table:
python3 exp2_1_temperature.py plot --results-dir results/2-1
```

Produces `results/2-1/exp2_1_temperature.png` (3-curve graph) and
`exp2_1_summary_table.csv` (also printed as a markdown table to stdout —
copy straight into your report). Take the "screenshot" the checklist
asks for from either the terminal output or the PNG itself.

---

## 2-2: Memory over 5 min of continuous streaming

```bash
# Start streaming FIRST (laptop_stream_webcam.sh + open /api/v1/stream
# somewhere), so it's actively running for the full 5 minutes below.
python3 exp2_2_memory.py sample --host 192.168.0.170

python3 exp2_2_memory.py plot --results-dir results/2-2
```

Produces `exp2_2_memory.png` and `exp2_2_leak_analysis.txt` (net change,
peak usage, linear trend slope in MB/min, and a plain-language verdict).
The slope is a screening heuristic, not proof — say so in your report
rather than treating a 5-minute window as conclusive either way; if the
slope does look suspicious, the honest next step is a longer run (30-60
min), not a stronger claim from this one.

---

## 2-3: 50 concurrent requests to /api/v1/telemetry

```bash
python3 exp2_3_concurrency.py run --host 192.168.0.170
python3 exp2_3_concurrency.py plot --results-dir results/2-3
```

`run` fires all 50 requests at once (thread pool), sampling telemetry
before the burst, immediately after, and again 10s later ("settled") so
you can see whether temp/CPU/memory actually spike under load and
recover. `plot` produces a latency histogram + a before/after/settled
bar-ish comparison, plus prints p50/p95/max latency and flags any failed
requests (a real concurrency limit, not just a latency number, if your
`https_server.c`'s thread-per-connection model chokes at 50 — worth
checking `MAX_THREADS`/similar in your config if so).

Want to test scaling instead of just the required 50? `--n 100
--concurrency 20` etc. work too.

---

## 2-4: Kill network mid-stream, reconnect after 2 min

This one genuinely needs you to physically kill the network (unplug
Ethernet / disable Wi-Fi on whichever side matches your setup) — no
script can do that part. Two terminals, both started before you kill
the network:

```bash
# Terminal 1 — lightweight liveness poll (~1s interval), for the clean
# up/down timeline graph:
python3 exp2_4_network_recovery.py monitor --host 192.168.0.170 --duration-s 400

# Terminal 2 — keeps the actual MJPEG stream open, logs exactly when it
# breaks and when a reconnect attempt succeeds:
python3 exp2_4_network_recovery.py stream-watch --host 192.168.0.170 --duration-s 400
```

Then: let both run normally for ~30s, kill the network, wait roughly 2
minutes, reconnect. Let both scripts run to completion on their own
(`--duration-s 400` gives comfortable margin on both sides of a 2-minute
outage — bump it if your outage runs long).

Afterward, once the network is back:

```bash
# Pull the C server's own journalctl output covering the outage window —
# this is the "show logs" part of the checklist item, straight from the
# real systemd journal via the bonus /api/v1/services/{name}/logs endpoint.
python3 exp2_4_network_recovery.py fetch-logs --host 192.168.0.170

# Turn the liveness poll into the timeline graph + detected outage window:
python3 exp2_4_network_recovery.py plot --results-dir results/2-4
```

`plot` prints the detected outage start/end and total downtime — compare
that against your own stopwatch/clock timing of when you actually pulled
and restored the network as a sanity check, and note any gap between
them in your writeup (e.g. TCP timeout delay before the client notices
the drop is expected and worth explaining, not a bug).

For "describe recovery": look at `stream_watch_log.csv`'s
`connection_lost` / `retrying_connect` / `connected` events — that's
your client-side reconnect behavior — alongside whatever the
`surveillance-web` logs show (or don't show — an outage on the network
layer often produces no C-server-side log entries at all, since the
process itself never crashed or restarted; that absence is itself worth
explaining in the report: the server kept running throughout, it was
only unreachable).
