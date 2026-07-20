#!/usr/bin/env bash
set -e

echo "=================================================="
echo "          MASTER CORE RUNTIME INTERFACE           "
echo "=================================================="

# 1. Automated check for required SQLite3 development header installations
if [ ! -f /usr/include/sqlite3.h ]; then
    echo "[+] System header missing. Installing libsqlite3-dev dynamically..."
    sudo apt update && sudo apt install -y libsqlite3-dev
else
    echo "[+] SQLite3 development headers validated."
fi

# 2. Interactive configuration parameter binding
read -p "Enter Gateway Binding Port for Master [8080]: " M_PORT
M_PORT=${M_PORT:-8080}

read -p "Enter target Master SQLite DB path [../master.db]: " M_DB
M_DB=${M_DB:-../master.db}

read -p "Enter Target Slave 1 IP Address [192.168.56.102]: " S1_IP
S1_IP=${S1_IP:-"192.168.56.102"}

read -p "Enter Target Slave 1 Port [8080]: " S1_PORT
S1_PORT=${S1_PORT:-8080}

read -p "Enter Target Slave 2 IP Address [192.168.56.103]: " S2_IP
S2_IP=${S2_IP:-"192.168.56.103"}

read -p "Enter Target Slave 2 Port [8080]: " S2_PORT
S2_PORT=${S2_PORT:-8080}

# 3. Dynamic Nginx Installation and Setup Engine
if ! command -v nginx &> /dev/null; then
    echo "[+] Nginx not detected. Installing web server engine dynamically..."
    sudo apt update && sudo apt install -y nginx
else
    echo "[+] Nginx runtime binary validation successful."
fi

NGINX_CONF="/etc/nginx/sites-available/api_gateway"
NGINX_LINK="/etc/nginx/sites-enabled/api_gateway"

echo "[+] Injecting clean API Gateway proxy block configuration matrix..."
sudo tee "$NGINX_CONF" > /dev/null <<EOF
# =======================================================
# Automated Project API Gateway Configuration
# =======================================================

# 1. Route for Master Application
server {
    listen 8001;
    
    location / {
        proxy_pass http://127.0.0.1:$M_PORT;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
    }
}

# 2. Route for Slave 1
server {
    listen 8002;

    location / {
        proxy_pass http://$S1_IP:$S1_PORT;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
    }
}

# 3. Route for Slave 2
server {
    listen 8003;

    location / {
        proxy_pass http://$S2_IP:$S2_PORT;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
    }
}
EOF

if [ ! -L "$NGINX_LINK" ]; then
    echo "[+] Creating symbolic runtime link in sites-enabled..."
    sudo ln -s "$NGINX_CONF" "$NGINX_LINK"
fi

echo "[+] Validating Nginx configuration syntax matrix..."
if sudo nginx -t; then
    echo "[+] Configuration verified. Hot-reloading daemon instance..."
    sudo systemctl reload nginx
else
    echo "[-] Nginx configuration test failed! Aborting pipeline."
    exit 1
fi

# 4. Persist environment asset configuration context
echo "PORT=$M_PORT" > env
echo "DB_PATH=$M_DB" >> env
echo "SLAVE1_IP=$S1_IP" >> env
echo "SLAVE1_PORT=$S1_PORT" >> env
echo "SLAVE2_IP=$S2_IP" >> env
echo "SLAVE2_PORT=$S2_PORT" >> env

# 5. Directory layout consistency mapping
mkdir -p src headers

if [ ! -f src/mongoose.c ] || [ ! -f src/mongoose.h ]; then
    echo "[+] Fetching Mongoose layout header targets..."
    curl -s https://raw.githubusercontent.com/cesanta/mongoose/master/mongoose.c -o src/mongoose.c
    curl -s https://raw.githubusercontent.com/cesanta/mongoose/master/mongoose.h -o src/mongoose.h
fi

# Apply running environment context states into process memory namespace
export PORT=$M_PORT
export DB_PATH=$M_DB
export SLAVE1_IP=$S1_IP
export SLAVE1_PORT=$S1_PORT
export SLAVE2_IP=$S2_IP
export SLAVE2_PORT=$S2_PORT

echo "[+] Initiating compilation engine via Makefile..."
make clean && make

echo "[+] Compilation successful."
echo "[+] Starting Master Core system in FOREGROUND..."
echo "--------------------------------------------------"
./master_node