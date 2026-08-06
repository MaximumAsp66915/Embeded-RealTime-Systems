# Step 6 Experiments (4-1 through 4-4)

## Setup

```bash
cd experiments
pip install -r requirements.txt
```

`exp4_1` and `exp4_3` run from a laptop/PC (hit the REST API + MQTT over
the network, same as any external client). `exp4_4` runs **on the Pi
itself** (it uses `stress` locally and reads `control.json` directly off
disk). `exp4_2` can run from either.

---

## 4-1: Guard Mode demo -> video + report images

```bash
python3 exp4_1_guard_mode.py --host 192.168.0.107 --student-id 402101906 \
    --username board_client --password orangepi --duration-s 300
```

Start this **before** you start recording your demo video. It subscribes
to the alarm topic and gives you an exact timestamped log of every alarm
that fires — screenshot the terminal output or `results/4-1/alarm_log.csv`
alongside your video for the report. The video/screenshots themselves
you make by hand: toggle Guard Mode (dashboard button or
`curl -X POST .../api/v1/guard -d '{"enabled": true}'`), walk into
frame, show the alert arriving.

---

## 4-2: Black box -> screenshot of stored DB events

```bash
python3 exp4_2_blackbox.py --host 192.168.0.170
```

Pretty-prints the total count + recent events as a table (screenshot
the terminal directly), plus writes `results/4-2/events.csv` for a
cleaner table image if you'd rather open it in a spreadsheet. See the
script's own docstring for the equivalent direct-`sqlite3`-CLI commands
if you want a second, independent piece of evidence alongside the API
result.

---

## 4-3: Disconnect camera -> video + screenshots of watchdog reacting

```bash
python3 exp4_3_watchdog.py --host 192.168.0.170 --duration-s 180
```

Start this **before** you disconnect the camera (physically, or — for
this project's `http_poll` setup — kill `laptop_stream_webcam.sh`, same
effect). Leave it running through the disconnect, the ~30s watchdog
timeout, the restart, and reconnecting the camera. It logs exactly when
`persons.json` went stale and when fresh frames resumed, then pulls
`surveillance-imgproc`'s own `journalctl` logs afterward (via the bonus
`/api/v1/services/{name}/logs` endpoint from Step 4) as the actual
restart evidence — look for a timestamp gap / fresh "Started" line in
`results/4-3/imgproc_journalctl.txt`. Also worth checking
`journalctl -u surveillance-notifier` yourself for the
`[watchdog] no new frame for >Ns` line that triggered it.

---

## 4-4: Simulate high temp with Linux tools -> screenshots/logs of adaptive response

**Run this ON THE PI**, not from a laptop:

```bash
sudo apt install stress   # if not already installed
python3 exp4_4_thermal.py run --duration-s 300 --stress-workers 4
python3 exp4_4_thermal.py plot --results-dir results/4-4
```

`run` drives real CPU load with `stress` (baseline period, then stress,
then cooldown) while sampling `https://localhost/api/v1/telemetry` and
`control.json` together. `plot` produces a temp-vs-time graph with the
throttled state overlaid and prints the exact detected throttle/recover
transition timestamps.

**300s may not be enough** on a board that heats up slowly — Orange Pi
Zero-class SoCs can take a couple of minutes under load to actually
cross a 70°C default trigger threshold. If `plot` reports "No throttle
event observed," rerun with a longer `--duration-s`/`--stress-duration-s`
rather than assuming something's broken — check the temp curve in the
graph first; if it's still climbing at the end of the window, that's a
timing issue, not a thermal-management bug.
