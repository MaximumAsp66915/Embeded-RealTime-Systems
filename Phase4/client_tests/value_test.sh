#!/usr/bin/env bash

# ==============================================================================
#                 DISTRIBUTED CLUSTER SNMP PERFORMANCE BENCHMARK
# ==============================================================================

read -p "Enter Target Host IP [192.168.56.101]: " INPUT_IP
BROKER_IP=${INPUT_IP:-"192.168.56.101"}

declare -a SENSORS=(
    "101:24.8"
    "201:25.3"
    "301:23.1"
)

run_benchmark_round() {
    local round_num="$1"
    echo "======================================================================"
    echo "   RUNNING BENCHMARK ROUND $round_num"
    echo "======================================================================"
    
    for item in "${SENSORS[@]}"; do
        id="${item%%:*}"
        expected="${item#*:}"
        
        echo -e "\n[TEST] Querying Sensor via OID .1.3.6.1.4.1.9999.${id}.3 (Expected=$expected)"
        
        start_t=$(date +%s%N)
        raw_response=$(snmpget -v2c -c public "${BROKER_IP}:161" ".1.3.6.1.4.1.9999.${id}.3" 2>/dev/null || true)
        end_t=$(date +%s%N)
        
        total_time=$(echo "scale=3; ($end_t - $start_t) / 1000000" | bc)
        echo "       Response Payload: $raw_response"
        echo "       Round-trip Time : ${total_time}ms"
        
        if [ "$expected" = "NOT_FOUND" ]; then
            if [[ "$raw_response" =~ "NOT_FOUND" || -z "$raw_response" ]]; then
                echo -e "       \033[0;32m[PASS]\033[0m Successfully identified missing target."
            else
                echo -e "       \033[0;31m[FAIL]\033[0m Failed to flag missing node correctly!"
            fi
        else
            if [[ "$raw_response" =~ "$expected" ]]; then
                echo -e "       \033[0;32m[PASS]\033[0m Successfully validated telemetry: $expected"
            else
                echo -e "       \033[0;31m[FAIL]\033[0m Value mismatch detected!"
            fi
        fi
    done
}

echo "[+] Flushing remote Memcached instance to clear stale cache frames..."
echo "flush_all" | nc -q 1 "$BROKER_IP" 11211 2>/dev/null || echo "[!] Notice: Memcached did not respond to flush."

run_benchmark_round "1 (Cache Misses - Reading DBs / Cascade)"
run_benchmark_round "2 (Cache Hits  - Zero DB Interaction)"

echo -e "\n=== RUNNING COMPLETE STRUCTURAL SNMPWALK SENSORS TREE DUMP ==="
snmpwalk -v2c -c public "${BROKER_IP}:161" .1.3.6.1.4.1.9999