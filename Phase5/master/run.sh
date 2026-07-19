#!/usr/bin/env bash
set -e

echo "=================================================="
echo "          MASTER CORE RUNTIME INTERFACE           "
echo "=================================================="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

UPDATED_APT=false

for package in libsqlite3-dev memcached libmemcached-dev libpaho-mqtt-dev mosquitto snmp snmpd netcat-openbsd wget; do
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
# Install the pass-protocol bridge script to a stable, world-readable
# path. snmpd (running as the Debian-snmp system user) must be able
# to traverse to and execute this file, which is not guaranteed for
# scripts left inside a user's home/project directory.
# ------------------------------------------------------------------
BRIDGE_SCRIPT="/usr/local/bin/snmp_pass.sh"
echo "[+] Installing SNMP pass-protocol bridge script to $BRIDGE_SCRIPT..."
sudo cp "$SCRIPT_DIR/scripts/snmp_pass.sh" "$BRIDGE_SCRIPT"
sudo chmod 755 "$BRIDGE_SCRIPT"

# Native Linux snmpd Integration Layer
SNMPD_CONF="/etc/snmp/snmpd.conf"
echo "[+] Overwriting snmpd tracking matrix configuration..."
sudo tee "$SNMPD_CONF" > /dev/null << CONFEOF
agentAddress udp:161
rocommunity public default
view systemview included .1.3.6.1.4.1.9999
pass .1.3.6.1.4.1.9999 /bin/bash ${BRIDGE_SCRIPT}
CONFEOF

echo "[+] Restarting Net-SNMP daemon service..."
sudo systemctl restart snmpd

MOSQUITTO_CONF="/etc/mosquitto/conf.d/local.conf"
if [ ! -f "$MOSQUITTO_CONF" ] || ! grep -q "listener 1883 0.0.0.0" "$MOSQUITTO_CONF"; then
    echo "[+] Configuring Mosquitto for external/anonymous connections..."
    sudo mkdir -p /etc/mosquitto/conf.d
    echo -e "listener 1883 0.0.0.0\nallow_anonymous true" | sudo tee "$MOSQUITTO_CONF" > /dev/null
    sudo systemctl restart mosquitto
fi

MEMCACHED_CONF="/etc/memcached.conf"
if [ -f "$MEMCACHED_CONF" ] && grep -q "\-l 127.0.0.1" "$MEMCACHED_CONF"; then
    echo "[+] Configuring Memcached to accept external cluster flushes (0.0.0.0)..."
    sudo sed -i 's/-l 127.0.0.1/-l 0.0.0.0/g' "$MEMCACHED_CONF"
    sudo systemctl restart memcached
fi

if ! systemctl is-active --quiet memcached; then sudo systemctl start memcached; fi
if ! systemctl is-active --quiet mosquitto; then sudo systemctl start mosquitto; fi
if ! systemctl is-active --quiet snmpd; then sudo systemctl start snmpd; fi

read -p "Enter target Master SQLite DB path [../master.db]: " M_DB
M_DB=${M_DB:-../master.db}

read -p "Enter MQTT Broker Endpoint [tcp://127.0.0.1:1883]: " M_BROKER
M_BROKER=${M_BROKER:-"tcp://127.0.0.1:1883"}

if [[ ! "$M_BROKER" =~ ^tcp:// ]]; then
    M_BROKER="tcp://$M_BROKER"
fi

read -p "Enter comma-separated SNMP-exposed sensor IDs [101,102,103,104,201,202,203,204,301,302,303,304]: " M_SENSOR_IDS
M_SENSOR_IDS=${M_SENSOR_IDS:-"101,102,103,104,201,202,203,204,301,302,303,304"}

read -p "Enter HTTP port for the embedded Sensor Log API (Section 5) [8000]: " M_API_PORT
M_API_PORT=${M_API_PORT:-8000}

echo "DB_PATH=$M_DB" > env
echo "MQTT_BROKER=$M_BROKER" >> env
echo "SENSOR_IDS=$M_SENSOR_IDS" >> env
echo "API_PORT=$M_API_PORT" >> env

mkdir -p src

# ------------------------------------------------------------------
# Fetch the Mongoose embedded web server library (single-file
# amalgamation) if it isn't already vendored in src/. This powers the
# Section 5 Sensor Log API, which is compiled straight into master_node.
# ------------------------------------------------------------------
MONGOOSE_VERSION="7.15"
if [ ! -f "src/mongoose.c" ] || [ ! -f "src/mongoose.h" ]; then
    echo "[+] Fetching Mongoose v${MONGOOSE_VERSION} source files..."
    wget -q "https://raw.githubusercontent.com/cesanta/mongoose/${MONGOOSE_VERSION}/mongoose.c" -O src/mongoose.c
    wget -q "https://raw.githubusercontent.com/cesanta/mongoose/${MONGOOSE_VERSION}/mongoose.h" -O src/mongoose.h
    if [ ! -s "src/mongoose.c" ] || [ ! -s "src/mongoose.h" ]; then
        echo "[!] Failed to download Mongoose automatically."
        echo "[!] Please manually place mongoose.c and mongoose.h into master/src/ and re-run."
        exit 1
    fi
else
    echo "[+] Mongoose sources already present in src/, skipping download."
fi

export DB_PATH=$M_DB
export MQTT_BROKER=$M_BROKER
export SENSOR_IDS=$M_SENSOR_IDS
export API_PORT=$M_API_PORT

echo "[+] Initiating compilation engine via Makefile..."
make clean && make

echo "[+] Compilation successful."
echo "[+] Starting Master Core system in FOREGROUND..."
echo "[+]   SNMP bridge listener : UDP 127.0.0.1:1161 (via snmpd on :161)"
echo "[+]   Sensor Log API       : http://0.0.0.0:${M_API_PORT}/api/logs"
echo "--------------------------------------------------"
./master_node
