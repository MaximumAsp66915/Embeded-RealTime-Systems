# Smart Surveillance System — Build Plan & Checklist
**Student ID: 402101906** | Platform: Orange Pi Zero Plus 2 (H5), Ubuntu 20.04 Focal

---

## 0. What you're actually building

One embedded system, three moving parts, talking to each other:

- **Orange Pi board** — runs everything: web server, REST API, sensor reading, MQTT client, email sender, security. Core logic must be in **C** (25% deduction per section if you use another language for the mandatory parts).
- **Image processing module** — detects people in the camera feed. Python/C++ is fine here (this is the one exception to the C rule, along with Swagger).
- **Your PC** — runs the MQTT broker (Mosquitto) and subscribes to see what the board is reporting.

**Non-negotiable security baseline** (applies everywhere, not tied to one part):
- SSH: key or password login only, **root login disabled**
- MQTT: no anonymous access, dedicated user + password
- No hardcoded passwords/API keys anywhere in the code

Grading is 25 / 25 / 25 / 25 across the four parts below, and every part has "mandatory experiments" — these are just as important as the build itself, since they're what you screenshot/video for the report. Treat each experiment as a deliverable, not an afterthought.

---

## Suggested build order (not the same as the grading order)

Building in grading order (Part 1 → 4) means you build the web server before you have anything to show on it. Build in this order instead:

1. **Foundation & security** — OS hardening, so it's done once and never revisited
2. **Image processing** — get person detection working standalone first
3. **Web server + SSL + systemd** — now you have a live feed to actually display
4. **REST API + Swagger** — wraps what already works
5. **MQTT + Email** — hook up notifications
6. **Advanced features** — Guard Mode, black box, watchdog, thermal management
7. **Run all mandatory experiments** — after the system is stable
8. **Write the report**

---

## Step 1 — Foundation & Security (underpins Part 1 & Part 3 grading)

- [x] Disable root SSH login (`PermitRootLogin no` in `sshd_config`)
- [x] Enforce key-based or password SSH auth (pick one, document your choice)
- [x] Install Mosquitto on the PC; disable anonymous access (`allow_anonymous false`)
- [x] Create a dedicated MQTT user + password (`mosquitto_passwd`)
- [x] Decide how secrets (MQTT password, email credentials) will be injected — env vars, a config file excluded from git, etc. **Never hardcoded.**
- [x] Generate the self-signed SSL cert with OpenSSL, **CN = your student ID**

**Experiments to run at the end of this step:**
- [x] 3-6: Attempt unauthorized MQTT login → screenshot the failure
- [x] 3-7: Attempt unauthorized SSH login → screenshot the failure

---

## Step 2 — Image Processing (feeds Part 3-A, but build it first)

- [x] Get a working camera/webcam feed (or a streamed sample video if the board has no camera)
- [x] Integrate a lightweight person-detection model
- [x] Draw bounding boxes + live person counter on the frame
- [x] Overlay: Student ID + live system date/time
- [x] Overlay: real-time FPS measurement
- [x] Expose the current person count somewhere your C code can read it (shared memory, socket, file — your choice, but keep it simple and documented)

**Experiments (can run once detection works, don't need the whole system yet):**
- [x] 3-1: Accuracy under daylight / low light / backlight — build a table
- [x] 3-2: Test with a printed photo or phone screen photo of a person — does it fool the detector? Write up analysis + a proposed fix
- [x] 3-3: Test 3 different input resolutions — table of FPS / temp / memory / accuracy, conclude on the optimal setting

---

## Step 3 — Web Server, SSL, systemd (Part 1 — 25 pts)

**Web server (C):**
- [x] HTML page, title includes name + student ID
- [x] Embeds the live camera stream
- [x] Shows live person count
- [x] Shows CPU temp, free memory, CPU usage — updating every 2 seconds

**SSL:**
- [x] Serve HTTPS only using the cert from Step 1
- [x] Plain HTTP requests get a 301 redirect to HTTPS

**systemd:**
- [x] Service files for: web server, image processing, MQTT client
- [x] `Restart=on-failure` (or similar) on each
- [x] Use `After=` / `Requires=` to enforce startup order (e.g. image processing after camera init) — draw this as a flowchart for the report

**Experiments:**
- [x] 1-1: `systemd-analyze blame` after reboot → boot time chart + critical services log
- [x] 1-2: `kill -9` the web server → journalctl screenshot showing auto-restart
- [x] 1-3: Full power cycle → video of unattended boot
- [x] 1-4: Hit the site over HTTP → screenshot of the 301 in browser devtools
- [x] 1-5: Open HTTPS page → screenshot of cert overview showing CN = student ID
- [x] 1-6: Open the HTML page → screenshot showing the student ID

---

## Step 4 — REST API + Swagger (Part 2 — 25 pts)

Core logic in C; FastAPI is allowed **only** as a thin documentation/gateway layer on top.

- [x] `GET /api/v1/stream` — MJPEG live video
- [x] `GET /api/v1/persons` — current count + timestamp
- [x] `GET /api/v1/telemetry` — CPU temp, free memory, CPU load % (read directly in C — **no shelling out to Linux commands**)
- [x] `POST /api/v1/command` — e.g. `{"cmd": "reboot"}`, designed so new commands can be added later without a rewrite
- [x] `GET /api/v1/history` — last 5 detection records
- [x] Wire all endpoints into Swagger UI, confirm each one actually executes and returns real data

**Experiments:**
- [x] 2-1: Sample temp every 30s for 5 min under idle / streaming-only / streaming+detection → 3-curve graph + min/max table + screenshot
- [x] 2-2: Memory usage over 5 min of continuous streaming, sampled every 5s → graph + leak analysis
- [x] 2-3: 50 concurrent curl requests to `/api/v1/telemetry` → graph temp/CPU/memory changes + latency analysis
- [x] 2-4: Kill network mid-stream, reconnect after 2 min → explain behavior, show logs, describe recovery

---

## Step 5 — MQTT + Email (Part 3-B/C — shares the 25 pts with Step 2's detection work)

**Email (C):**
- [x] On detecting ≥1 person, send email with: count, timestamp, CPU temp, attached frame
- [x] Debounce: max 1 email per 30 seconds — document the mechanism in the report

**MQTT (C client):**
- [x] Publish to `home/persons/<student_id>` and `home/telemetry/<student_id>`
- [x] JSON payloads: count/temp/timestamp as applicable
- [x] QoS = 1
- [x] Configure LWT so the PC is notified if the board drops suddenly

**Experiments:**
- [x] 3-4: Stop the broker, restart after 3 min → show the LWT message
- [x] 3-5: 10 samples of entry-to-MQTT-receipt latency → mean + standard deviation

---

## Step 6 — Advanced Features (Part 4 — 25 pts)

- [x] **Guard Mode** — toggle via API or page; while active, detection triggers immediate email + MQTT alert on `home/<student_id>/alarm`
- [x] **Black box logging** — SQLite, circular buffer, written via C API, queryable total-detection-count
- [x] **Software watchdog** — if no new frames for >30s, alert + log + email + restart the service
- [x] **Adaptive thermal management** — over-threshold CPU temp triggers automatic FPS/resolution reduction + alert email

**Experiments:**
- [x] 4-1: Guard Mode demo → video + report images
- [x] 4-2: Black box → screenshot of stored DB events
- [x] 4-3: Disconnect camera → video + screenshots of watchdog reacting
- [x] 4-4: Simulate high temp with Linux tools → screenshots/logs of adaptive response

---

## Step 7 — Report & Deliverables

- [x] All source code
- [x] PDF report: architecture explanation (include the systemd flowchart from Step 3), every table/graph above, results analysis, issues + how you resolved them
- [x] Requested test videos (1-3, 4-1, 4-3)
- [x] Config files: systemd units, Mosquitto config, SSL certs

---

## Quick-reference: everything that costs points if skipped

- C required for: web server, telemetry reading, REST API logic, command execution, email sending, MQTT client, black box, watchdog, thermal management
- Python/C++ allowed only for: image processing, Swagger/FastAPI doc layer
- Root SSH login must be disabled
- MQTT must require auth
- No hardcoded secrets anywhere
- SSL cert CN must literally be your student ID
- Telemetry must be read in C directly — not via shell commands
