#!/usr/bin/env bash
#===============================================================================
# secure_setup.sh
#
# Smart Surveillance System — Step 1: Foundation & Security
#===============================================================================

set -euo pipefail

log()  { echo -e "\033[1;32m[+]\033[0m $*"; }
warn() { echo -e "\033[1;33m[!]\033[0m $*"; }
err()  { echo -e "\033[1;31m[x]\033[0m $*" >&2; }

require_root() {
    if [[ $EUID -ne 0 ]]; then
        err "This script must be run as root (use: sudo ./secure_setup.sh)"
        exit 1
    fi
}

ask() {
    local prompt="$1"
    local default="${2:-}"
    local reply
    if [[ -n "$default" ]]; then
        read -rp "$prompt [$default]: " reply
        echo "${reply:-$default}"
    else
        read -rp "$prompt: " reply
        echo "$reply"
    fi
}

ask_yes_no() {
    local prompt="$1"
    local reply
    while true; do
        read -rp "$prompt [y/n]: " reply
        case "$reply" in
            [Yy]*) echo "yes"; return ;;
            [Nn]*) echo "no"; return ;;
            *) echo "Please answer y or n." >&2 ;;
        esac
    done
}

require_root

echo "==============================================================="
echo "  Smart Surveillance System — Secure Setup (Step 1)"
echo "==============================================================="
echo

log "Let's collect a few parameters first."
echo

DO_SSH=$(ask_yes_no "Configure SSH hardening on this machine? (usually: the board)")
DO_MQTT=$(ask_yes_no "Install/configure Mosquitto with authentication on this machine? (usually: the PC)")
DO_SSL=$(ask_yes_no "Generate the self-signed SSL certificate on this machine? (usually: the board)")
DO_SECRETS=$(ask_yes_no "Create a secrets file (.env) for credentials used by your services?")

if [[ "$DO_SSH" == "yes" ]]; then
    echo
    log "SSH configuration"
    echo "  1) Key-based authentication only (recommended, disables password login)"
    echo "  2) Password authentication (with root login disabled)"
    SSH_METHOD=$(ask "Choose SSH auth method (1 or 2)" "1")

    if [[ "$SSH_METHOD" == "1" ]]; then
        echo "Paste your PUBLIC key below (press Ctrl+D on a new line when done):"
        SSH_PUBKEY=$(cat)
        SSH_PUBKEY=$(echo "$SSH_PUBKEY" | tr -d '\r' | tr '\n' ' ' | xargs)

        if [[ -z "$SSH_PUBKEY" ]]; then
            err "Public key cannot be empty!"
            exit 1
        fi

        SSH_USER=$(ask "Which system user should this key be authorized for?" "$(logname 2>/dev/null || echo pi)")
    fi
fi

if [[ "$DO_MQTT" == "yes" ]]; then
    echo
    log "MQTT (Mosquitto) configuration"
    MQTT_USER=$(ask "MQTT username to create" "board_client")
    read -rsp "MQTT password for '$MQTT_USER': " MQTT_PASS
    echo
    read -rsp "Confirm MQTT password: " MQTT_PASS_CONFIRM
    echo
    if [[ "$MQTT_PASS" != "$MQTT_PASS_CONFIRM" ]]; then
        err "Passwords did not match. Re-run the script."
        exit 1
    fi
fi

if [[ "$DO_SSL" == "yes" ]]; then
    echo
    log "SSL certificate configuration"
    STUDENT_ID=$(ask "Enter your student ID (this becomes the certificate CN)")
    if [[ -z "$STUDENT_ID" ]]; then
        err "Student ID cannot be empty — the assignment requires CN = student ID."
        exit 1
    fi
    CERT_DAYS=$(ask "Certificate validity in days" "365")
    CERT_DIR=$(ask "Directory to store the cert/key" "/etc/ssl/surveillance")
fi

if [[ "$DO_SECRETS" == "yes" ]]; then
    echo
    log "Secrets file configuration"
    SECRETS_PATH=$(ask "Where should the secrets file be written?" "/etc/surveillance/secrets.env")
    EMAIL_ADDR=$(ask "Sender email address for notifications (leave blank to fill in later)" "")
    if [[ -n "$EMAIL_ADDR" ]]; then
        read -rsp "Email account password / app password: " EMAIL_PASS
        echo
    fi
fi

echo
log "All parameters collected. Starting setup — no more prompts from here."
echo

#-------------------------------------------------------------------------------
# SSH Hardening
#-------------------------------------------------------------------------------

if [[ "$DO_SSH" == "yes" ]]; then
    log "Hardening SSH configuration..."

    SSHD_CONFIG="/etc/ssh/sshd_config"
    BACKUP="${SSHD_CONFIG}.bak.$(date +%Y%m%d%H%M%S)"
    cp "$SSHD_CONFIG" "$BACKUP"
    log "Backed up existing sshd_config to $BACKUP"

    set_sshd_option() {
        local key="$1" value="$2"
        if grep -qE "^\s*#?\s*${key}\b" "$SSHD_CONFIG"; then
            sed -i -E "s|^\s*#?\s*${key}\b.*|${key} ${value}|" "$SSHD_CONFIG"
        else
            echo "${key} ${value}" >> "$SSHD_CONFIG"
        fi
    }

    set_sshd_option "PermitRootLogin" "no"
    log "Root SSH login disabled."

    if [[ "${SSH_METHOD:-}" == "1" ]]; then
        set_sshd_option "PasswordAuthentication" "no"
        set_sshd_option "PubkeyAuthentication" "yes"

        TARGET_HOME=$(eval echo "~${SSH_USER}")
        mkdir -p "${TARGET_HOME}/.ssh"
        chmod 700 "${TARGET_HOME}/.ssh"
        touch "${TARGET_HOME}/.ssh/authorized_keys"
        grep -qxF "$SSH_PUBKEY" "${TARGET_HOME}/.ssh/authorized_keys" 2>/dev/null || \
            echo "$SSH_PUBKEY" >> "${TARGET_HOME}/.ssh/authorized_keys"
        chmod 600 "${TARGET_HOME}/.ssh/authorized_keys"
        chown -R "${SSH_USER}:${SSH_USER}" "${TARGET_HOME}/.ssh"
        log "Public key authorized for user '${SSH_USER}'. Password login disabled."
    else
        set_sshd_option "PasswordAuthentication" "yes"
        log "Password authentication kept enabled (root login remains disabled)."
    fi

    systemctl restart ssh || systemctl restart sshd
    log "SSH service restarted with new configuration."
    echo
fi

#-------------------------------------------------------------------------------
# Mosquitto Broker Configuration
#-------------------------------------------------------------------------------

if [[ "$DO_MQTT" == "yes" ]]; then
    log "Installing and configuring Mosquitto..."

    if ! command -v mosquitto >/dev/null 2>&1; then
        apt-get update -qq
        apt-get install -y mosquitto mosquitto-clients
    else
        log "Mosquitto already installed, skipping install."
    fi

    MOSQ_CONF_DIR="/etc/mosquitto/conf.d"
    mkdir -p "$MOSQ_CONF_DIR"
    PASSWD_FILE="/etc/mosquitto/passwd"

    # Create user password file cleanly
    mosquitto_passwd -c -b "$PASSWD_FILE" "$MQTT_USER" "$MQTT_PASS"
    
    # Allow mosquitto user/group read access
    chown mosquitto:mosquitto "$PASSWD_FILE" 2>/dev/null || chown root:root "$PASSWD_FILE"
    chmod 644 "$PASSWD_FILE"

    cat > "${MOSQ_CONF_DIR}/auth.conf" <<EOF
# Generated by secure_setup.sh — enforce authenticated MQTT access
listener 1883 0.0.0.0
allow_anonymous false
password_file ${PASSWD_FILE}
EOF

    systemctl restart mosquitto
    systemctl enable mosquitto
    log "Mosquitto configured: binding to 0.0.0.0:1883, anonymous access disabled, user '${MQTT_USER}' created."
    echo
fi

#-------------------------------------------------------------------------------
# SSL Certificate Generation
#-------------------------------------------------------------------------------

if [[ "$DO_SSL" == "yes" ]]; then
    log "Generating self-signed SSL certificate..."

    mkdir -p "$CERT_DIR"
    KEY_FILE="${CERT_DIR}/server.key"
    CERT_FILE="${CERT_DIR}/server.crt"

    openssl req -x509 -nodes \
        -newkey rsa:2048 \
        -keyout "$KEY_FILE" \
        -out "$CERT_FILE" \
        -days "$CERT_DAYS" \
        -subj "/CN=${STUDENT_ID}"

    chmod 600 "$KEY_FILE"
    chmod 644 "$CERT_FILE"

    log "Certificate written to: $CERT_FILE"
    log "Private key written to: $KEY_FILE"
    log "CN is set to your student ID: ${STUDENT_ID}"
    echo
fi

#-------------------------------------------------------------------------------
# Secrets File Generation
#-------------------------------------------------------------------------------

if [[ "$DO_SECRETS" == "yes" ]]; then
    log "Writing secrets file..."

    mkdir -p "$(dirname "$SECRETS_PATH")"
    {
        echo "# Generated by secure_setup.sh on $(date)"
        echo "# Load these as environment variables in your services."
        echo "# Add this file's path to .gitignore — never commit it."
        [[ "${DO_MQTT}" == "yes" ]] && echo "MQTT_USER=${MQTT_USER}"
        [[ "${DO_MQTT}" == "yes" ]] && echo "MQTT_PASS=${MQTT_PASS}"
        [[ -n "${EMAIL_ADDR:-}" ]] && echo "EMAIL_ADDR=${EMAIL_ADDR}"
        [[ -n "${EMAIL_PASS:-}" ]] && echo "EMAIL_PASS=${EMAIL_PASS}"
    } > "$SECRETS_PATH"

    chmod 600 "$SECRETS_PATH"
    log "Secrets written to $SECRETS_PATH (permissions restricted to 600)."
    warn "Remember to add this path to .gitignore."
    echo
fi

echo "==============================================================="
echo "  Setup complete. Summary:"
echo "==============================================================="
[[ "$DO_SSH" == "yes" ]]     && echo "  - SSH: root login disabled, auth method = ${SSH_METHOD:-N/A}"
[[ "$DO_MQTT" == "yes" ]]    && echo "  - MQTT: user '${MQTT_USER:-N/A}' created, listening on 0.0.0.0:1883"
[[ "$DO_SSL" == "yes" ]]     && echo "  - SSL: cert at ${CERT_FILE:-N/A}, CN=${STUDENT_ID:-N/A}"
[[ "$DO_SECRETS" == "yes" ]] && echo "  - Secrets file: ${SECRETS_PATH:-N/A}"
echo