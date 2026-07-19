#!/usr/bin/env bash

# ==============================================================================
#                 DISTRIBUTED CLUSTER REST API PERFORMANCE BENCHMARK
# ==============================================================================
# Updated for the sensor_id + sensor_type + date matching key (Section 5).
# sensor_name is no longer a request input -- it's resolved from the DB and
# only appears in the response payload.
# ==============================================================================

read -p "Enter Target Host IP [192.168.56.101]: " INPUT_IP
BROKER_IP=${INPUT_IP:-"192.168.56.101"}

# Comprehensive matrix mapping all cluster sensor payloads from Master, Slave1, and Slave2
declare -a SENSORS=(
# --- Master Node Telemetry Data Targets ---
"101:temperature:24.8"
"102:humidity:45"
"103:motion:1"
"104:temperature:23.9"
# --- Slave Node 1 Telemetry Data Targets ---
"201:temperature:25.3"
"202:humidity:50"
"203:motion:0"
"204:co2:735"
# --- Slave Node 2 Telemetry Data Targets ---
"301:temperature:23.1"
"302:humidity:41"
"303:motion:0"
"304:smoke:0"
)

TARGET_DATE="2026-06-01"

run_benchmark_round() {
local round_num="$1"
echo "======================================================================"
echo "   RUNNING BENCHMARK ROUND $round_num"
echo "======================================================================"
for item in "${SENSORS[@]}"; do
IFS=':' read -r id type expected <<< "$item"
echo -e "\n[TEST] Querying ID $id (type=$type) via REST API Endpoint (Expected=$expected)"
start_t=$(date +%s%N)
raw_response=$(curl -s "http://${BROKER_IP}:8000/api/logs?sensor_id=${id}&sensor_type=${type}&date=${TARGET_DATE}" 2>/dev/null || true)
end_t=$(date +%s%N)
total_time=$(echo "scale=3; ($end_t - $start_t) / 1000000" | bc)
echo "       Response Payload: $raw_response"
echo "       Round-trip Time : ${total_time}ms"
if [[ "$raw_response" =~ "$expected" ]]; then
echo -e "       \033[0;32m[PASS]\033[0m Successfully validated telemetry: $expected"
else
echo -e "       \033[0;31m[FAIL]\033[0m Value mismatch detected!"
fi
done
}

echo "[+] Verifying target subsystem health status via API endpoint..."
curl -s "http://${BROKER_IP}:8000/api/health" || echo "[!] Notice: Health endpoint did not respond."
echo -e "\n"

echo "[+] Flushing remote Memcached instance to clear stale cache frames..."
echo "flush_all" | nc -q 1 "$BROKER_IP" 11211 2>/dev/null || echo "[!] Notice: Memcached did not respond to flush."

run_benchmark_round "1 (Cache Misses - Reading Distributed DBs / Cascade)"
run_benchmark_round "2 (Cache Hits  - Zero DB Interaction via Memcached)"

echo -e "\n=== RUNNING COMPLETE STRUCTURAL API HEALTH STATUS DUMP ==="
curl -i "http://${BROKER_IP}:8000/api/health"
