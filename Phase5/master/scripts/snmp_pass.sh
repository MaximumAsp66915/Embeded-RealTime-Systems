#!/usr/bin/env bash
# ==============================================================================
# snmp_pass.sh - Net-SNMP "pass" protocol bridge
#
# snmpd executes this script directly for every request under the
# .1.3.6.1.4.1.9999 subtree, invoking it as:
#   snmp_pass.sh -g <OID>          (GET)
#   snmp_pass.sh -n <OID>          (GETNEXT)
#   snmp_pass.sh -s <OID> <value>  (SET - unused, this is a read-only agent)
#
# It re-packages the request as "<mode>|<oid>" and forwards it over UDP to
# the Master Engine's internal bridge listener on 127.0.0.1:1161, then
# relays back whatever the engine returns:
#   - three lines: OID / TYPE / VALUE   -> valid pass-protocol answer
#   - nothing (timeout)                  -> "No Such Instance" / end of walk
# ==============================================================================

MODE="$1"
OID="$2"

if [ -z "$MODE" ] || [ -z "$OID" ]; then
    exit 0
fi

RESPONSE=$(printf '%s|%s' "$MODE" "$OID" | nc -u -w 1 127.0.0.1 1161)

if [ -n "$RESPONSE" ]; then
    printf '%s\n' "$RESPONSE"
fi

exit 0
