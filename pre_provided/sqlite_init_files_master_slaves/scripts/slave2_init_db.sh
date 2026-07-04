#!/bin/bash
set -euo pipefail

DB_FILE=${1:-slave2.db}
CSV_FILE=${2:-../data/slave2_sensors.csv}
NODE_NAME="slave2"
NODE_ROLE="SLAVE"
NODE_DESCRIPTION="Second slave node responsible for local sensor data"

if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "Error: sqlite3 is not installed."
    echo "Install it with: sudo apt update && sudo apt install -y sqlite3"
    exit 1
fi

if [ ! -f "$CSV_FILE" ]; then
    echo "Error: CSV file not found: $CSV_FILE"
    echo "Usage: ./slave2_init_db.sh <database_file> <csv_file>"
    exit 1
fi

echo "Initializing SQLite database for $NODE_NAME..."
echo "Database: $DB_FILE"
echo "CSV file: $CSV_FILE"

rm -f "$DB_FILE"

sqlite3 "$DB_FILE" <<EOF
PRAGMA foreign_keys = ON;

CREATE TABLE node_info (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    node_name TEXT NOT NULL,
    node_role TEXT NOT NULL,
    description TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE sensors (
    sensor_id TEXT PRIMARY KEY,
    sensor_type TEXT NOT NULL,
    sensor_name TEXT NOT NULL,
    location TEXT,
    unit TEXT,
    node_name TEXT NOT NULL,
    is_active INTEGER DEFAULT 1
);

CREATE TABLE sensor_readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sensor_id TEXT NOT NULL,
    value TEXT NOT NULL,
    recorded_at TEXT NOT NULL,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY(sensor_id) REFERENCES sensors(sensor_id)
);

CREATE INDEX idx_sensors_type_id ON sensors(sensor_type, sensor_id);
CREATE INDEX idx_readings_sensor_time ON sensor_readings(sensor_id, recorded_at DESC);

INSERT INTO node_info (node_name, node_role, description)
VALUES ('$NODE_NAME', '$NODE_ROLE', '$NODE_DESCRIPTION');
EOF

TMP_FILE=$(mktemp)
tail -n +2 "$CSV_FILE" > "$TMP_FILE"

sqlite3 "$DB_FILE" <<EOF
.mode csv

CREATE TABLE raw_import (
    sensor_id TEXT,
    sensor_type TEXT,
    sensor_name TEXT,
    location TEXT,
    value TEXT,
    unit TEXT,
    recorded_at TEXT
);

.import "$TMP_FILE" raw_import

INSERT OR IGNORE INTO sensors (
    sensor_id,
    sensor_type,
    sensor_name,
    location,
    unit,
    node_name
)
SELECT DISTINCT
    sensor_id,
    sensor_type,
    sensor_name,
    location,
    unit,
    '$NODE_NAME'
FROM raw_import;

INSERT INTO sensor_readings (
    sensor_id,
    value,
    recorded_at
)
SELECT
    sensor_id,
    value,
    recorded_at
FROM raw_import;

DROP TABLE raw_import;
EOF

rm -f "$TMP_FILE"

echo "Database initialized successfully for $NODE_NAME."
echo "Summary:"

sqlite3 "$DB_FILE" <<EOF
.headers on
.mode column
SELECT node_name, node_role FROM node_info;
SELECT COUNT(*) AS sensors_count FROM sensors;
SELECT COUNT(*) AS readings_count FROM sensor_readings;
EOF

echo
echo "Sample latest readings:"
sqlite3 "$DB_FILE" <<EOF
.headers on
.mode column
SELECT
    s.sensor_id,
    s.sensor_type,
    s.sensor_name,
    r.value,
    s.unit,
    r.recorded_at
FROM sensors s
JOIN sensor_readings r ON r.sensor_id = s.sensor_id
WHERE r.recorded_at = (
    SELECT MAX(r2.recorded_at)
    FROM sensor_readings r2
    WHERE r2.sensor_id = s.sensor_id
)
ORDER BY s.sensor_id
LIMIT 5;
EOF
