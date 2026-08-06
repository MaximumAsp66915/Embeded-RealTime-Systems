# Step 6 — Advanced Features

Four features, spread across the two processes that already existed
(Step 4's web server, Step 5's notifier daemon) plus one small addition
to Step 2's Python detector — see "Architecture" below for why nothing
new is a third process.

| Feature | Lives in | 
|---|---|
| Guard Mode | Toggle: Step 4 (`api_router.c`, `index_page.c`). Enforcement: this daemon (`main.c`, reads what Step 4 wrote) |
| Black box (SQLite) | Write: this daemon (`black_box.c`). Read: Step 4 (`blackbox_reader.c`) |
| Watchdog | Entirely this daemon (`watchdog.c` + `service_ctl.c`) |
| Adaptive thermal | Decision: this daemon (`thermal.c`). Applied: Step 2's `person_detector.py` (mechanically, no thermal logic of its own) |

## Architecture — why one daemon, not four separate ones

Guard Mode, the watchdog, and thermal management all need to send email
alerts. Step 5 already had exactly one email-sending path with one
debounce clock enforcing "max 1 per 30s." Adding three MORE processes,
each with their OWN debounce timer, would mean the system as a whole
could send up to 4 emails in the same 30-second window — four
independently-rate-limited processes don't add up to one rate-limited
system. So all four triggers (plain detection, Guard Mode alarm,
watchdog timeout, thermal throttle) live in `main.c`'s poll loop and
funnel through the exact same `should_send_email()` — see that
function's own comment in `main.c` for the full reasoning, worth citing
directly in the report.

Black box logging is the opposite choice on purpose: it's NEVER
debounced. The email debounce limits *notifications*; the black box is
meant to be a complete record even during a debounce-suppressed burst —
every detection/alarm/watchdog/thermal event gets logged regardless of
whether an email actually went out for it.

### Cross-process state (same IPC pattern as `frame.jpg`/`persons.json` throughout this project)

- **`guard_state.json`** — Step 4 writes it (`POST /api/v1/guard`, or the
  dashboard's toggle button, which is just the same endpoint via JS
  fetch — "toggle via API or page" is one implementation, not two).
  This daemon only reads it.
- **`control.json`** — this daemon's `thermal.c` writes it whenever the
  throttle state changes. `person_detector.py` (Step 2) polls it every
  ~2s and mechanically applies `target_fps`/`processing_scale` — it
  makes NO thermal decisions of its own, just applies whatever numbers
  it's told.
- **`blackbox.db`** (SQLite, WAL mode) — this daemon writes it via the
  SQLite C API directly (`black_box.c` — never shells out to the
  `sqlite3` CLI). Step 4 opens the same file read-only for
  `GET /api/v1/blackbox/*`.

## Guard Mode

`POST /api/v1/guard {"enabled": true}` (or the dashboard button) arms
it. While armed, a new detection frame (`count >= 1`):
- Publishes **immediately** to `home/<student_id>/alarm`, QoS 1 — NOT
  debounced (only email is rate-limited by spec; "immediate" is the
  whole point of the alarm topic).
- Sends a `GUARD MODE ALARM` email through the shared debounce.
- Logs a `guard_alarm` event to the black box.

## Black box (SQLite)

Two things, deliberately separate:
- `events` table — a circular buffer, capped at `blackbox_max_events`
  (default 1000). Every insert past the cap deletes the oldest row(s)
  first.
- `meta.total_events_ever` — a counter that is NEVER trimmed. This is
  what answers "queryable total-detection-count" unambiguously — the
  `events` table alone can't answer "how many total" once it's wrapped
  around and started overwriting old rows.

Query it three ways:
```bash
# 1. Directly (on the Pi):
sqlite3 /opt/surveillance/notifier/blackbox.db "SELECT * FROM events ORDER BY id DESC LIMIT 20;"
sqlite3 /opt/surveillance/notifier/blackbox.db "SELECT value FROM meta WHERE key='total_events_ever';"

# 2. Via the REST API (from anywhere):
curl -sk https://192.168.0.170/api/v1/blackbox/count
curl -sk "https://192.168.0.170/api/v1/blackbox/events?limit=20"

# 3. Via the experiment helper (pretty-printed + CSV for the report):
python3 experiments/exp4_2_blackbox.py --host 192.168.0.170
```

## Watchdog

`person_detector.py`'s own loop never exits on a bad camera read (a
failed `cap.read()` just logs and `continue`s forever) — so systemd's
`Restart=on-failure` would never fire even with a genuinely dead feed.
This daemon notices from the outside: if `persons.json`'s timestamp
hasn't advanced in `watchdog_timeout_s` (default 30), it logs a
`watchdog_timeout` event, sends an alert email, and restarts
`watchdog_service_name` (default `surveillance-imgproc`) via
`service_ctl.c` — `execvp("systemctl", ...)` with a fixed argv array,
never a shell string, same safe pattern as Step 4's bonus service
endpoints.

## Adaptive thermal management

Hysteresis-based: `thermal_trigger_c` (default 70°C) to start throttling,
`thermal_recover_c` (default 65°C, deliberately LOWER — without that
gap, a temperature sitting right at one boundary would flip
throttled/normal every single poll cycle) to go back to normal. On
throttle: writes `control.json` with `thermal_throttled_fps`/
`thermal_throttled_scale`, logs a `thermal_throttle` event, sends an
alert email. On recovery: writes normal settings back, logs
`thermal_recover` — no email on recovery (spec ties the alert to the
triggering event, not the all-clear).

## Building and deploying

**Two things need rebuilding** — this is easy to miss, since the two
processes are usually deployed separately:

```bash
# 1. Step 4's web server (guard mode toggle + black box read endpoints):
cd "4.REST API & Swagger/code"
sudo apt install build-essential libssl-dev libsqlite3-dev
make clean && make
sudo cp surveillance_web /opt/surveillance/web/surveillance_web
# merge new keys into the deployed server.conf if you've customized it:
#   guard_state_path=  (blank = auto-derived from shared_dir)
#   blackbox_db_path=/opt/surveillance/notifier/blackbox.db
sudo systemctl restart surveillance-web

# 2. This daemon (Guard Mode enforcement, black box write, watchdog, thermal):
cd "6.Advanced Features/code"
sudo apt install build-essential libmosquitto-dev libcurl4-openssl-dev libsqlite3-dev
make clean && make
sudo cp surveillance_notifier /opt/surveillance/notifier/surveillance_notifier
# merge the new [Step 6] keys from this folder's notifier.conf into your
# deployed one if you've customized it (see notifier.conf's own comments)
sudo systemctl restart surveillance-notifier
```

**And Step 2's `person_detector.py`** needs the updated file (it now
polls `control.json` — see its module docstring and the new `[thermal]`
section in `config.ini`). No rebuild needed, it's Python — just restart
the service:
```bash
sudo systemctl restart surveillance-imgproc
```

## Verifying each feature actually works

```bash
# Guard Mode
curl -sk https://192.168.0.170/api/v1/guard
curl -sk -X POST https://192.168.0.170/api/v1/guard -d '{"enabled": true}'

# Black box
curl -sk https://192.168.0.170/api/v1/blackbox/count

# Watchdog — check the daemon's own log for the mechanism being armed
journalctl -u surveillance-notifier -f
# (then disconnect the camera and wait ~30s+ — see experiments/exp4_3)

# Thermal — check control.json exists and has sane defaults even before
# ever throttling:
cat /dev/shm/surveillance/control.json
```

## Experiments (4-1 through 4-4)

See `experiments/README.md`. One script per experiment, all tested
end-to-end against a mock server in development (not just syntax-
checked) — `exp4_1_guard_mode.py`, `exp4_2_blackbox.py`,
`exp4_3_watchdog.py`, `exp4_4_thermal.py`.
