# Sensor Log API — Execution & Testing Guide (Section 5)

The Sensor Log API is **embedded inside the Master node** — it is not a
separate program. It is compiled into the same `master_node` binary as
the SNMP/cache/MQTT engine (Phases 1-4) and started as a second thread
alongside it, so it runs automatically whenever `master/run.sh` is run.
There is nothing extra to install, seed, or start.

It reads from the master's existing SQLite database (`sensors` +
`sensor_readings`, the same tables the SNMP path already queries) — no
new tables, no seeding script, no schema changes.

## 1. How to run it

Exactly as before — one script, on the master node:

```bash
cd master
./run.sh
```

`run.sh` now additionally:

1. Installs `wget` if missing (used to fetch Mongoose).
2. Downloads `mongoose.c` / `mongoose.h` into `src/` on first run if they
   aren't already vendored there (cached for subsequent runs — no
   re-download needed).
3. Prompts for an extra value: **HTTP port for the embedded Sensor Log
   API** (default `8000`), alongside the existing DB path / MQTT broker /
   sensor ID prompts.
4. Writes `API_PORT=<port>` into `env` alongside the other settings.
5. Compiles `src/main.cpp`, `src/api.cpp`, and `src/mongoose.c` together
   into the single `master_node` binary via the `Makefile`.
6. Runs `master_node` in the foreground — this starts the SNMP bridge
   listener, the MQTT listener, **and** the HTTP API, all in one process.

You'll see this in the startup log:

```
[STARTUP] Sensor Log API will listen on port 8000
...
[API] Sensor Log API starting on http://0.0.0.0:8000
[API] Reading from DB_PATH = ../master.db
[API] Endpoint: GET /api/logs?sensor_id=<id>&sensor_type=<type>&date=YYYY-MM-DD
```

Stop everything (SNMP, MQTT, API) at once with `Ctrl+C`.

## 2. Testing the API

Query it from any machine that can reach the master's IP and the chosen
port — your laptop, the `client/` VM, `curl`, a browser, or Postman.

### 2.1 Health check

```bash
curl "http://192.168.56.101:8000/api/health"
```

```json
{ "status": "ok", "service": "master-sensor-log-api" }
```

### 2.2 Successful lookup — sensor stored locally on the master

Using the sample data actually loaded on the master's `master.db`
(sensors 101-104):

```bash
curl "http://192.168.56.101:8000/api/logs?sensor_id=101&sensor_type=temperature&date=2026-06-01"
```

```json
{
  "sensor_name": "Floor1_Room101_Temp",
  "sensor_id": "101",
  "date": "2026-06-01",
  "values": [
    { "time": "10:00:00", "value": "24.2" },
    { "time": "10:15:00", "value": "24.8" }
  ]
}
```

Another example, this time a motion sensor:

```bash
curl "http://192.168.56.101:8000/api/logs?sensor_id=103&sensor_type=motion&date=2026-06-01"
```

```json
{
  "sensor_name": "Floor1_Corridor_Motion",
  "sensor_id": "103",
  "date": "2026-06-01",
  "values": [
    { "time": "10:10:00", "value": "0" },
    { "time": "10:16:00", "value": "1" }
  ]
}
```

### 2.3 Successful lookup — sensor owned by a slave

Sensors 201-204 live in `slave1.db`, 301-304 in `slave2.db`, not in the
master's own `master.db`. The master's API now reaches them too, by
falling back to the same cache → local DB → MQTT cascade pattern the SNMP
path already uses — just query the master, no need to know which node
actually stores the data:

```bash
curl "http://192.168.56.101:8000/api/logs?sensor_id=204&sensor_type=co2&date=2026-06-01"
```

```json
{
  "sensor_name": "Floor2_Meeting_CO2",
  "sensor_id": "204",
  "date": "2026-06-01",
  "values": [
    { "time": "10:05:00", "value": "710" },
    { "time": "10:20:00", "value": "735" }
  ]
}
```

This round-trip (master → MQTT → slave → MQTT → master) takes longer than
a local hit — expect tens of ms rather than single-digit ms, since it
waits on a real request/response over the broker rather than a local
SQLite read.

### 2.4 Sensor exists, but nothing recorded on that date

```bash
curl "http://192.168.56.101:8000/api/logs?sensor_id=101&sensor_type=temperature&date=2026-06-02"
```

```json
{
  "sensor_name": "Floor1_Room101_Temp",
  "sensor_id": "101",
  "date": "2026-06-02",
  "values": [],
  "message": "No recorded values found for sensor 'Floor1_Room101_Temp' (id=101) on 2026-06-02."
}
```

### 2.5 Unknown sensor_id / sensor_id + sensor_type mismatch

```bash
curl "http://192.168.56.101:8000/api/logs?sensor_id=999&sensor_type=temperature&date=2026-06-01"
```

Returns HTTP `404`:

```json
{
  "sensor_name": "",
  "sensor_id": "999",
  "date": "2026-06-01",
  "values": [],
  "message": "No sensor found with sensor_id=999 and sensor_type=temperature anywhere in the cluster."
}
```

The same 404 also fires if `sensor_id` is real but `sensor_type` doesn't
match it (e.g. `sensor_id=101&sensor_type=humidity`, since 101 is
actually a temperature sensor) — `sensor_type` is a hard match
requirement, not just a hint.

### 2.6 Missing or malformed input

```bash
curl "http://192.168.56.101:8000/api/logs?sensor_type=temperature&date=2026-06-01"
curl "http://192.168.56.101:8000/api/logs?sensor_id=abc&sensor_type=temperature&date=2026-06-01"
curl "http://192.168.56.101:8000/api/logs?sensor_id=101&sensor_type=temperature&date=06-01-2026"
```

All return HTTP `400` with an `{"error": "..."}` body explaining what was
wrong (missing parameter, non-numeric `sensor_id`, non-alphanumeric
`sensor_type`, or a `date` that isn't `YYYY-MM-DD`).

## 3. How the cluster-wide lookup works

Querying `master:8000/api/logs` now answers for **any** sensor in the
cluster, not just the ones physically stored in `master.db`:

1. The master checks its own `master.db` first (`sensor_id` +
   `sensor_type` must both match a row).
2. On a local miss, it publishes a `cluster/slave/logs_request` MQTT
   message and waits (up to 1.5s) for a slave to answer on
   `cluster/slave/logs_response`.
3. Each slave, on receiving that request, queries its **own** SQLite DB
   directly (`slave1.db` / `slave2.db`) for that `sensor_id` +
   `sensor_type` + `date` and replies `FOUND`/`NOT_FOUND`.
4. If no slave reports `FOUND`, the API returns 404.

This mirrors the existing SNMP path's cache → local DB → MQTT cascade,
just with a separate topic pair (`cluster/slave/logs_request` /
`cluster/slave/logs_response`) since a "give me everything on this date"
query returns multiple rows, not a single cacheable latest-value.

