# Sensor Log API — Design Report (Section 5)

## 1. Purpose

Sections 1-4 expose the **latest** value of each sensor through an SNMP
pass-protocol bridge (cache → local DB → MQTT cascade). Section 5 adds a
complementary, HTTP-based way to answer a different question: *"show me
everything sensor X recorded on date Y"* — a multi-row, historical query
that doesn't fit the single-latest-value shape of the SNMP path.

## 2. Why it's embedded in the Master, not a separate program

Per the project's constraint that every component is driven entirely by
its own `run.sh` with nothing run manually outside of it, the API is
compiled directly into `master_node` (`src/api.cpp`, alongside the
existing `src/main.cpp`) and started as a third `std::thread` from
`main()`, next to the existing SNMP bridge listener thread and MQTT
listener thread. `master/run.sh` builds and starts it automatically —
there is no separate binary, no separate service, and no extra manual
step. `Ctrl+C` on the master stops the SNMP listener, the MQTT listener,
and the API together, cleanly, via the same `g_running` flag and signal
handler already in place.

## 3. Technology choice

- **Mongoose** (`mongoose.c` / `mongoose.h`), as required by the spec —
  a single-file, dependency-free embedded HTTP server. `master/run.sh`
  fetches it automatically on first run (mirroring how it already
  installs system packages via `apt`), and caches it in `src/` for
  subsequent runs.
- **SQLite** (`libsqlite3`), already a dependency of `master_node`. The
  API reuses the exact same `sensors` / `sensor_readings` tables the
  SNMP path already queries — **no new tables, no seeding script,
  no schema changes.** The data (as sampled: `sensor_id, sensor_type,
  sensor_name, location, value, unit, recorded_at`) is already present
  in `master.db` / `slave1.db` / `slave2.db` before the API ever runs.
- Written in **C++17** (`api.cpp`), compiled together with Mongoose's C
  source (`gcc` for `mongoose.c`, `g++` for `main.cpp`/`api.cpp`, linked
  into one binary) — the `Makefile` was extended with a C compile rule
  alongside the existing C++ rule.

## 4. Request structure

```
GET /api/logs?sensor_id=<id>&sensor_type=<type>&date=<YYYY-MM-DD>
```

| Parameter     | Type   | Required | Validation                          |
|---------------|--------|----------|--------------------------------------|
| `sensor_id`   | string | yes      | non-empty, numeric characters only   |
| `sensor_type` | string | yes      | non-empty, alphanumeric characters only |
| `date`        | string | yes      | must match `YYYY-MM-DD`              |

All three are standard URL query parameters, parsed with Mongoose's
`mg_http_get_var()`. `sensor_id` is validated as numeric, `sensor_type`
as alphabetic, and `date` against a strict regex before ever reaching a
SQL query, rejecting malformed input early with a `400`.

The lookup is keyed on **both** `sensor_id` and `sensor_type` together —
a row only matches if a sensor with that exact id has that exact type.
`sensor_name` is no longer a request input; it's resolved from the DB
and only echoed back in the response, since it's derived data rather
than something the caller needs to already know. `sensor_type` was
chosen as the disambiguating field instead, since it protects against a
sensor_id ever being reused for a different kind of sensor — the lookup
fails safely (treated as "not found") rather than silently returning the
wrong sensor's data.

## 5. Response structure

### 5.1 Success (rows found)

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

`values` is an array of `{time, value}` pairs, ordered chronologically
(ascending `recorded_at`), matching the sample output in the spec.
HTTP status: `200`.

### 5.2 No data for that date (sensor exists, but nothing recorded)

```json
{
  "sensor_name": "Floor1_Room101_Temp",
  "sensor_id": "101",
  "date": "2026-06-02",
  "values": [],
  "message": "No recorded values found for sensor 'Floor1_Room101_Temp' (id=101) on 2026-06-02."
}
```

HTTP status: `200` — the request itself was well-formed and successfully
answered; the *answer* is "nothing recorded."

### 5.3 Unknown sensor (wrong `sensor_id`, wrong `sensor_type`, or genuinely not found anywhere in the cluster)

Same shape as 5.2, but the message states no sensor was found with that
`sensor_id` + `sensor_type` combination "anywhere in the cluster" — this
fires only after the master's own DB *and* both slaves (via the MQTT
fallback, §6) have all reported a miss. HTTP status: `404`.

### 5.4 Bad input

```json
{ "error": "Invalid date. Expected format is YYYY-MM-DD." }
```

HTTP status: `400`. Distinct messages are returned for missing
parameter(s), a non-numeric `sensor_id`, and a malformed `date`.

### 5.5 Server-side failure

```json
{ "error": "Unable to open the sensor database on this node." }
```

HTTP status: `500` — only if the SQLite file itself can't be opened
(e.g. `DB_PATH` misconfigured).

All responses are pretty-printed JSON with `Content-Type:
application/json`, built manually (no JSON library dependency) with a
small escaping helper to keep quotes/backslashes/control characters
safe inside string values.

## 6. Data reading path

```
HTTP GET /api/logs?sensor_id=&sensor_type=&date=
        │
        ▼
Mongoose event loop, running on its own thread inside master_node
(ev_handler → MG_EV_HTTP_MSG)
        │
        ▼
handle_logs_request()
   ├─ validate sensor_id / sensor_type / date
   ▼
query_sensor_logs(db_path, sensor_id, sensor_type, date)   -- LOCAL DB, this node only
   ├─ sqlite3_open_v2(DB_PATH, READONLY)
   ├─ SELECT sensor_name FROM sensors
   │      WHERE sensor_id = ? AND sensor_type = ?
   │      → confirms the sensor exists on THIS node, fetches its name
   ├─ SELECT time(recorded_at), value
   │    FROM sensor_readings
   │    WHERE sensor_id = ? AND date(recorded_at) = ?
   │    ORDER BY recorded_at ASC
   │      → every reading for that sensor on that calendar day
   │
   ├─ found here?  ──yes──▶ build_success_json() / build_message_json()
   │
   └─ not found on this node
        ▼
   fetch_logs_from_slaves_mqtt(sensor_id, sensor_type, date, corr_id)
   ├─ publish "cluster/slave/logs_request" {id, sensor_type, date, correlation_id}
   ├─ mqtt_listen_loop() (main.cpp) resolves the matching pending request
   │    when a slave replies FOUND on "cluster/slave/logs_response",
   │    or after all slaves report NOT_FOUND / a 1.5s timeout elapses
   ├─ each slave, independently: same two SELECTs as above, against its
   │    OWN slave1.db / slave2.db
   ▼
found on a slave?  ──yes──▶ parse_values_string() → build_success_json()
   │
   no ──▶ build_message_json() (404)
        │
        ▼
mg_http_reply() → JSON response back to the client
```

The database is opened **read-only** (`SQLITE_OPEN_READONLY`) on every
request — a deliberate simplicity/safety choice for a reporting
endpoint: the API can never write to or corrupt any node's database, and
no connection pooling or locking strategy is needed. SQLite handles
concurrent readers natively, and each node's own cascade
(`resolve_sensor_record` / `handle_incoming_request`) remains the only
writer path (through Memcached caching and, indirectly, whatever process
originally populates `sensor_readings`).

The cluster fallback reuses the master's existing MQTT connection and
pending-request table (declared in `main.cpp`, shared via `cluster.h`)
rather than opening a second connection — it's a second topic pair
(`cluster/slave/logs_request` / `cluster/slave/logs_response`) layered on
the same request/wait/timeout mechanism the SNMP path's
`fetch_from_slaves_mqtt()` already uses, not a separate system. It's a
genuinely separate cascade from the SNMP one, though: a historical,
date-scoped, multi-row query doesn't fit the single-latest-value shape
the SNMP path answers, so it isn't routed through Memcached at all — each
slave reads its own SQLite directly for every request.

## 7. Deliverables checklist

| Requirement                                        | Location                          |
|-----------------------------------------------------|-------------------------------------|
| API code in C/C++ using Mongoose                     | `master/src/api.cpp` (+ `api.h`)   |
| Receives sensor identity + date as input        | `sensor_id` + `sensor_type` + `date` query params (see note below) |
| Returns recorded values for that date                  | `values` array                      |
| Data read from the database                              | `query_sensor_logs()` + cluster MQTT fallback |
| Specific, readable response structure                     | JSON schema above (§5)             |
| Appropriate message when no data exists                     | §5.2 / §5.3                        |
| Makefile to compile                                            | `master/Makefile` (extended)       |
| Bash script to compile and run                                    | `master/run.sh` (extended)         |
| Execution/testing guide in Markdown                                  | `master/API.md`                    |
| This design report                                                     | `master/API_REPORT.md`             |

> **Deviation from the assignment's literal sample I/O:** the spec's
> sample input is `sensor_name`, `sensor_id`, `date`. This implementation
> instead takes `sensor_id`, `sensor_type`, `date` — `sensor_name` is
> resolved from the DB and returned rather than supplied — per an
> explicit design change requested during development. If the grading
> rubric checks the exact input parameter names, this will need to be
> reconciled (e.g. by keeping `sensor_name` as an additional optional
> echo-back parameter) before submission.

No separate DB seeding script is included: the master and slave
databases were already populated with real sensor data before this
section, and the API is a pure reader on top of that existing data.
