#!/usr/bin/env bash

# ==============================================================================
#                      DISTRIBUTED CLUSTER TEST AUTOMATION
# ==============================================================================

# Interactive Target Prompts (Defaults provided in brackets)
read -p "Enter Master Node IP [192.168.56.101]: " INPUT_IP
TARGET_IP=${INPUT_IP:-"192.168.56.101"}

read -p "Enter Master Node Port [8001]: " INPUT_PORT
TARGET_PORT=${INPUT_PORT:-"8001"}

BASE_URL="http://${TARGET_IP}:${TARGET_PORT}/query"

# Test Counter Metrics
PASSED_TESTS=0
FAILED_TESTS=0

# Helper Assertion Matrix Function
assert_query() {
    local type="$1"
    local id="$2"
    local expected="$3"
    local description="$4"

    echo "----------------------------------------------------------------------"
    echo "[TEST] ${description}"
    echo "       Query: ${BASE_URL}?type=${type}&id=${id}"

    # Execute request and capture response payload line string stripping trailing carriage returns
    local response=$(curl -s "${BASE_URL}?type=${type}&id=${id}" | tr -d '\r')

    echo "       Expected: '${expected}'"
    echo "       Received: '${response}'"

    # Evaluate dynamic validation matches depending on expected payload target
    local match_success=1
    
    if [ "${expected}" = "NOT_FOUND" ]; then
        # Check if JSON status matches NOT_FOUND
        if [[ "$response" =~ \"status\":\"NOT_FOUND\" ]]; then
            match_success=0
        fi
    else
        # Check if JSON value matches the expected metric string
        if [[ "$response" =~ \"value\":\"${expected}\" ]]; then
            match_success=0
        fi
    fi

    if [ ${match_success} -eq 0 ]; then
        echo -e "       \033[0;32m[PASS]\033[0m Verification successful."
        ((PASSED_TESTS++))
    else
        echo -e "       \033[0;31m[FAIL]\033[0m Mismatch detected!"
        ((FAILED_TESTS++))
    fi
}

echo "======================================================================"
echo "          INITIATING END-TO-END CASCADE ORCHESTRATION TEST            "
echo "======================================================================"

# ==============================================================================
# PLACEHOLDERS: Third argument automatically extracts against the new JSON payload fields
# ==============================================================================

assert_query "temperature" "101" "24.8"      "Probe local Master DB Storage Matrix"
assert_query "temperature" "201" "25.3"      "Probe downstream Slave 1 Node Cascade Pipeline"
assert_query "temperature" "301" "23.1"      "Probe downstream Slave 2 Node Cascade Pipeline"
assert_query "temperature" "401" "NOT_FOUND" "Verify Cluster Miss across complete Topology"

assert_query "humidity" "102" "45"           "Probe local Master DB Storage Matrix"
assert_query "humidity" "202" "50"           "Probe downstream Slave 1 Node Cascade Pipeline"
assert_query "humidity" "302" "41"           "Probe downstream Slave 2 Node Cascade Pipeline"
assert_query "humidity" "402" "NOT_FOUND"    "Verify Cluster Miss across complete Topology"

assert_query "motion" "103" "1"              "Probe local Master DB Storage Matrix"
assert_query "motion" "203" "0"              "Probe downstream Slave 1 Node Cascade Pipeline"
assert_query "motion" "303" "0"              "Probe downstream Slave 2 Node Cascade Pipeline"
assert_query "motion" "403" "NOT_FOUND"    "Verify Cluster Miss across complete Topology"

assert_query "temperature" "104" "23.9"      "Probe local Master DB Storage Matrix"
assert_query "temperature" "204" "NOT_FOUND" "Probe downstream Slave 1 Node Cascade Pipeline"
assert_query "temperature" "304" "NOT_FOUND" "Probe downstream Slave 2 Node Cascade Pipeline"
assert_query "temperature" "404" "NOT_FOUND" "Verify Cluster Miss across complete Topology"

assert_query "co2" "104" "NOT_FOUND"         "Probe local Master DB Storage Matrix"
assert_query "co2" "204" "735"               "Probe downstream Slave 1 Node Cascade Pipeline"
assert_query "co2" "304" "NOT_FOUND"         "Probe downstream Slave 2 Node Cascade Pipeline"
assert_query "co2" "404" "NOT_FOUND"         "Verify Cluster Miss across complete Topology"

assert_query "smoke" "104" "NOT_FOUND"       "Probe local Master DB Storage Matrix"
assert_query "smoke" "204" "NOT_FOUND"       "Probe downstream Slave 1 Node Cascade Pipeline"
assert_query "smoke" "304" "0"               "Probe downstream Slave 2 Node Cascade Pipeline"
assert_query "smoke" "404" "NOT_FOUND"       "Verify Cluster Miss across complete Topology"

# ==============================================================================
#                               FINAL EVALUATION
# ==============================================================================
echo "======================================================================"
echo "                           TEST EXECUTION SUMMARY                     "
echo "======================================================================"
echo " Total Executed: $((PASSED_TESTS + FAILED_TESTS))"
echo " Passed Matrix:  ${PASSED_TESTS}"
echo " Failed Matrix:  ${FAILED_TESTS}"
echo "----------------------------------------------------------------------"

if [ ${FAILED_TESTS} -eq 0 ]; then
    echo -e "\033[0;32m[SUCCESS] All cascade validation paths matches expectations perfectly!\033[0m"
    exit 0
else
    echo -e "\033[0;31m[FAILURE] Architectural trace errors verified inside the data return stream.\033[0m"
    exit 1
fi