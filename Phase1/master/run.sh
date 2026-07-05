#!/usr/bin/env bash
set -e

echo "=================================================="
echo "          MASTER CORE RUNTIME INTERFACE           "
echo "=================================================="

# Automated check for required SQLite3 development header installations
if [ ! -f /usr/include/sqlite3.h ]; then
    echo "[+] System header missing. Installing libsqlite3-dev dynamically..."
    sudo apt update && sudo apt install -y libsqlite3-dev
else
    echo "[+] SQLite3 development headers validated."
fi

# Interactive configuration parameter binding
read -p "Enter Gateway Binding Port for Master [8080]: " M_PORT
M_PORT=${M_PORT:-8080}

read -p "Enter target Master SQLite DB path [master.db]: " M_DB
M_DB=${M_DB:-master.db}

read -p "Enter Target Slave 1 Communication Port [8002]: " S1_PORT
S1_PORT=${S1_PORT:-8002}

read -p "Enter Target Slave 2 Communication Port [8003]: " S2_PORT
S2_PORT=${S2_PORT:-8003}

# Persist environment asset configuration context
echo "PORT=$M_PORT" > env
echo "DB_PATH=$M_DB" >> env
echo "SLAVE1_PORT=$S1_PORT" >> env
echo "SLAVE2_PORT=$S2_PORT" >> env

# Directory layout consistency mapping
mkdir -p src headers

if [ ! -f src/mongoose.c ] || [ ! -f src/mongoose.h ]; then
    echo "[+] Fetching Mongoose layout header targets..."
    curl -s https://raw.githubusercontent.com/cesanta/mongoose/master/mongoose.c -o src/mongoose.c
    curl -s https://raw.githubusercontent.com/cesanta/mongoose/master/mongoose.h -o src/mongoose.h
fi

# Apply running environment context states into process memory namespace
export PORT=$M_PORT
export DB_PATH=$M_DB
export SLAVE1_PORT=$S1_PORT
export SLAVE2_PORT=$S2_PORT

echo "[+] Initiating compilation engine via Makefile..."
make clean && make

echo "[+] Compilation successful."
echo "[+] Starting Master Core system in FOREGROUND..."
echo "--------------------------------------------------"
./master_node