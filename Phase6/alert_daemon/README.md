# Alert Daemon — Execution & Usage Guide (Section 6)

`alert_daemon` is a **standalone binary**, separate from `master_node` and
`slave_node`. It is deployed **identically on all three nodes** (master,
slave1, slave2) — the same compiled program, pointed at whichever node's
own local SQLite DB via `DB_PATH`. It never talks to MQTT, another node,
or another node's database; each instance only ever watches its own
`sensors` / `sensor_readings` tables and writes to its own `alerts` table
in that same local DB file. This mirrors how `master_node` and
`slave_node` already only know about their own sensors.

## 1. How to compile the Daemon

```bash
cd alert_daemon
make clean && make
```

This produces the `alert_daemon` binary in the current directory, built
from `src/main.cpp` against `libsqlite3` only (`-lsqlite3`) — no
Memcached, no MQTT, no Mongoose; this daemon has none of those
dependencies.

`./run.sh` also does this for you (see item 3) — installing
`libsqlite3-dev`/`sqlite3` if missing, then running `make clean && make`
— so a manual `make` is only needed if you want to compile without also
launching the daemon.

## 2. How to install the service file

Nothing to do by hand here — `./run.sh` (item 3) generates the unit file
itself, with `WorkingDirectory=` / `EnvironmentFile=` / `ExecStart=` all
pointed at wherever you actually checked out `alert_daemon/` on this
node, then installs it to `/etc/systemd/system/alert_daemon.service`,
runs `daemon-reload`, and `enable`s it. The `alert_daemon.service` file
shipped in this directory is just a reference copy of what gets
generated; you never need to copy, edit, or `daemon-reload` it manually.

## 3. How to run the Daemon

Everything — installing deps, prompting for `DB_PATH` and every
threshold, writing `env`, compiling, installing/enabling the systemd
unit, and starting the service — is done by a single command, run once
per node:

```bash
cd alert_daemon
./run.sh
```

Sample prompts (defaults shown; press Enter to accept):

```
Enter local SQLite database path for THIS node [../master.db]:
Enter poll interval in seconds [10]:
Enter high-temperature threshold in degrees [35.0]:
Enter minimum allowed humidity percent [20.0]:
Enter maximum allowed humidity percent [70.0]:
Enter sensor timeout in seconds (no new reading = alert) [300]:
Enter minimum sane recorded value (sanity floor) [-50.0]:
Enter maximum sane recorded value (sanity ceiling) [1000.0]:
```

Point `DB_PATH` at **this node's own DB** — `../master.db` on the master,
`../slave1.db` on slave 1, `../slave2.db` on slave 2 — exactly as you
would for `master/run.sh` / `slave/run.sh`.

Once the prompts are answered, `run.sh` builds the binary, writes
`/etc/systemd/system/alert_daemon.service`, runs `daemon-reload`,
`enable`s the service (so it also starts on boot), `restart`s it
(starts it if not already running, or picks up a freshly rebuilt binary
if it was), and finally prints `systemctl status` plus a cheat-sheet of
the `systemctl`/`journalctl` commands from items 5 and 6 below. Re-run
`./run.sh` any time you change a threshold or rebuild — it's idempotent.

## 4. How to stop the Daemon

```bash
sudo systemctl stop alert_daemon
```

`Type=simple` + `Restart=on-failure` means a crash gets restarted
automatically, but an explicit `stop` is respected and does not trigger
a restart.

## 5. How to view the status of the Daemon

```bash
sudo systemctl status alert_daemon
```

Shows whether it's `active (running)`, `inactive (dead)`, or
`failed`, its PID, uptime, and the last few log lines.

## 6. How to view system logs

```bash
sudo journalctl -u alert_daemon -f      # follow live
sudo journalctl -u alert_daemon -n 100  # last 100 lines
```

You'll see the same `[STARTUP]` / `[ALERT][OPEN]` / `[ALERT][CLEAR]` /
`[SHUTDOWN]` lines that print to stdout when run via `./run.sh` in the
foreground — systemd captures them into the journal automatically.

Example:

```
[STARTUP] Initializing Alert Daemon...
[STARTUP] DB_PATH=../master.db
[STARTUP] POLL_INTERVAL_SEC=10
[STARTUP] TEMP_HIGH_MAX=35
[STARTUP] HUMIDITY_MIN=20 HUMIDITY_MAX=70
[STARTUP] SENSOR_TIMEOUT_SEC=300
[STARTUP] VALUE_MIN=-50 VALUE_MAX=1000
[STARTUP] alerts table ready (CREATE TABLE IF NOT EXISTS). Entering poll loop.
[ALERT][OPEN] HIGH_TEMPERATURE sensor_id=101 (Floor1_Room101_Temp) value=36.9
[ALERT][CLEAR] HIGH_TEMPERATURE sensor_id=101 (Floor1_Room101_Temp)
```

## 7. How to check registered alerts in the database

Directly with the `sqlite3` CLI, against this node's own DB:

```bash
sqlite3 ../master.db "SELECT id, sensor_id, sensor_name, alert_type, sensor_value, created_at, status FROM alerts ORDER BY id DESC LIMIT 20;"
```

Only currently-active alerts:

```bash
sqlite3 ../master.db "SELECT * FROM alerts WHERE status = 'active';"
```

For a quick demo without waiting for a real condition to occur, seed a
few sample rows first:

```bash
cd alert_daemon
./seed_alerts.sh ../master.db
```

(`./seed_alerts.sh` with no argument falls back to `$DB_PATH` or the
`DB_PATH` already written into `./env` by `run.sh`.)

## 8. Explanation of the alert generation conditions

The daemon polls every `POLL_INTERVAL_SEC` seconds (default `10`). For
every row in this node's own `sensors` table, it looks up the most
recent `sensor_readings` row and checks four conditions:

| # | Alert type | Applies to | Condition |
|---|---|---|---|
| 1 | `HIGH_TEMPERATURE` | `sensor_type = 'temperature'` | latest value `> TEMP_HIGH_MAX` (default `35.0`) |
| 2 | `HUMIDITY_OUT_OF_RANGE` | `sensor_type = 'humidity'` | latest value `< HUMIDITY_MIN` or `> HUMIDITY_MAX` (default `20.0` – `70.0`) |
| 3 | `SENSOR_TIMEOUT` | every sensor | no `sensor_readings` row at all, or the latest one is older than `SENSOR_TIMEOUT_SEC` (default `300`) |
| 4 | `INVALID_VALUE` | every sensor with a reading | the recorded value isn't a parseable number, or is numeric but outside the generic sanity range `[VALUE_MIN, VALUE_MAX]` (default `-50.0` – `1000.0`) |

Notes on precedence and edge cases:

- `INVALID_VALUE` is checked before, and takes precedence over, the
  type-specific threshold checks: if a reading is garbage (unparseable,
  or absurdly out of range), the daemon does not also claim it "proves" a
  confirmed high-temperature or bad-humidity reading. Only a valid,
  in-sanity-range number is evaluated against `TEMP_HIGH_MAX` /
  `HUMIDITY_MIN`/`HUMIDITY_MAX`.
- `SENSOR_TIMEOUT` is evaluated independently of all the others and is
  the only condition that can fire when there is no reading at all.
- **Dedupe / logging strategy**: a sustained violation does **not**
  produce a new `alerts` row every poll cycle. The daemon keeps an
  in-memory table of which (sensor, condition) pairs currently have an
  open (`status = 'active'`) row. The first poll where a condition
  becomes true inserts one new row; it stays untouched while the
  condition remains true; the poll where it stops being true flips that
  same row's `status` to `'resolved'`. This keeps `alerts` a log of
  *episodes* rather than a per-tick spam log — `SELECT * FROM alerts
  WHERE status='active'` directly answers "what's wrong right now." The
  cost: the table doesn't record how many poll cycles a violation
  lasted, and this state is in-memory only, so restarting the daemon
  while a condition is still active logs it again as if it were new
  (there is no startup reconciliation against existing `'active'` rows).
  See `ALERTS_REPORT.md` for the full design writeup.
