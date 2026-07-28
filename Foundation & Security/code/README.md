# secure_setup.sh — Step 1: Foundation & Security

Automates the security baseline for the Smart Surveillance System project:
SSH hardening, authenticated MQTT (Mosquitto), a self-signed SSL cert
(CN = student ID), and a `.env`-style secrets file so nothing is hardcoded.

---

## Where to run it

This script covers work that's split across **two machines**. Run it once per
machine, and just say "no" to the parts that don't apply:

| Machine        | Sections to say "yes" to        |
|-----------------|----------------------------------|
| Orange Pi board | SSH hardening, SSL cert, secrets |
| PC              | Mosquitto (MQTT broker), secrets |

You can technically run all four sections on one machine (e.g. for testing),
but for the real setup the board doesn't need to host Mosquitto and the PC
doesn't need the SSL cert.

---

## Usage

```bash
chmod +x secure_setup.sh
sudo ./secure_setup.sh
```

Must be run with `sudo` / as root — it edits `sshd_config`, installs
packages, and writes to system directories.

The script asks all its questions **up front**, then runs unattended. You'll
be asked:

1. Which sections to run (SSH / MQTT / SSL / secrets) — yes/no each
2. **SSH**: key-based or password auth (root login is disabled either way).
   If key-based, it asks for your public key and which user to authorize.
3. **MQTT**: username + password for the dedicated MQTT account
4. **SSL**: The **student ID** (becomes the cert's CN), validity length,
   and where to store the cert
5. **Secrets**: where to write the secrets file, and optionally an email
   address/password for the notification feature you'll build in Part 3

---

## What it does, section by section

### SSH hardening
- Backs up your existing `sshd_config` before touching it (timestamped, next
  to the original)
- Sets `PermitRootLogin no` unconditionally
- If you chose key-based auth: disables `PasswordAuthentication`, installs
  your public key into `~/.ssh/authorized_keys` for the user you specified
- If you chose password auth: leaves password login on, but root is still
  locked out
- Restarts the SSH service

### Mosquitto (MQTT broker)
- Installs `mosquitto` + `mosquitto-clients` if not already present
- Creates a password file via `mosquitto_passwd` for the user you specified
- Writes `/etc/mosquitto/conf.d/auth.conf` with `allow_anonymous false`
- Restarts and enables the Mosquitto service

### SSL certificate
- Generates a 2048-bit self-signed cert with OpenSSL
- Sets the certificate's **CN to student ID**, as the assignment requires
- Key file permissions locked to `600`

### Secrets file
- Writes MQTT/email credentials to a single file (default
  `/etc/surveillance/secrets.env`) with `600` permissions
- The C/Python services should read these as environment variables at
  runtime instead of embedding them in source — add this path to `.gitignore`

---

## Notes

- The script is idempotent-ish: re-running it won't duplicate SSH keys or
  break an existing Mosquitto password file, but it will overwrite the
  secrets file and SSL cert if you say yes to those sections again.
- If you re-run with a different SSH method, remember the old `sshd_config`
  backup is still sitting next to the live one — clean those up before
  submitting if you don't want clutter in your deliverables.
- Nothing here is committed to git for you — you still need to add
  `secrets.env` (or wherever you pointed `SECRETS_PATH`) to `.gitignore`
  yourself.
