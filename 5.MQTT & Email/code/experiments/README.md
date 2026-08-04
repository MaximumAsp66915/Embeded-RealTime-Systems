# Step 5 Experiments (3-4, 3-5)

## Setup

```bash
cd experiments
pip install -r requirements.txt
```

Both scripts run FROM A LAPTOP/PC — they play the role of "the PC" in
"notified if the board drops," connecting to the MQTT broker the same
way any external subscriber would (`mosquitto_sub`, a home automation
system, etc.).

If your broker requires auth (e.g. `secure_setup.sh`'s Mosquitto config
sets `allow_anonymous false`), pass `--username`/`--password` — same
credentials as `notifier.conf`'s `mqtt_username`/`mqtt_password` (from
that script's `secrets.env`). Omit both for an anonymous-access broker.

---

## 3-4: Stop the broker, restart after 3 min -> show the LWT message

```bash
python3 exp3_4_lwt.py --host 192.168.0.170 --student-id 402101906 --duration-s 500 --username board_client --password orangepi
```

Full instructions are in the script's own docstring — read it before
running, since the timing of when you run `systemctl stop/start
mosquitto` relative to the script matters. Short version: start the
script, let it run normally for ~30s, stop the broker (expect an
`offline` message almost immediately — that's Mosquitto delivering the
Last Will as part of its own shutdown, not a keepalive timeout you wait
out), wait ~3 minutes, restart the broker, watch both this script's own
subscriber and the Pi's notifier daemon reconnect on their own.

Output: `results/3-4/lwt_log.csv` — every message/connect/disconnect
event with a wall-clock timestamp, so you can line it up against exactly
when you ran each `systemctl` command in your report.

**If you see zero `offline` messages**: either the stop didn't happen
during the monitored window, or your Mosquitto's shutdown behavior
differs from the common case described above — check `journalctl -u
mosquitto` around the stop time to see what it actually did with
already-connected clients.

---

## 3-5: 10 samples of entry-to-MQTT-receipt latency

```bash
python3 exp3_5_latency.py --host 192.168.0.170 --student-id 402101906 --username board_client --password orangepi
```

Make sure `person_detector.py` is actively running and someone is in
frame (or moving in and out of frame) so new detection timestamps
actually keep arriving — this script waits for 10 genuinely NEW
`persons.json` timestamps published over MQTT, not just 10 repeated poll
cycles of the same detection.

**Read the resolution caveat in the script's own docstring before citing
the numbers in your report**: `persons.json` timestamps only have
1-second resolution, so this measurement has up to ~1s of quantization
noise on top of the real pipeline latency (poll interval + network
round-trip). The script still gives an honest *relative* latency figure;
just don't present it as more precise than it is. If you want to remove
that noise entirely, the actual fix is on the Step 2 side — have
`person_detector.py` additionally write a sub-second epoch float into
`persons.json` — worth mentioning as a "future work" line in the report
even if you don't implement it.

Also assumes the Pi's clock and your laptop's clock are reasonably
synced (both on NTP) and in a timezone the comparison makes sense for —
`persons.json`'s timestamp has no timezone marker, so it's compared
against your laptop's local time. The script flags any negative latency
sample (a strong sign of clock skew or a timezone mismatch) rather than
silently folding it into the average.

Output: `results/3-5/latency_samples.csv` (raw entry timestamp + latency
per sample) and `latency_summary.txt` (mean, stdev, min/max — paste
straight into the report).
