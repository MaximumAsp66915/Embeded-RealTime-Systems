#!/usr/bin/env bash

read -p "Enter Master Node IP [192.168.56.101]: " INPUT_IP
TARGET_IP=${INPUT_IP:-"192.168.56.101"}

read -p "Enter Master Node Port [8001]: " INPUT_PORT
TARGET_PORT=${INPUT_PORT:-"8001"}

BASE_URL="http://${TARGET_IP}:${TARGET_PORT}/query"

# Test cases array (type, id)
declare -a SENSORS=("temperature:101" "temperature:201" "temperature:301" "temperature:401")

run_round() {
    local round_num=$1
    echo -e "\n--- Running Round ${round_num} ---"
    for item in "${SENSORS[@]}"; do
        type="${item%%:*}"
        id="${item##*:}"
        
        start=$(date +%s%N)
        response=$(curl -s "${BASE_URL}?type=${type}&id=${id}")
        end=$(date +%s%N)
        
        duration_ms=$(echo "scale=3; ($end - $start) / 1000000" | bc)
        echo "Sensor Type: $type, ID: $id -> Total Roundtrip: ${duration_ms}ms | Payload: $response"
    done
}

echo "=================================================="
echo "         TWO-LAYER CACHE PERFORMANCE BENCHMARK    "
echo "=================================================="

# Dynamic Cache Purge to ensure a valid baseline
echo "[+] Flushing remote Memcached instance to clear stale states..."
echo "flush_all" | nc -q 1 127.0.0.1 11211 || echo "[!] Warning: Could not auto-flush cache."

# Round 1: Will now correctly show "source":"DB" (with higher roundtrip latencies)
run_round "1 (Cache Misses)"

# Round 2: Will correctly show "source":"CACHE" (with sub-millisecond inner times)
run_round "2 (Cache Hits)"