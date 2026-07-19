#!/usr/bin/env bash
set -e

echo "=================================================="
echo "          MASTER CORE RUNTIME INTERFACE           "
echo "=================================================="

UPDATED_APT=false

# Install the correct development headers
for package in libsqlite3-dev memcached libmemcached-dev libpaho-mqtt-dev mosquitto; do
    if ! dpkg -s "$package" &> /dev/null; then
        echo "[+] Installing missing package: $package..."
        if [ "$UPDATED_APT" = false ]; then
            sudo apt update
            UPDATED_APT=true
        fi
        sudo apt install -y "$package"
    fi
done

# Ensure Mosquitto allows external anonymous connections
MOSQUITTO_CONF="/etc/mosquitto/conf.d/local.conf"
if [ ! -f "$MOSQUITTO_CONF" ] || ! grep -q "listener 1883 0.0.0.0" "$MOSQUITTO_CONF"; then
    echo "[+] Configuring Mosquitto for external/anonymous connections..."
    sudo mkdir -p /etc/mosquitto/conf.d
    echo -e "listener 1883 0.0.0.0\nallow_anonymous true" | sudo tee "$MOSQUITTO_CONF" > /dev/null
    echo "[+] Restarting Mosquitto daemon to apply new configuration..."
    sudo systemctl restart mosquitto
fi

# Ensure Memcached listens globally on all interfaces (Fixes the Cache Hit bug)
MEMCACHED_CONF="/etc/memcached.conf"
if [ -f "$MEMCACHED_CONF" ] && grep -q "\-l 127.0.0.1" "$MEMCACHED_CONF"; then
    echo "[+] Configuring Memcached to accept external cluster flushes (0.0.0.0)..."
    sudo sed -i 's/-l 127.0.0.1/-l 0.0.0.0/g' "$MEMCACHED_CONF"
    echo "[+] Restarting Memcached daemon..."
    sudo systemctl restart memcached
fi

# Ensure background services are active
if ! systemctl is-active --quiet memcached; then
    sudo systemctl start memcached
fi
if ! systemctl is-active --quiet mosquitto; then
    sudo systemctl start mosquitto
fi

read -p "Enter target Master SQLite DB path [../master.db]: " M_DB
M_DB=${M_DB:-../master.db}

read -p "Enter MQTT Broker Endpoint [tcp://127.0.0.1:1883]: " M_BROKER
M_BROKER=${M_BROKER:-"tcp://127.0.0.1:1883"}

# Auto-correct missing tcp:// prefix
if [[ ! "$M_BROKER" =~ ^tcp:// ]]; then
    echo "[!] Warning: Missing protocol prefix. Normalizing endpoint to tcp://$M_BROKER"
    M_BROKER="tcp://$M_BROKER"
fi

echo "DB_PATH=$M_DB" > env
echo "MQTT_BROKER=$M_BROKER" >> env

mkdir -p src

export DB_PATH=$M_DB
export MQTT_BROKER=$M_BROKER

echo "[+] Initiating compilation engine via Makefile..."
make clean && make

echo "[+] Compilation successful."
echo "[+] Starting Master Core system in FOREGROUND..."
echo "--------------------------------------------------"
./master_node