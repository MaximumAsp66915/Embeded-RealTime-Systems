# Distributed Sensor Cluster — Full Project (Phases 1–5)

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
└── docs/
    └── ARCHITECTURE.md   # Diagram + explanation of the full data flow
```

## Setup (per the project's strict "run.sh only" rule)

Each component is self-contained and driven entirely by its own
`run.sh` — install deps, configure, compile, and run all happen inside
that one script, with no other manual commands required on the target
machine. The Section 5 API is **not** a separate component: it starts
automatically as part of `master/run.sh`.

| Machine | Command |
|---|---|
| Master (192.168.56.101) | `cd master && ./run.sh` — starts SNMP bridge, MQTT listener, **and** the Sensor Log API |
| Slave 1 | `cd slave && ./run.sh` |
| Slave 2 | `cd slave && ./run.sh` |
| Laptop (benchmark) | `cd client && ./value_test.sh` |

See `docs/ARCHITECTURE.md` for how the SNMP/cache/DB/MQTT pieces connect,
and `master/API.md` + `master/API_REPORT.md` for the Section 5 API's
usage and design.
