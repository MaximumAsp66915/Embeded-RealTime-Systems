# Smart Surveillance System — Orange Pi Zero Plus 2 (H5)

An embedded, real-time person-surveillance system built end-to-end on an
Orange Pi Zero Plus 2 (H5): live camera stream, on-board person
detection, an HTTPS web dashboard, a REST API with Swagger docs, MQTT +
e-mail notifications, and a set of advanced daemon features (Guard Mode,
black-box logging, a software watchdog, and adaptive thermal
management). Core logic is written in C by design — Python is used only
where the assignment explicitly allows it (image processing, Swagger's
FastAPI gateway, and the standalone experiment scripts used to gather
report data).

## What this project actually does

- Detects people in a live camera feed on-board and overlays a
  bounding box, person count, student ID, timestamp, and FPS.
- Serves that feed and live system telemetry (CPU temp, memory, load)
  over an HTTPS web dashboard, secured with a self-signed SSL
  certificate and hardened SSH/MQTT access.
- Exposes the same data and controls through a documented REST API
  (Swagger UI), including an extensible, safety-gated command
  dispatcher.
- Publishes detections and telemetry over MQTT and sends debounced
  e-mail alerts (with the triggering frame attached) when someone is
  detected.
- Adds a "Guard Mode" alarm path, a SQLite black box that logs every
  event with a never-trimmed total-detections counter, a watchdog that
  restarts the detector if the camera feed goes stale, and hysteresis-based
  thermal throttling that trades frame rate/resolution for CPU
  temperature under load.
- Every stage was validated with its own set of measured experiments
  (boot time, TLS/auth failure handling, detection accuracy under
  different lighting, concurrency/load testing, network-recovery
  behavior, thermal behavior with and without a fan, etc.) — all of
  which are documented, with data, in the final report.

## What you need before starting

- An Orange Pi Zero Plus 2 (H5) (or similar SBC) running Ubuntu/Armbian,
  with a USB camera or a laptop willing to act as a streamed camera
  source, and a second machine (PC/laptop) on the same network to run
  the MQTT broker and the experiment scripts.
- A C toolchain (`gcc`/`make`) and OpenSSL on the board; Python 3 with
  `pip` on whichever machine runs the image-processing script and the
  experiment scripts.
- `libmosquitto` and `libcurl` (development headers) for the MQTT/e-mail
  daemon; `sqlite3` development headers for the black box.
- An SMTP account for outgoing alert e-mail (e.g. Gmail with an app
  password) and a Mosquitto MQTT broker with authentication enabled.
- Nothing hardcoded: secrets (MQTT password, SMTP credentials) are
  supplied through local, git-ignored config files (`server.conf`,
  `notifier.conf`) — copy the template values in each section's `code/`
  folder and fill in your own.

## How to start

1. Read `PreProvided/Humanized-project-Claude_sonnet5.md` for the full
   build plan and grading breakdown — it explains the recommended build
   order, which differs from the numbered section order below.
2. Start with **`1.Foundation & Security/`** and run `secure_setup.sh`
   on both machines (board and PC) to harden SSH, set up authenticated
   MQTT, generate the SSL certificate, and lay down the secrets files
   the later sections read from.
3. Work forward through sections **2 → 6** in order — each section's own
   `code/README.md` has that section's specific build/run instructions,
   and each depends on the shared files (`frame.jpg`, `persons.json`,
   `control.json`, etc.) written by the section(s) before it.
4. Each section's `code/experiments/` folder (where present) contains
   the scripts used to reproduce that section's measured results; each
   has its own `README.md` with exact usage.
5. The full write-up, with every figure, table, and analysis, is in
   `Document/Report.pdf`.

## Project structure

```
.
├── 1.Foundation & Security/         Part 1 baseline: SSH hardening, authenticated MQTT,
│   ├── code/                        self-signed SSL cert, secrets handling.
│   │   ├── secure_setup.sh              <- run this on both the board and the PC
│   │   └── README.md
│   └── results/                     Screenshots/logs for this section's experiments (3-6, 3-7).
│
├── 2.Image Processing/              On-board person detection (Python, the one section
│   ├── code/                        where the assignment allows a non-C language).
│   │   ├── person_detector.py           <- main detection loop, writes frame.jpg + persons.json
│   │   ├── fused_tracker.py             <- detection/tracking logic
│   │   ├── detection_logger.py
│   │   ├── resolution_bench.py          <- resolution/FPS/temp/accuracy benchmarking
│   │   ├── config.ini
│   │   ├── cascades/                    <- Haar cascade models (face/profile/upper-body)
│   │   ├── scripts/                     <- laptop-side camera server + viewer helper scripts
│   │   └── README.md
│   └── results/                     Accuracy-under-lighting tables, spoof-test results,
│                                     resolution benchmark data (light-results/, resolution-results/).
│
├── 3.Web Server, SSL & systemd/     Part 1: the C web server + dashboard.
│   ├── code/
│   │   ├── src/                         <- https_server.c, mjpeg_stream.c, sysinfo.c, etc.
│   │   ├── systemd/                     <- unit files (web, imgproc, mqtt), Restart=on-failure
│   │   ├── server.conf                  <- edit this (ports, paths, cert location)
│   │   ├── Makefile
│   │   └── README.md
│   └── results/                     Boot-time, kill/restart, HTTPS-redirect, and cert
│                                     experiment evidence.
│
├── 4.REST API & Swagger/            Part 2: thin FastAPI/Swagger layer over the same C server.
│   ├── code/
│   │   ├── src/                         <- api_router.c, command_dispatch.c, history_log.c,
│   │   │                                    guard_mode.c, blackbox_reader.c, service_ctl.c, ...
│   │   ├── gateway/main.py              <- FastAPI app, typing/Swagger only, zero business logic
│   │   ├── experiments/                 <- exp2_1..2_4 scripts + their results/ data
│   │   ├── server.conf, Makefile
│   │   └── README.md
│   └── results/                     Experiment 2-1..2-4 evidence (temperature, memory,
│                                     concurrency, network recovery).
│
├── 5.MQTT & Email/                  Part 3: standalone notifier daemon.
│   ├── code/
│   │   ├── src/                         <- mqtt_client.c, email_notifier.c, main.c, ...
│   │   ├── experiments/                 <- exp3_4 (LWT), exp3_5 (latency) + results/
│   │   ├── notifier.conf                <- edit this (MQTT + SMTP credentials, never commit real values)
│   │   ├── Makefile
│   │   └── README.md
│   └── results/                     LWT and pub/alert-latency experiment evidence.
│
├── 6.Advanced Features/             Part 4: Guard Mode, black box, watchdog, thermal management —
│   ├── code/                        all added to the Part 3 notifier daemon rather than as new processes.
│   │   ├── src/                         <- guard_state.c, black_box.c, watchdog.c, thermal.c, ...
│   │   ├── experiments/                 <- exp4_1..4_4 scripts + results/ data
│   │   ├── notifier.conf
│   │   ├── Makefile
│   │   └── README.md
│   └── results/                     Experiment 4-1..4-4 evidence (Guard Mode, black box,
│                                     watchdog vs. disconnected camera, thermal fan/no-fan).
│
├── Document/
│   └── Report.pdf                   The full written report — every section, figure, table,
│                                     and experiment analysis referenced above.
│
├── PreProvided/
│   ├── Final_Proj_Embedded.pdf          <- original assignment brief, as given
│   └── Humanized-project-Claude_sonnet5.md  <- build plan / checklist derived from the brief
│
└── README.md                        This file.
```

Within each numbered section, `results/fig/` holds the screenshots cited
in the report; a `results/fig-gitignore/` folder (where present) holds
additional screenshots that include a live camera frame, a personal
e-mail address, or other personally identifying content — these back
the same experiments but are deliberately excluded from anything
published or shared outside this local copy. Likewise, a handful of
demonstration videos referenced in the report (e.g. an unattended
power-cycle/boot video) were recorded but are **not included** in this
copy of the project, for the same privacy reason — the corresponding
`results/video-gitignore/` folders are present but effectively empty.
As a direct consequence, `Document/Report.pdf` itself has a small number
of figures/videos omitted from what's cited in the text.

## Notes for whoever picks this up next

- Every `server.conf` / `notifier.conf` in this tree is a **local**
  copy with real values filled in — treat them as secrets, not
  templates, before pushing or sharing further.
- Sections 3 → 6 build on each other's shared files under
  `/dev/shm/surveillance/` (`frame.jpg`, `persons.json`,
  `control.json`, `guard_state.json`) and `blackbox.db` — they're
  meant to run together as systemd services, not in isolation.

## References

- **Author:** Mohammadhossein Sabzalian
- **Email:** Mohammad.sabzalian83@gmail.com
- **Date:** August 9, 2026
- **Repository:** [github.com/MaximumAsp66915/Embeded-RealTime-Systems](https://github.com/MaximumAsp66915/Embeded-RealTime-Systems/tree/Smart-Surveillance-System(Orange-Pi-Zero))
