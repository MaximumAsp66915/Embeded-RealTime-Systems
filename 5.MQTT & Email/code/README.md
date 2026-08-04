# Step 5 — MQTT + Email

A standalone C daemon (`surveillance_notifier`), separate from the Step
3/4 web server, that watches the same `persons.json`/`frame.jpg` Step 2's
`person_detector.py` writes, and:

- publishes to MQTT every poll cycle (`home/persons/<id>`,
  `home/telemetry/<id>`), QoS 1
- sends a debounced alert email (count + timestamp + CPU temp + attached
  frame) when it sees `count >= 1`
- sets up a Last Will and Testament (LWT) so external subscribers know if
  the board disappears without a clean disconnect

Built with `libmosquitto` (MQTT) and `libcurl` (SMTP + MIME attachment)
rather than hand-rolled protocol implementations or shelling out to
`mail`/`sendmail` — both are well-tested standard C libraries, which is
the appropriate scope for "write this in C" here (reimplementing
MQTT/SMTP/TLS/MIME by hand would just be reinventing these libraries,
worse).

## Why a separate daemon, not part of the Step 3/4 web server?

Different lifecycle and different failure modes: the web server should
keep serving `/api/v1/*` even if the MQTT broker or SMTP server is
unreachable, and this notifier should keep trying to publish/email even
if nobody's hitting the REST API. Keeping them as separate processes
(and separate systemd units) means a stuck SMTP connection or an MQTT
reconnect loop can't accidentally block the web server's request
handling, and vice versa.

## Building

```bash
sudo apt install build-essential libmosquitto-dev libcurl4-openssl-dev
cd code
make
```

## Configuring

Copy `notifier.conf`, fill in your real MQTT broker address and SMTP
credentials (see the file's own comments — Gmail needs an App Password,
not your normal password). **Don't commit the filled-in version.**

If you don't already have an MQTT broker, the simplest option is
Mosquitto on the Pi itself:
```bash
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable --now mosquitto
```
then leave `mqtt_host=localhost` in `notifier.conf`.

### Broker on a different machine (e.g. `secure_setup.sh`'s laptop-hosted Mosquitto)

If you ran Step 1's `secure_setup.sh` with MQTT setup on your laptop
instead of the board, the broker lives on the laptop, and the Pi
connects to it as a remote client:

1. Set `mqtt_host` in `notifier.conf` to the laptop's LAN IP (not
   `localhost` — that would mean "the Pi itself" instead).
2. `secure_setup.sh` sets `allow_anonymous false`, so the Pi needs to
   authenticate. It already wrote the credentials to a `secrets.env` on
   the laptop (`MQTT_USER=...` / `MQTT_PASS=...`) — copy those two
   values into `notifier.conf`'s `mqtt_username` / `mqtt_password`.
3. Confirm the laptop's firewall allows port 1883 from the Pi's IP if
   you have one active (`sudo ufw allow from <pi-ip> to any port 1883`
   on the laptop, or `sudo ufw allow 1883` for simplicity on a trusted
   LAN).

Quick manual test from the Pi before trusting the full daemon:
```bash
mosquitto_sub -h 192.168.0.107 -u <MQTT_USER> -P <MQTT_PASS> -t 'home/#' -v
```
If that hangs without connecting, it's a network/firewall/credentials
issue to resolve before `surveillance_notifier` will fare any better —
narrows down where to look.

## Running

```bash
./surveillance_notifier notifier.conf
```

Watch it work from another terminal:
```bash
mosquitto_sub -h localhost -t 'home/#' -v
```

## The debounce mechanism (for the report)

`main.c`'s `should_send_email()`:

```c
static int should_send_email(time_t now, time_t *last_email_epoch, int debounce_seconds) {
    if (*last_email_epoch != 0 && (now - *last_email_epoch) < debounce_seconds) {
        return 0;
    }
    *last_email_epoch = now;
    return 1;
}
```

`last_email_epoch` holds the wall-clock time of the last email actually
sent (0 if none yet). Every poll cycle where `count >= 1` AND the
detection frame's timestamp has genuinely advanced since the last cycle
(so polling faster than the detector updates can't double-count one
frame), this check runs. If fewer than `debounce_seconds` have passed,
the detection is simply **not emailed this cycle** — not queued, not
batched for later, just dropped for notification purposes (it's still
published to MQTT regardless; only the email is rate-limited, per spec).
The timestamp is updated **before** the email send itself (which can
take a moment over the network), so two poll cycles landing close
together can never both slip through while a send is in flight — the
slot is reserved first, sent second.

## LWT (Last Will and Testament)

Set in `mqtt_client.c`'s `mqtt_client_start()`, **before** connecting —
this ordering matters, since the broker only knows about a will if it's
part of the CONNECT packet:

```c
mosquitto_will_set(client->mosq, client->status_topic,
                    strlen("offline"), "offline",
                    1 /* QoS 1 */, true /* retained */);
```

If this process ever disappears without a clean disconnect (crash,
network drop, board loses power, keepalive timeout), the **broker**
delivers this `offline` message on `home/status/<student_id>` on our
behalf — that's the whole mechanism, and it's why the LWT has to be
configured before the connection is established rather than sent as a
normal publish. On a clean shutdown (SIGINT/SIGTERM), `mqtt_client_stop()`
instead publishes `offline` itself and then disconnects properly —
distinguishing "I'm going offline on purpose" from "I vanished," so a
subscriber watching the status topic can tell the difference between a
planned restart and an actual problem, if you want to note that
distinction in the report.

## MQTT topics and payloads

| Topic | QoS | Retained | Payload |
|---|---|---|---|
| `home/persons/<student_id>` | 1 | No | `{"count": int, "timestamp": str, "student_id": str}` |
| `home/telemetry/<student_id>` | 1 | No | `{"cpu_temp_c": float, "timestamp": str, "student_id": str}` |
| `home/status/<student_id>` | 1 | Yes | `"online"` or `"offline"` (plain string, not JSON — this project's own addition, carrying the LWT) |

QoS 1 ("at least once") per spec — a subscriber might see an occasional
duplicate persons/telemetry publish, but never silently misses one due
to a dropped packet, which matters more here than avoiding rare
duplicates would.

## Deploying under systemd

```bash
sudo mkdir -p /opt/surveillance/notifier
sudo cp surveillance_notifier notifier.conf /opt/surveillance/notifier/
# edit /opt/surveillance/notifier/notifier.conf with real credentials

sudo cp systemd/surveillance-notifier.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now surveillance-notifier
```

Check it's alive and follow its logs (useful while getting SMTP/MQTT
credentials right):
```bash
systemctl status surveillance-notifier
journalctl -u surveillance-notifier -f
```

## Experiments (3-4, 3-5)

See `experiments/README.md`. `exp3_4_lwt.py` walks through the broker
stop/restart LWT test; `exp3_5_latency.py` collects the 10 entry-to-
receipt latency samples with mean/stdev (and an honest caveat about the
1-second timestamp resolution this measurement inherits from
`persons.json` — read it before citing the numbers).

## Known limitations / things worth mentioning in the report

- **MQTT initial-connect failure was a permanent, unrecoverable state
  (found and fixed):** the very first version connected exactly once at
  startup — if that single attempt failed (a real symptom: `Network is
  unreachable` a few seconds after boot, from `After=network-online.target`
  not actually waiting for a real route on this board's image), MQTT was
  disabled for the entire remaining lifetime of the process, even once
  the network came up moments later. Fixed two ways: (1)
  `mqtt_client_start()` now retries the initial connect up to 8 times, 3s
  apart (~24s total), covering the typical "just booted, DHCP still
  settling" race; (2) `main.c`'s poll loop now retries
  `mqtt_client_start()` again every 60s for as long as `mqtt` is still
  NULL, covering a genuinely longer outage beyond that startup budget.
  Once actually connected, `mosquitto_loop_start()`'s background thread
  already handles reconnection-after-a-drop automatically (a separate,
  pre-existing mechanism) — these two fixes specifically close the gap
  around the *first* connection attempt, which that automatic mechanism
  doesn't cover.
- **Attachment corruption — two separate bugs found and fixed, both
  worth citing in the report:**
  1. `curl_mime_filedata()` doesn't set a `Content-Transfer-Encoding` by
     default — SMTP is a line-based 7/8-bit text protocol, so a lone
     `0x0A` byte occurring naturally inside binary JPEG scan data can get
     mangled by mail relays. Fixed with an explicit
     `curl_mime_encoder(attachment_part, "base64")` call.
  2. Even after fixing that, a second corruption still occurred —
     visually looking like two images overlapping. Root cause:
     `curl_mime_filedata()` doesn't read the file into memory upfront, it
     streams it lazily from disk *during* `curl_easy_perform()` (which
     for SMTP can take several seconds — TLS handshake, auth, DATA
     transfer). If `person_detector.py` rewrites `frame.jpg` (truncate +
     rewrite, not atomic) at any point during that window, curl can read
     a mix of leftover bytes from the old file and new bytes from its
     replacement — genuinely two partial JPEGs concatenated. Fixed in
     `email_notifier.c`'s `snapshot_frame_to_tempfile()`: read the whole
     frame into memory in one fast local `fread()` and write it to a
     private temp file *before* the network send starts, shrinking the
     race window from "however long SMTP takes" to "one fast local file
     copy." Not a hard guarantee — the fully correct fix is Step 2
     writing `frame.jpg` atomically (write-to-temp + `rename()`), which
     this project doesn't control — but this makes the corruption
     effectively unreproducible in practice, and is worth noting as a
     documented, understood residual risk rather than a claimed-fixed
     guarantee.
- **Email attachment failure doesn't block the email.** If
  `frame_path` can't be read for some reason, `email_notifier_send()`
  logs a warning and sends the email without the attachment rather than
  failing the whole send — a warning email with no picture is still more
  useful than no email at all when the count/timestamp/temp are the
  actual point.
- **`persons.json` timestamp resolution (1 second)** is a real,
  documented limitation for anything timing-sensitive built on top of
  it (see the 3-5 caveat above) — Step 2's detector doesn't currently
  write a sub-second value, and this project doesn't require it, but
  it's worth naming as a specific, fixable limitation rather than a
  vague "could be more precise."
