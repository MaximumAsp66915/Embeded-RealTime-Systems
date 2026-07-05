#!/usr/bin/env bash
set -e

echo "=================================================="
echo "          SLAVE NODE RUNTIME INTERFACE            "
echo "=================================================="

# Automated check for required SQLite3 development header installations
if [ ! -f /usr/include/sqlite3.h ]; then
    echo "[+] System header missing. Installing libsqlite3-dev dynamically..."
    sudo apt update && sudo apt install -y libsqlite3-dev
else
    echo "[+] SQLite3 development headers validated."
fi

# Dynamic configuration input
read -p "Enter Binding Port for this Slave [8080]: " CHOSEN_PORT
CHOSEN_PORT=${CHOSEN_PORT:-8080}

read -p "Enter local SQLite database path [slave1.db]: " CHOSEN_DB
CHOSEN_DB=${CHOSEN_DB:-slave1.db}

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