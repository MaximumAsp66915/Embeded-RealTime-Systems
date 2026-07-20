# Distributed Sensor Cluster — Full Project (Phases 1–6)

## Layout

```
project/
├── master/     # Master node: SNMP bridge + cache/DB/MQTT cascade (Phases 1-4)
│               # PLUS the Section 5 Sensor Log API, embedded in the same binary
│   ├── src/main.cpp    Master engine (SNMP bridge listener, MQTT listener)
│   ├── src/api.cpp     Section 5: Mongoose HTTP API (embedded, own thread)
│   ├── src/api.h
│   ├── src/cluster.h   Shared MQTT helpers (logs cascade fallback to slaves)
│   ├── (src/mongoose.c / mongoose.h - fetched automatically by run.sh)
│   ├── Makefile         builds main.cpp + api.cpp + mongoose.c into master_node
│   ├── run.sh            installs deps, fetches Mongoose, compiles, runs everything
│   ├── API.md             Section 5 execution & testing guide
│   └── API_REPORT.md       Section 5 design report
├── slave/      # Slave node: local cache/DB responder over MQTT (Phases 1-4)
├── client/     # Laptop-side SNMP benchmark script (Phases 1-4)
├── alert_daemon/  # Section 6: standalone watchdog binary, deployed identically
│               # on master, slave1, AND slave2 -- each instance only monitors
│               # its own node's local sensors/sensor_readings via its own DB_PATH
│   ├── src/main.cpp        Poll loop; 4 alert conditions; active/resolved dedupe
│   ├── Makefile            builds alert_daemon (links libsqlite3 only)
│   ├── run.sh              installs deps, prompts for config, compiles, runs
│   ├── alert_daemon.service  systemd unit (Type=simple, Restart=on-failure)
│   ├── seed_alerts.sh      inserts sample alert rows for demo purposes
│   ├── ALERTS.md           Section 6 execution & usage guide (8-item checklist)
│   └── ALERTS_REPORT.md    Section 6 design report
└── docs/
    └── ARCHITECTURE.md   # Diagrams + explanation of the full data flow
```

## Setup (per the project's strict "run.sh only" rule)

Each component is self-contained and driven entirely by its own
`run.sh` — install deps, configure, compile, and run all happen inside
that one script, with no other manual commands required on the target
machine. The Section 5 API is **not** a separate component: it starts
automatically as part of `master/run.sh`. The Section 6 Alert Daemon
**is** a separate component (`alert_daemon/`) — it must be run in
addition to `master/run.sh` or `slave/run.sh` on each node, since it's a
standalone binary rather than a thread inside `master_node`/`slave_node`.

| Machine | Command |
|---|---|
| Master (192.168.56.101) | `cd master && ./run.sh` — starts SNMP bridge, MQTT listener, **and** the Sensor Log API; separately, `cd alert_daemon && ./run.sh` (`DB_PATH=../master.db`) for alerting |
| Slave 1 | `cd slave && ./run.sh`; separately, `cd alert_daemon && ./run.sh` (`DB_PATH=../slave1.db`) for alerting |
| Slave 2 | `cd slave && ./run.sh`; separately, `cd alert_daemon && ./run.sh` (`DB_PATH=../slave2.db`) for alerting |
| Laptop (benchmark) | `cd client && ./value_test.sh` |

See `docs/ARCHITECTURE.md` for how the SNMP/cache/DB/MQTT pieces connect,
`master/API.md` + `master/API_REPORT.md` for the Section 5 API's usage
and design, and `alert_daemon/ALERTS.md` + `alert_daemon/ALERTS_REPORT.md`
for the Section 6 Alert Daemon's usage and design.


# Cluster Architecture — Phases 1 through 6

This document shows how the pieces built so far (SNMP bridge, Master
engine, Slave engines, Memcached, Mosquitto/MQTT, and the client
benchmark script) fit together, before the Phase 5 HTTP API is added.

```mermaid
flowchart TB
    subgraph CLIENT["Client machine (laptop)"]
        VT["value_test.sh\nsnmpget / snmpwalk"]
    end

    subgraph MASTER["Master node (192.168.56.101)"]
        SNMPD["snmpd\nUDP :161\npass .1.3.6.1.4.1.9999"]
        BRIDGE["snmp_pass.sh\n(/usr/local/bin)"]
        MNODE["master_node\n(master_node binary)"]
        SNMPLISTEN["SNMP bridge listener\nUDP 127.0.0.1:1161\n(snmp_listener_loop)"]
        MQTTLOOP["MQTT listener thread\n(mqtt_listen_loop)\nsub: cluster/slave/response"]
        RESOLVE["resolve_sensor_record()\nCache -> Local DB -> MQTT cascade"]
        MCACHE_M[("Memcached\n:11211")]
        MDB[("master.db\nsensors / sensor_readings")]
        BROKER{{"Mosquitto broker\n:1883"}}
    end

    subgraph SLAVE1["Slave node 1"]
        S1["slave_node"]
        S1CACHE[("Memcached\n:11211")]
        S1DB[("slave1.db")]
    end

    subgraph SLAVE2["Slave node 2"]
        S2["slave_node"]
        S2CACHE[("Memcached\n:11211")]
        S2DB[("slave2.db")]
    end

    VT -- "snmpget/snmpwalk\n(SNMP v2c, UDP 161)" --> SNMPD
    SNMPD -- "exec: -g/-n <OID>" --> BRIDGE
    BRIDGE -- "'<mode>|<oid>' over UDP" --> SNMPLISTEN
    SNMPLISTEN --> MNODE
    MNODE --> RESOLVE

    RESOLVE -- "1) GET sensor_id" --> MCACHE_M
    MCACHE_M -- "hit -> return" --> RESOLVE
    RESOLVE -- "2) miss -> query" --> MDB
    MDB -- "found -> cache + return" --> RESOLVE
    RESOLVE -- "3) miss -> publish request\ncluster/slave/request\n{id, correlation_id}" --> BROKER

    BROKER -- "fan-out" --> S1
    BROKER -- "fan-out" --> S2

    S1 -- "check" --> S1CACHE
    S1 -- "fallback" --> S1DB
    S2 -- "check" --> S2CACHE
    S2 -- "fallback" --> S2DB

    S1 -- "publish response\ncluster/slave/response" --> BROKER
    S2 -- "publish response\ncluster/slave/response" --> BROKER
    BROKER -- "deliver by\ncorrelation_id" --> MQTTLOOP
    MQTTLOOP -- "resolve pending request\n(condition_variable)" --> RESOLVE

    RESOLVE -- "OID / TYPE / VALUE" --> SNMPLISTEN
    SNMPLISTEN -- "UDP response" --> BRIDGE
    BRIDGE -- "stdout" --> SNMPD
    SNMPD -- "SNMP response" --> VT
```

## Component summary

| Component | Role |
|---|---|
| `client/value_test.sh` | Benchmark script; runs `snmpget`/`snmpwalk` against the master's SNMP port, flushes Memcached before each round to measure cache-miss vs cache-hit latency. |
| `snmpd` (Master) | Native Net-SNMP daemon; delegates the custom `.1.3.6.1.4.1.9999` subtree to the pass-protocol bridge script. |
| `snmp_pass.sh` (Master) | Thin bridge: re-packages `-g`/`-n` OID requests as `mode|oid` and forwards them over UDP to the master engine's internal listener on `127.0.0.1:1161`. |
| `master_node` (Master) | Runs two threads: the SNMP bridge listener (`snmp_listener_loop`) and the MQTT response listener (`mqtt_listen_loop`). Resolves every sensor lookup through a **cache → local DB → MQTT cascade** (`resolve_sensor_record`). |
| Memcached (per node) | 300s TTL cache of encoded sensor records (`type\x1Fname\x1Flocation\x1Funit\x1Fvalue`), checked first on every lookup to avoid repeat DB/MQTT hits. |
| `master.db` / `slave*.db` | SQLite databases; `sensors` (metadata) joined with `sensor_readings` (latest value) on `sensor_id`. |
| Mosquitto broker (`:1883`) | MQTT transport between the master and both slaves; topics `cluster/slave/request` and `cluster/slave/response`, correlated by a per-request `correlation_id`. |
| `slave_node` (Slave 1 & 2) | Subscribes to `cluster/slave/request`; on a miss in its own cache, falls back to its own local SQLite DB; publishes a `FOUND`/`NOT_FOUND` response back on `cluster/slave/response`. |

## Request lifecycle (single SNMP GET)

1. Client issues `snmpget`/`snmpwalk` → `snmpd` (UDP 161).
2. `snmpd` executes `snmp_pass.sh -g/-n <oid>` per the pass-protocol config.
3. The bridge script forwards `mode|oid` over UDP to the master engine's internal listener (`127.0.0.1:1161`).
4. The master engine resolves the sensor_id embedded in the OID through `resolve_sensor_record()`:
   - Memcached hit → return immediately.
   - Miss → query local `master.db`. Hit → cache it, return.
   - Miss → publish an MQTT request to both slaves and block (with a timeout) on a per-request `correlation_id`.
5. Each slave checks its own cache, then its own local DB, and replies on `cluster/slave/response`.
6. The first slave to report `FOUND` resolves the pending request; if both report `NOT_FOUND`, the master resolves it as not found.
7. The result flows back: engine → bridge script → `snmpd` → client, as a 3-line `OID / TYPE / VALUE` pass-protocol answer (or nothing, which `snmpd` reports as "No Such Instance"/"End of MIB view").

## Addendum — Phase 5: embedded Sensor Log API

Section 5 adds a third thread inside `master_node` itself: a Mongoose
HTTP server that answers historical, date-scoped log queries. It matches
on `sensor_id` + `sensor_type` + `date` (not `sensor_name`). It checks
the master's own `master.db` first; on a miss it falls back to the
slaves over a second MQTT topic pair
(`cluster/slave/logs_request` / `cluster/slave/logs_response`), so one
call to the master answers for any sensor in the cluster — mirroring the
existing SNMP cascade's cache→DB→MQTT fallback, but bypassing Memcached
(a multi-row, date-scoped history isn't a single cacheable value). It
runs automatically as part of `master/run.sh` — no separate process, no
separate `run.sh`.

```mermaid
flowchart TB
    LAPTOP["Any HTTP client\n(curl / browser / Postman)"]

    subgraph MASTER["Master node (master_node process)"]
        T3["Thread: run_api_server (api.cpp)\nMongoose HTTP :8000\nGET /api/logs?sensor_id=&sensor_type=&date="]
        MDB[("master.db\nsensors 101-104")]
        T2B["mqtt_listen_loop\n(also handles logs_response)"]
    end

    subgraph SLAVE1["Slave node 1"]
        S1["slave_node\nhandle_incoming_logs_request()"]
        S1DB[("slave1.db\nsensors 201-204")]
    end

    subgraph SLAVE2["Slave node 2"]
        S2["slave_node\nhandle_incoming_logs_request()"]
        S2DB[("slave2.db\nsensors 301-304")]
    end

    BROKER{{"Mosquitto broker\n:1883"}}

    LAPTOP -- "GET /api/logs" --> T3
    T3 -- "1) local SELECT" --> MDB
    MDB -- "hit -> rows" --> T3
    T3 -- "2) miss -> publish\ncluster/slave/logs_request\n{id, sensor_type, date, correlation_id}" --> BROKER
    BROKER -- "fan-out" --> S1
    BROKER -- "fan-out" --> S2
    S1 -- "query own DB" --> S1DB
    S2 -- "query own DB" --> S2DB
    S1 -- "publish\ncluster/slave/logs_response" --> BROKER
    S2 -- "publish\ncluster/slave/logs_response" --> BROKER
    BROKER -- "deliver by correlation_id" --> T2B
    T2B -- "resolve pending request" --> T3
    T3 -- "JSON response" --> LAPTOP
```

## Addendum — Phase 6: Alert Daemon

Section 6 adds a **fourth, fully independent process** — `alert_daemon`
— deployed as the *same compiled binary* on all three nodes (master,
slave1, slave2). Unlike the Phase 5 API, it is not a thread inside
`master_node`; it is its own program with its own `run.sh` / `Makefile` /
systemd unit, and it never touches MQTT, Memcached, or any other node.
Each instance only ever polls its own node's local `sensors` /
`sensor_readings` tables (via its own `DB_PATH`) and writes into a new
`alerts` table in that same local DB — no cross-node awareness, no
aggregation, no shared alert view across the cluster.

```mermaid
flowchart TB
    subgraph MASTER["Master node"]
        MNODE["master_node\n(unchanged)"]
        MDB[("master.db\nsensors / sensor_readings\n+ alerts (new)")]
        MDAEMON["alert_daemon\nDB_PATH=../master.db"]
    end

    subgraph SLAVE1["Slave node 1"]
        S1["slave_node\n(unchanged)"]
        S1DB[("slave1.db\n+ alerts (new)")]
        S1DAEMON["alert_daemon\nDB_PATH=../slave1.db"]
    end

    subgraph SLAVE2["Slave node 2"]
        S2["slave_node\n(unchanged)"]
        S2DB[("slave2.db\n+ alerts (new)")]
        S2DAEMON["alert_daemon\nDB_PATH=../slave2.db"]
    end

    MNODE -- "writes sensor_readings\n(existing cascade)" --> MDB
    MDAEMON -- "poll every POLL_INTERVAL_SEC:\nSELECT sensors, sensor_readings" --> MDB
    MDAEMON -- "on violation/clear:\nINSERT / UPDATE alerts" --> MDB

    S1 -- "writes sensor_readings\n(existing cascade)" --> S1DB
    S1DAEMON -- "poll every POLL_INTERVAL_SEC" --> S1DB
    S1DAEMON -- "INSERT / UPDATE alerts" --> S1DB

    S2 -- "writes sensor_readings\n(existing cascade)" --> S2DB
    S2DAEMON -- "poll every POLL_INTERVAL_SEC" --> S2DB
    S2DAEMON -- "INSERT / UPDATE alerts" --> S2DB
```

Each `alert_daemon` instance, every `POLL_INTERVAL_SEC` seconds:

1. Loads every row from its own node's `sensors` table.
2. Looks up each sensor's most recent `sensor_readings` row (and its
   age, computed in SQL via `julianday()`).
3. Evaluates four conditions against it: high temperature, humidity out
   of range, sensor timeout (no reading within `SENSOR_TIMEOUT_SEC`),
   and an invalid recorded value (unparseable or out of a sanity range).
4. Logs a new `'active'` row in `alerts` the moment a condition starts
   being violated, and flips that row's `status` to `'resolved'` the
   moment it stops — a sustained violation does not produce a new row
   every poll cycle. See `alert_daemon/ALERTS_REPORT.md` for the full
   dedupe rationale and `alert_daemon/ALERTS.md` for the condition
   thresholds and their env-var overrides.
