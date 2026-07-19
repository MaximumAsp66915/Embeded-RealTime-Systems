#!/usr/bin/env bash
set -e

echo "=================================================="
echo "          SLAVE NODE RUNTIME INTERFACE            "
echo "=================================================="

UPDATED_APT=false

for package in libsqlite3-dev memcached libmemcached-dev libpaho-mqtt-dev; do
    if ! dpkg -s "$package" &> /dev/null; then
        echo "[+] Installing missing development library: $package..."
        if [ "$UPDATED_APT" = false ]; then
            sudo apt update
            UPDATED_APT=true
        fi
        sudo apt install -y "$package"
    fi
done

MEMCACHED_CONF="/etc/memcached.conf"
if [ -f "$MEMCACHED_CONF" ] && grep -q "\-l 127.0.0.1" "$MEMCACHED_CONF"; then
    echo "[+] Configuring local Memcached to listen globally (0.0.0.0)..."
    sudo sed -i 's/-l 127.0.0.1/-l 0.0.0.0/g' "$MEMCACHED_CONF"
    sudo systemctl restart memcached
fi

if ! systemctl is-active --quiet memcached; then
    sudo systemctl start memcached
fi

read -p "Enter local SQLite database path [../slave1.db]: " CHOSEN_DB
CHOSEN_DB=${CHOSEN_DB:-../slave1.db}

read -p "Enter Target MQTT Broker Endpoint [tcp://192.168.56.101:1883]: " CHOSEN_BROKER
CHOSEN_BROKER=${CHOSEN_BROKER:-"tcp://192.168.56.101:1883"}

if [[ ! "$CHOSEN_BROKER" =~ ^tcp:// ]]; then
    echo "[!] Warning: Missing protocol prefix. Normalizing endpoint to tcp://$CHOSEN_BROKER"
    CHOSEN_BROKER="tcp://$CHOSEN_BROKER"
fi

echo "DB_PATH=$CHOSEN_DB" > env
echo "MQTT_BROKER=$CHOSEN_BROKER" >> env

mkdir -p src

export DB_PATH=$CHOSEN_DB
export MQTT_BROKER=$CHOSEN_BROKER

echo "[+] Initiating compilation engine via Makefile..."
make clean && make

echo "[+] Binary built successfully."
echo "[+] Initializing execution loop in FOREGROUND..."
echo "--------------------------------------------------"
./slave_node
