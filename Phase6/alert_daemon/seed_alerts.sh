#!/usr/bin/env bash
set -e

echo "=================================================="
echo "        ALERT DAEMON - SAMPLE DATA SEEDER         "
echo "=================================================="

# ------------------------------------------------------------------
# DB path resolution: explicit $1 argument wins; otherwise fall back to
# DB_PATH already exported in the shell, or the value written into
# ./env by run.sh (the same file the daemon itself reads via
# EnvironmentFile= / getenv). This mirrors how run.sh writes DB_PATH
# into `env` for this same node.
# ------------------------------------------------------------------
if [ -n "$1" ]; then
    SEED_DB="$1"
elif [ -n "$DB_PATH" ]; then
    SEED_DB="$DB_PATH"
elif [ -f "./env" ]; then
    SEED_DB="$(grep '^DB_PATH=' ./env | cut -d'=' -f2-)"
fi

if [ -z "$SEED_DB" ]; then
    echo "[!] Could not determine a DB path."
    echo "    Usage: ./seed_alerts.sh [path/to/node.db]"
    echo "    (or run ./run.sh first so DB_PATH is written into ./env)"
    exit 1
fi

if ! command -v sqlite3 &> /dev/null; then
    echo "[+] Installing sqlite3 CLI..."
    sudo apt update && sudo apt install -y sqlite3
fi

echo "[+] Seeding sample alert rows into: $SEED_DB"

sqlite3 "$SEED_DB" <<SQLEOF
CREATE TABLE IF NOT EXISTS alerts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sensor_id TEXT,
    sensor_name TEXT,
    alert_type TEXT,
    sensor_value TEXT,
    created_at TEXT,
    status TEXT
);

INSERT INTO alerts (sensor_id, sensor_name, alert_type, sensor_value, created_at, status)
VALUES
    ('101', 'Floor1_Room101_Temp',   'HIGH_TEMPERATURE',      '38.4',   datetime('now', '-2 hours'),  'resolved'),
    ('101', 'Floor1_Room101_Temp',   'HIGH_TEMPERATURE',      '36.9',   datetime('now', '-10 minutes'), 'active'),
    ('204', 'Floor2_Meeting_CO2',    'HUMIDITY_OUT_OF_RANGE', '14.2',   datetime('now', '-45 minutes'), 'resolved'),
    ('301', 'Floor3_Server_Temp',    'SENSOR_TIMEOUT',        'NO_DATA', datetime('now', '-5 minutes'),  'active'),
    ('103', 'Floor1_Corridor_Motion','INVALID_VALUE',         'ERR',    datetime('now', '-1 minutes'),  'active');
SQLEOF

echo "[+] Done. Sample rows inserted (2 active, 3 resolved)."
echo "[+] Verify with: sqlite3 $SEED_DB \"SELECT id, sensor_id, alert_type, status, created_at FROM alerts ORDER BY id;\""
