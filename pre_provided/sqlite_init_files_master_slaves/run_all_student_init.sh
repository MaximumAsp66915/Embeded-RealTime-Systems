#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

echo "sqlite3 is required. If needed, run:"
echo "sudo apt update && sudo apt install -y sqlite3"
echo

cd for_students/scripts
chmod +x *.sh

./master_init_db.sh master.db ../data/master_sensors.csv
./slave1_init_db.sh slave1.db ../data/slave1_sensors.csv
./slave2_init_db.sh slave2.db ../data/slave2_sensors.csv

echo
echo "Running sample queries..."
./read_latest_sample.sh master.db temperature 101
./read_latest_sample.sh slave1.db co2 204
./read_latest_sample.sh slave2.db smoke 304

echo
echo "Done."
