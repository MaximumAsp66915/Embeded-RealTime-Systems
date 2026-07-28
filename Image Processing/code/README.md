# Step 2 — Image Processing

Person detection module: reads a camera (or fallback video), detects people,
overlays student ID / timestamp / FPS / bounding boxes, and publishes the
result so the C code can consume it without touching OpenCV.

## Files

| File                    | Purpose                                                          |
|-------------------------|-------------------------------------------------------------------|
| `config.ini`             | All tunables — camera source, resolution, detector backend, IPC paths |
| `person_detector.py`     | Main detection loop                                               |
| `requirements.txt`       | Python deps                                                       |
| `detection_logger.py`    | Helper for Experiments 3-1 (lighting) and 3-2 (spoof test)         |
| `resolution_bench.py`    | Helper for Experiment 3-3 (resolution sweep)                       |

---

## Setup

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

> On the Orange Pi (ARM), a prebuilt `opencv-python` wheel may not exist for
> your Python version. If `pip install opencv-python` fails, use the distro
> package instead: `sudo apt install python3-opencv` and skip that line in
> `requirements.txt` (psutil still installs fine via pip).

Edit `config.ini`:
- Set `student_id` under `[general]`
- Set `source_type = camera` (or `video` + `video_path` if no camera is attached)
- Leave `backend = hog` unless you specifically want to try MobileNet-SSD (needs model files, see comments in the config)

## Running

```bash
python3 person_detector.py --config config.ini
```

This runs continuously, writing:
- `/dev/shm/surveillance/frame.jpg` — latest annotated frame
- `/dev/shm/surveillance/persons.json` — `{"count": ..., "timestamp": ..., "fps": ...}`

**Why `/dev/shm`:** it's a RAM-backed filesystem, so reads/writes are fast
and you're not wearing down the SD card with constant JPEG writes. Your C
web server and REST API can just open these two files — no sockets, no
extra dependencies, and it's easy to explain/diagram in the report.

Set `show_window = true` in `config.ini` if you have a monitor attached and
want a live preview; leave it `false` when running headless via systemd.

---

## Casting the laptop webcam to the Orange Pi (and results back)

Files: `scripts/laptop_stream_webcam.sh`, `scripts/laptop_camera_server.py`,
`scripts/pi_stream_result.sh`, `scripts/frame_viewer_server.py`,
`scripts/laptop_view_result.sh`.

### Why the original UDP command failed, and why HTTP-video also failed

```
ffmpeg -f v4l2 -i /dev/video0 -vf "scale=320:240" -vcodec libx264 ... -f mpegts "udp://192.168.0.170:5000?pkt_size=1316"
```

sends a raw MPEG-TS/UDP stream, and `person_detector.py` originally opened
it with `cv2.VideoCapture(url, cv2.CAP_FFMPEG)`. That failed outright with:

```
VIDEOIO(FFMPEG): backend is generally available but can't be used to capture by name
```

Switching to MJPEG-over-HTTP got past that error, but introduced a worse
problem: `cv2.VideoCapture` (and separately, plain `ffplay`) would connect
successfully and then **freeze on the very first frame** — FPS numbers
kept ticking (that's just loop timing, not frame freshness) but the actual
picture never changed. This happened on *both* directions we tried it
(laptop -> Pi and Pi -> laptop), which points at a structural issue: the
FFmpeg's MJPEG demuxer is unreliable at continuously parsing a *live*
MJPEG source (as opposed to a finite recorded file) over HTTP, whether the
video is muxed as `mjpeg`, `mpjpeg`, or served through a hand-written
multipart server.

**The fix used here: drop video codecs entirely, on both legs.** Instead
of streaming video, each side just serves the single most recent JPEG
frame over plain HTTP, and the other side asks for it repeatedly:

- **Laptop -> Pi:** `laptop_camera_server.py` captures the webcam locally
  with OpenCV (reliable — no network involved in the capture itself) and
  serves `GET /latest.jpg` with whatever frame it captured most recently.
  `person_detector.py` polls that URL on every read via the new
  `HttpPollCapture` class (`source_type = http_poll` in `config.ini`)
  instead of opening a network video stream.
- **Pi -> laptop:** `frame_viewer_server.py` serves `GET /latest.jpg` from
  the annotated `frame.jpg` that `person_detector.py` already writes to
  `/dev/shm/surveillance`. The laptop opens a small HTML page
  (`laptop_view_result.sh`) that re-fetches that URL every ~150ms via
  JavaScript.

Neither side ever asks FFmpeg (or OpenCV's FFmpeg backend) to parse a
live network video stream, so there's no muxer/demuxer state to get stuck
in — every request is a fresh, independent "give me the current bytes."

### Step-by-step

1. **On the laptop**, serve the webcam:
   ```bash
   ./scripts/laptop_stream_webcam.sh 0 320 240 8080
   ```
   (first arg can be a camera index like `0`, or a path like `/dev/video0`)
   This prints the URL to use, e.g. `http://192.168.0.50:8080/latest.jpg`.

2. **On the Orange Pi**, point `config.ini` at that URL:
   ```ini
   [camera]
   source_type = http_poll
   http_url = http://192.168.0.50:8080/latest.jpg
   ```
   Then run detection as usual:
   ```bash
   python3 person_detector.py --config config.ini
   ```

3. **Still on the Orange Pi**, serve the annotated result so the laptop
   can watch it:
   ```bash
   ./scripts/pi_stream_result.sh /dev/shm/surveillance/frame.jpg 8090
   ```
   This prints a URL like `http://192.168.0.170:8090/`.

4. **Back on the laptop**, watch the processed video:
   ```bash
   ./scripts/laptop_view_result.sh 192.168.0.170 8090
   ```
   This opens the live page in your default browser. If you're accessing
   the laptop remotely through VNC, run this script on the laptop as
   normal — the browser window shows up inside your VNC session like any
   other window on that desktop.

You'll end up with four terminals total: laptop-webcam-server,
pi-person_detector, pi-result-server, laptop-result-viewer. Run each in its
own terminal (or tmux pane) since they're all long-running/blocking.

### Verifying each leg independently

Before blaming the whole pipeline, check each leg on its own:

- **Is the laptop's webcam server producing fresh frames?** Open
  `http://<laptop-ip>:8080/latest.jpg` directly in a browser and reload a
  few times — the image should visibly change each time you're in frame.
- **Is the Pi actually receiving fresh frames?**
  ```bash
  watch -n 1 cat /dev/shm/surveillance/persons.json
  ```
  `timestamp`/`fps` changing only tells you the *loop* is running — to
  confirm the *picture* is actually changing, look at `frame.jpg` itself
  (copy it off with `scp` a couple of times a few seconds apart, or open
  `http://<pi-ip>:8090/latest.jpg` directly and reload).
- **Is the return leg updating?** Reload
  `http://<pi-ip>:8090/latest.jpg` in a browser a few times the same way.

### Troubleshooting

- **`cv2` not found running `laptop_stream_webcam.sh`:** install it —
  `pip install opencv-python` (this is the same dependency already listed
  in `requirements.txt`, so your existing venv should already have it if
  you set it up per the Setup section above).
- **Webcam server won't open the device / wrong camera selected:** list
  available cameras with `v4l2-ctl --list-devices`, and pass the right
  index or `/dev/videoN` path as the first argument.
- **Nothing connects / times out:** confirm both devices are on the same
  network/subnet and that no firewall is blocking the chosen ports
  (`sudo ufw allow 8080`, `sudo ufw allow 8090` if `ufw` is active).
- **`person_detector.py` exits immediately with "Could not reach
  <url>":** the laptop's `laptop_stream_webcam.sh` isn't running yet, or
  the IP/port in `config.ini`'s `http_url` doesn't match what it printed.
  Always start the laptop server first, confirm the URL in a browser,
  *then* start `person_detector.py`.
- **High latency / choppy updates:** lower `--width`/`--height`/`--fps`
  passed to `laptop_camera_server.py`, or raise `--quality` compression
  (lower number = smaller/faster, more compression artifacts).

---

## How the C side should consume this

- **Video stream endpoint**: read `frame.jpg` on each request/loop iteration and serve it as part of an MJPEG stream.
- **Person count / telemetry endpoints**: read and parse `persons.json` (it's tiny — a simple JSON parse, or even manual string extraction, is fine in C).
- Both files are written atomically (temp file + rename), so you will never read a half-written JPEG or JSON — no locking needed on the C side.

---

## Running the experiments

### 3-1: Accuracy under 3 lighting conditions

For each condition (daylight, low light, backlight):

1. Get `person_detector.py` running with people in frame
2. In a second terminal: `python3 detection_logger.py --label daylight --samples 20 --interval 2`
3. For each sample, look at the scene and type in how many people are actually there when prompted
4. Repeat for `--label low_light` and `--label backlight`

This appends to `results.csv`. Afterwards, build your report table:

| Condition  | Correct | Total | Accuracy |
|------------|---------|-------|----------|
| Daylight   | (count rows where detected_count == actual_count) | (total rows) | ... |
| Low light  | ...     | ...   | ...      |
| Backlight  | ...     | ...   | ...      |

**Screenshot needed:** one representative annotated frame per condition
(grab from `frame.jpg` or the preview window) plus the `results.csv` snippet
or your computed table.

### 3-2: Spoof test (printed photo / phone screen)

```bash
python3 detection_logger.py --label spoof_test --samples 10 --interval 2
```

Hold a printed photo or a phone displaying a photo of a person in frame.
If `detected_count` reports a false positive (counts a photo as a real
person), that's your finding.

**Analysis to include in the report:** a plain HOG/SSD detector has no
notion of "real vs. flat image" — it just matches shape/gradient patterns,
so a photo of a person triggers the same features as an actual person.

**Proposed fixes to discuss** (pick one or more, you don't need to implement all):
- Depth sensing (stereo camera / IR depth sensor) to distinguish flat surfaces from real 3D subjects
- Motion-based liveness check (require slight movement over N frames before confirming a detection)
- Texture/reflection analysis (screens and glossy prints reflect light differently than skin/fabric)
- Combine detection with a secondary lightweight liveness classifier

**Screenshot needed:** the frame showing the false-positive detection box on the photo, plus your written analysis.

### 3-3: Resolution sweep

Pick 3 resolutions, e.g. `320x240`, `640x480`, `1280x720`. For each:

1. Edit `config.ini` → `[camera] width` / `height`
2. Restart `person_detector.py`
3. Run: `python3 resolution_bench.py --label 640x480 --duration 300`
   (5 minutes, matching the other experiments in the report)
4. Repeat for the other two resolutions — you'll end up with one shared `resolution_results.csv`

Average `fps`, `cpu_temp_c`, and `mem_used_mb` per label for your table.
For the accuracy column, either eyeball it during the run or cross-reference
with a `detection_logger.py` session done at the same resolution.

| Resolution | Avg FPS | Avg CPU Temp (°C) | Avg Memory (MB) | Accuracy note |
|------------|---------|--------------------|------------------|----------------|
| 320x240    | ...     | ...                | ...              | ...            |
| 640x480    | ...     | ...                | ...              | ...            |
| 1280x720   | ...     | ...                | ...              | ...            |

Conclude with which resolution you'd pick as the default and why (usually
a balance — e.g. 640x480 gets most of the accuracy at a fraction of the
CPU/temp cost of 720p).

**Screenshot needed:** your final table + one graph (FPS/temp/memory vs. resolution).

---

## Notes

- `resolution_bench.py` looks for a running `person_detector.py` process to
  read its exact memory footprint; if it can't find one, it falls back to
  reporting system-wide used memory (still fine for the report, just less
  precise).
- CPU temperature is read straight from
  `/sys/class/thermal/thermal_zone0/temp` — the same kernel interface your
  C telemetry code should use later in Part 2, so it's worth reusing that
  logic/path when you build the REST API.
- Both experiment scripts append rather than overwrite, so you can re-run
  them across sessions without losing earlier data — just remember to
  delete or rename the CSVs if you want a clean run.
