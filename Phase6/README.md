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
