#!/usr/bin/env bash
set -e

echo "=================================================="
echo "          ALERT DAEMON RUNTIME INTERFACE          "
echo "=================================================="

UPDATED_APT=false

for package in libsqlite3-dev sqlite3; do
    if ! dpkg -s "$package" &> /dev/null; then
        echo "[+] Installing missing package: $package..."
        if [ "$UPDATED_APT" = false ]; then
            sudo apt update
            UPDATED_APT=true
        fi
        sudo apt install -y "$package"
    fi
done

# ------------------------------------------------------------------
# This is the SAME binary on every node -- master, slave1, slave2.
# Point DB_PATH at THIS node's own local DB (../master.db on the
# master, ../slave1.db on slave 1, ../slave2.db on slave 2), the same
# way master/run.sh and slave/run.sh each only ever configure their
# own node's DB_PATH. There is no cross-node awareness here.
# ------------------------------------------------------------------
read -p "Enter local SQLite database path for THIS node [../master.db]: " A_DB
A_DB=${A_DB:-../master.db}

read -p "Enter poll interval in seconds [10]: " A_POLL
A_POLL=${A_POLL:-10}

read -p "Enter high-temperature threshold in degrees [35.0]: " A_TEMP_HIGH
A_TEMP_HIGH=${A_TEMP_HIGH:-35.0}

read -p "Enter minimum allowed humidity percent [20.0]: " A_HUM_MIN
A_HUM_MIN=${A_HUM_MIN:-20.0}

read -p "Enter maximum allowed humidity percent [70.0]: " A_HUM_MAX
A_HUM_MAX=${A_HUM_MAX:-70.0}

read -p "Enter sensor timeout in seconds (no new reading = alert) [300]: " A_TIMEOUT
A_TIMEOUT=${A_TIMEOUT:-300}

read -p "Enter minimum sane recorded value (sanity floor) [-50.0]: " A_VAL_MIN
A_VAL_MIN=${A_VAL_MIN:--50.0}

read -p "Enter maximum sane recorded value (sanity ceiling) [1000.0]: " A_VAL_MAX
A_VAL_MAX=${A_VAL_MAX:-1000.0}

{
    echo "DB_PATH=$A_DB"
    echo "POLL_INTERVAL_SEC=$A_POLL"
    echo "TEMP_HIGH_MAX=$A_TEMP_HIGH"
    echo "HUMIDITY_MIN=$A_HUM_MIN"
    echo "HUMIDITY_MAX=$A_HUM_MAX"
    echo "SENSOR_TIMEOUT_SEC=$A_TIMEOUT"
    echo "VALUE_MIN=$A_VAL_MIN"
    echo "VALUE_MAX=$A_VAL_MAX"
} > env

export DB_PATH=$A_DB
export POLL_INTERVAL_SEC=$A_POLL
export TEMP_HIGH_MAX=$A_TEMP_HIGH
export HUMIDITY_MIN=$A_HUM_MIN
export HUMIDITY_MAX=$A_HUM_MAX
export SENSOR_TIMEOUT_SEC=$A_TIMEOUT
export VALUE_MIN=$A_VAL_MIN
export VALUE_MAX=$A_VAL_MAX

echo "[+] Initiating compilation engine via Makefile..."
make clean && make
echo "[+] Compilation successful."

# ------------------------------------------------------------------
# Everything below installs and runs the daemon as a systemd service.
# No manual systemctl/cp/sed commands are needed outside this script --
# run.sh generates the unit file (with THIS checkout's real absolute
# path filled in, so there's nothing to hand-edit), installs it,
# reloads systemd, enables it on boot, and (re)starts it.
# ------------------------------------------------------------------
if ! command -v systemctl &> /dev/null; then
    echo "[!] systemctl not found on this system -- cannot install as a service." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_NAME="alert_daemon"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

echo "[+] Generating systemd unit for this node..."
echo "[+]   WorkingDirectory / ExecStart / EnvironmentFile -> $SCRIPT_DIR"
sudo bash -c "cat > '$SERVICE_FILE'" <<EOF
[Unit]
Description=Distributed Sensor Cluster - Alert Daemon (Section 6)
After=network.target

[Service]
Type=simple
WorkingDirectory=$SCRIPT_DIR
EnvironmentFile=$SCRIPT_DIR/env
ExecStart=$SCRIPT_DIR/alert_daemon
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

echo "[+] Reloading systemd daemon..."
sudo systemctl daemon-reload

echo "[+] Enabling $SERVICE_NAME (start automatically on boot)..."
sudo systemctl enable "$SERVICE_NAME"

echo "[+] Starting $SERVICE_NAME (restarts it if it was already running)..."
sudo systemctl restart "$SERVICE_NAME"

echo "--------------------------------------------------"
echo "[+] Current service status:"
sudo systemctl status "$SERVICE_NAME" --no-pager || true

echo "=================================================="
echo "          ALERT DAEMON IS NOW MANAGED BY SYSTEMD   "
echo "=================================================="
echo "[+]   Watching: $A_DB"
echo "[+]   Poll interval: ${A_POLL}s"
echo ""
echo "Useful commands for working with the service:"
echo "  sudo systemctl status  $SERVICE_NAME           # check if it's active/failed"
echo "  sudo systemctl stop    $SERVICE_NAME           # stop it"
echo "  sudo systemctl restart $SERVICE_NAME           # restart it (e.g. after a rebuild)"
echo "  sudo systemctl disable $SERVICE_NAME           # stop it starting on boot"
echo ""
echo "Useful commands for reading its logs (journalctl):"
echo "  sudo journalctl -u $SERVICE_NAME -f             # follow logs live"
echo "  sudo journalctl -u $SERVICE_NAME -n 100         # last 100 lines"
echo "  sudo journalctl -u $SERVICE_NAME --since today  # everything logged today"
