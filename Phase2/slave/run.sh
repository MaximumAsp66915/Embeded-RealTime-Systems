#!/usr/bin/env bash
set -e

echo "=================================================="
echo "          SLAVE NODE RUNTIME INTERFACE            "
echo "=================================================="

# Automated check for SQLite3 and Memcached installations
UPDATED_APT=false

if [ ! -f /usr/include/sqlite3.h ]; then
    echo "[+] System header missing. Installing libsqlite3-dev dynamically..."
    sudo apt update && sudo apt install -y libsqlite3-dev
    UPDATED_APT=true
else
    echo "[+] SQLite3 development headers validated."
fi

if [ ! -f /usr/include/libmemcached/memcached.h ] || ! command -v memcached &> /dev/brk; then
    echo "[+] Memcached tools or headers missing. Installing ecosystem..."
    if [ "$UPDATED_APT" = false ]; then
        sudo apt update
    fi
    sudo apt install -y memcached libmemcached-dev
else
    echo "[+] Memcached development headers validated."
fi

# Ensure the Memcached background daemon is running smoothly
if ! systemctl is-active --quiet memcached; then
    echo "[+] Starting local Memcached service daemon..."
    sudo systemctl start memcached
    sudo systemctl enable memcached
else
    echo "[+] Local Memcached service daemon is already running."
fi

# Dynamic configuration input
read -p "Enter Binding Port for this Slave [8080]: " CHOSEN_PORT
CHOSEN_PORT=${CHOSEN_PORT:-8080}

read -p "Enter local SQLite database path [../slave1.db]: " CHOSEN_DB
CHOSEN_DB=${CHOSEN_DB:-../slave1.db}

# Save context inside the env asset file
echo "PORT=$CHOSEN_PORT" > env
echo "DB_PATH=$CHOSEN_DB" >> env

# Structural Directory Verification
mkdir -p src headers

# Dynamically download modern standalone Mongoose if not present locally
if [ ! -f src/mongoose.c ] || [ ! -f src/mongoose.h ]; then
    echo "[+] Fetching Mongoose library components..."
    curl -s https://raw.githubusercontent.com/cesanta/mongoose/master/mongoose.c -o src/mongoose.c
    curl -s https://raw.githubusercontent.com/cesanta/mongoose/master/mongoose.h -o src/mongoose.h
fi

# Export properties into active shell execution namespace
export PORT=$CHOSEN_PORT
export DB_PATH=$CHOSEN_DB

echo "[+] Initiating compilation engine via Makefile..."
make clean && make

echo "[+] Binary built successfully."
echo "[+] Initializing execution loop in FOREGROUND..."
echo "--------------------------------------------------"
./slave_node