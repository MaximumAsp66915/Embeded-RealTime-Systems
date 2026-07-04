#!/bin/bash
set -euo pipefail

DB_FILE=${1:-master.db}
SENSOR_TYPE=${2:-temperature}
SENSOR_ID=${3:-101}

if [ ! -f "$DB_FILE" ]; then
    echo "Error: database file not found: $DB_FILE"
    exit 1
fi

sqlite3 "$DB_FILE" <<EOF
.headers on
.mode column
SELECT
    s.sensor_id,
    s.sensor_type,
    s.sensor_name,
    s.location,
    r.value,
    s.unit,
    r.recorded_at
FROM sensors s
JOIN sensor_readings r ON s.sensor_id = r.sensor_id
WHERE s.sensor_type = '$SENSOR_TYPE'
  AND s.sensor_id = '$SENSOR_ID'
ORDER BY datetime(r.recorded_at) DESC
LIMIT 1;
EOF
