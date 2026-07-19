#!/usr/bin/env bash

# ==============================================================================
#                      DISTRIBUTED CLUSTER MQTT PERFORMANCE TEST
# ==============================================================================

read -p "Enter MQTT Broker Host IP [192.168.56.101]: " INPUT_IP
BROKER_IP=${INPUT_IP:-"192.168.56.101"}

# Dynamic local system verification of client tools
if ! command -v mosquitto_pub &> /dev/null || ! command -v mosquitto_sub &> /dev/null; then
    echo "[+] Missing client tools. Installing mosquitto-clients..."
    sudo apt update && sudo apt install -y mosquitto-clients
fi

# Define our test scenarios (Type, ID, Expected value)
declare -a SENSORS=(
    "temperature:101:24.8"
    "temperature:201:25.3"
    "temperature:301:23.1"
    "temperature:401:NOT_FOUND"
)

query_mqtt_node() {
    local type="$1"
    local id="$2"
    local corr_id="test_corr_$(date +%s%N)"
    local tmp_file
    tmp_file=$(mktemp)
    
    # 1. Start the subscriber cleanly in the background writing to a temp file
    mosquitto_sub -h "$BROKER_IP" -t "sensor/response" -C 1 -W 3 > "$tmp_file" &
    local sub_pid=$!
    
    # 2. Give the subscriber a moment to safely establish its socket connection
    sleep 0.2
    
    # 3. Publish the structured query request packet
    mosquitto_pub -h "$BROKER_IP" -t "sensor/request" -m "{\"type\":\"$type\",\"id\":\"$id\",\"correlation_id\":\"$corr_id\"}"
    
    # 4. Wait for the background subscriber process to finish receiving the response
    wait $sub_pid 2>/dev/null
    
    # 5. Read the payload, filter by correlation ID, cleanup, and return
    local raw_response
    raw_response=$(cat "$tmp_file" | grep "$corr_id" || true)
    rm -f "$tmp_file"
    
    echo "$raw_response"
}

run_benchmark_round() {
    local round_num="$1"
    echo "======================================================================"
    echo "   RUNNING BENCHMARK ROUND $round_num"
    echo "======================================================================"
    
    for item in "${SENSORS[@]}"; do
        type="${item%%:*}"
        tmp="${item#*:}"
        id="${tmp%%:*}"
        expected="${tmp#*:}"
        
        echo -e "\n[TEST] Querying Sensor: Type=$type, ID=$id (Expected=$expected)"
        
        start_t=$(date +%s%N)
        raw_response=$(query_mqtt_node "$type" "$id")
        end_t=$(date +%s%N)
        
        total_time=$(echo "scale=3; ($end_t - $start_t) / 1000000" | bc)
        echo "       Response Payload: $raw_response"
        echo "       Round-trip Time : ${total_time}ms"
        
        # Validation checks
        if [ "$expected" = "NOT_FOUND" ]; then
            if [[ "$raw_response" =~ \"status\":\"NOT_FOUND\" ]]; then
                echo -e "       \033[0;32m[PASS]\033[0m Successfully identified missing target."
            else
                echo -e "       \033[0;31m[FAIL]\033[0m Failed to flag missing node correctly!"
            fi
        else
            if [[ "$raw_response" =~ \"value\":\"$expected\" ]]; then
                echo -e "       \033[0;32m[PASS]\033[0m Successfully validated telemetry: $expected"
            else
                echo -e "       \033[0;31m[FAIL]\033[0m Value mismatch detected!"
            fi
        fi
    done
}

# Flush Memcached prior to test execution to establish a baseline
echo "[+] Flushing remote Memcached instance to clear stale cache frames..."
echo "flush_all" | nc -q 1 "$BROKER_IP" 11211 2>/dev/null || echo "[!] Notice: Memcached did not respond to flush on $BROKER_IP."

# Executing both benchmark rounds sequentially
run_benchmark_round "1 (Cache Misses - Reading DBs / Cascade)"
run_benchmark_round "2 (Cache Hits  - Zero DB Interaction)"