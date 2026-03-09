#!/usr/bin/env bash
# ============================================================================
# kirdi integration test helpers: network namespace setup/teardown + configs
# ============================================================================
set -euo pipefail

# ── Constants ───────────────────────────────────────────────────────────────

NS_SERVER="kirdi-ns-server"
NS_CLIENT="kirdi-ns-client"
VETH_SERVER="veth-ks"
VETH_CLIENT="veth-kc"
VETH_SERVER_IP="10.99.0.1"
VETH_CLIENT_IP="10.99.0.2"
VETH_MASK="24"

TUN_SUBNET="10.8.0.0"
TUN_MASK="255.255.255.0"
TUN_SERVER_IP="10.8.0.1"
# First client gets .2 (server allocates from subnet+2)
TUN_CLIENT_IP="10.8.0.2"

SERVER_PORT=17777
WS_PATH="/tunnel/"
TEST_PSK="kirdi_integration_test_psk_$(date +%s)"
MTU=1400

# ── Paths ───────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
TEST_TMP="${TEST_TMP:-$(mktemp -d /tmp/kirdi-test.XXXXXX)}"

SERVER_BIN="$BUILD_DIR/kirdi-server"
CLIENT_BIN="$BUILD_DIR/kirdi-client"

SERVER_LOG="$TEST_TMP/server.log"
CLIENT_LOG="$TEST_TMP/client.log"
SERVER_PID_FILE="$TEST_TMP/server.pid"
CLIENT_PID_FILE="$TEST_TMP/client.pid"
SERVER_CONFIG="$TEST_TMP/server.json"
CLIENT_CONFIG="$TEST_TMP/client.json"
TLS_CERT="$TEST_TMP/test.crt"
TLS_KEY="$TEST_TMP/test.key"

# ── Build ───────────────────────────────────────────────────────────────────

build_if_needed() {
    if [[ -x "$SERVER_BIN" && -x "$CLIENT_BIN" ]]; then
        echo "[build] Binaries exist, skipping build"
        return 0
    fi

    echo "[build] Building kirdi..."
    mkdir -p "$BUILD_DIR"
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DKIRDI_BUILD_TESTS=ON \
        -DKIRDI_BUILD_SERVER=ON \
        -DKIRDI_BUILD_CLIENT=ON \
        2>&1 | tail -5

    cmake --build "$BUILD_DIR" -j "$(nproc)" 2>&1 | tail -10

    if [[ ! -x "$SERVER_BIN" ]]; then
        echo "[build] FATAL: kirdi-server not found at $SERVER_BIN"
        return 1
    fi
    if [[ ! -x "$CLIENT_BIN" ]]; then
        echo "[build] FATAL: kirdi-client not found at $CLIENT_BIN"
        return 1
    fi
    echo "[build] Build complete"
}

# ── TLS Certificates ───────────────────────────────────────────────────────

generate_test_tls() {
    echo "[tls] Generating self-signed certificate for testing"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$TLS_KEY" \
        -out "$TLS_CERT" \
        -days 1 \
        -subj "/CN=kirdi-test" \
        -addext "subjectAltName=IP:${VETH_SERVER_IP}" \
        2>/dev/null
    echo "[tls] Cert: $TLS_CERT  Key: $TLS_KEY"
}

# ── Namespace Management ────────────────────────────────────────────────────

setup_namespaces() {
    echo "[netns] Setting up network namespaces"

    # Clean up any leftovers from a previous failed run
    teardown_namespaces 2>/dev/null || true

    # Create namespaces
    ip netns add "$NS_SERVER"
    ip netns add "$NS_CLIENT"

    # Create veth pair
    ip link add "$VETH_SERVER" type veth peer name "$VETH_CLIENT"

    # Move each end into its namespace
    ip link set "$VETH_SERVER" netns "$NS_SERVER"
    ip link set "$VETH_CLIENT" netns "$NS_CLIENT"

    # Assign IPs and bring up
    ip netns exec "$NS_SERVER" ip addr add "${VETH_SERVER_IP}/${VETH_MASK}" dev "$VETH_SERVER"
    ip netns exec "$NS_SERVER" ip link set lo up
    ip netns exec "$NS_SERVER" ip link set "$VETH_SERVER" up

    ip netns exec "$NS_CLIENT" ip addr add "${VETH_CLIENT_IP}/${VETH_MASK}" dev "$VETH_CLIENT"
    ip netns exec "$NS_CLIENT" ip link set lo up
    ip netns exec "$NS_CLIENT" ip link set "$VETH_CLIENT" up

    # Enable ip_forward in server namespace (needed for TUN packet routing)
    ip netns exec "$NS_SERVER" sysctl -qw net.ipv4.ip_forward=1

    # Ensure /dev/net/tun exists (required for TUN device creation)
    mkdir -p /dev/net
    [[ -c /dev/net/tun ]] || mknod /dev/net/tun c 10 200

    # Verify veth connectivity
    if ! ip netns exec "$NS_CLIENT" ping -c 1 -W 2 "$VETH_SERVER_IP" >/dev/null 2>&1; then
        echo "[netns] FATAL: veth connectivity check failed"
        return 1
    fi

    echo "[netns] Ready: $NS_SERVER ($VETH_SERVER_IP) <-> $NS_CLIENT ($VETH_CLIENT_IP)"
}

teardown_namespaces() {
    echo "[netns] Tearing down"

    # Kill any kirdi processes lingering in namespaces
    for ns in "$NS_SERVER" "$NS_CLIENT"; do
        if ip netns list 2>/dev/null | grep -q "^${ns}"; then
            ip netns exec "$ns" kill -9 $(ip netns exec "$ns" pgrep -f kirdi 2>/dev/null) 2>/dev/null || true
        fi
    done

    # Kill by PID files (belt & suspenders)
    for pidfile in "$SERVER_PID_FILE" "$CLIENT_PID_FILE"; do
        if [[ -f "$pidfile" ]]; then
            kill -9 "$(cat "$pidfile")" 2>/dev/null || true
            rm -f "$pidfile"
        fi
    done

    sleep 0.3

    # Delete namespaces (this also destroys the veth pair)
    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true

    # Clean up temp directory
    if [[ -d "$TEST_TMP" && "$TEST_TMP" == /tmp/kirdi-test.* ]]; then
        rm -rf "$TEST_TMP"
    fi

    echo "[netns] Cleanup complete"
}

# ── Config Generation ───────────────────────────────────────────────────────

generate_test_configs() {
    local psk="${1:-$TEST_PSK}"

    echo "[config] Generating test configs (PSK=${psk:0:16}...)"

    generate_test_tls

    # Server config: TLS enabled (client transport always uses SSL)
    cat > "$SERVER_CONFIG" <<EOJSON
{
    "listen_addr": "${VETH_SERVER_IP}",
    "listen_port": ${SERVER_PORT},
    "ws_path": "${WS_PATH}",
    "tls_enabled": true,
    "tls_cert_path": "${TLS_CERT}",
    "tls_key_path": "${TLS_KEY}",
    "tun_subnet": "${TUN_SUBNET}",
    "tun_mask": "${TUN_MASK}",
    "tun_server_ip": "${TUN_SERVER_IP}",
    "mtu": ${MTU},
    "auth_secret": "${psk}",
    "max_clients": 8,
    "keepalive_sec": 10,
    "session_timeout_sec": 60,
    "log_level": "debug"
}
EOJSON

    # Client config: no auto_route/dns in namespace (we test TUN pings directly)
    cat > "$CLIENT_CONFIG" <<EOJSON
{
    "server_host": "${VETH_SERVER_IP}",
    "server_port": ${SERVER_PORT},
    "ws_path": "${WS_PATH}",
    "tls_enabled": true,
    "auth_user": "test-user",
    "auth_token": "${psk}",
    "mtu": ${MTU},
    "auto_route": false,
    "dns_server": "",
    "transport": "websocket",
    "keepalive_sec": 10,
    "log_level": "debug"
}
EOJSON

    echo "[config] Server: $SERVER_CONFIG"
    echo "[config] Client: $CLIENT_CONFIG"
}

# ── Process Management ──────────────────────────────────────────────────────

start_server() {
    echo "[server] Starting kirdi-server in $NS_SERVER"
    ip netns exec "$NS_SERVER" "$SERVER_BIN" "$SERVER_CONFIG" \
        >"$SERVER_LOG" 2>&1 &
    echo $! > "$SERVER_PID_FILE"
    echo "[server] PID=$(cat "$SERVER_PID_FILE")"
}

wait_server_ready() {
    local timeout="${1:-15}"
    local elapsed=0

    echo "[server] Waiting for server to be ready (timeout=${timeout}s)..."
    while (( elapsed < timeout )); do
        if grep -q "Listening on" "$SERVER_LOG" 2>/dev/null; then
            echo "[server] Ready (${elapsed}s)"
            return 0
        fi
        # Check if process died
        if [[ -f "$SERVER_PID_FILE" ]] && ! kill -0 "$(cat "$SERVER_PID_FILE")" 2>/dev/null; then
            echo "[server] FATAL: process died. Log tail:"
            tail -20 "$SERVER_LOG" 2>/dev/null || true
            return 1
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
    done

    echo "[server] FATAL: timeout waiting for ready. Log tail:"
    tail -20 "$SERVER_LOG" 2>/dev/null || true
    return 1
}

start_client() {
    echo "[client] Starting kirdi-client in $NS_CLIENT"
    ip netns exec "$NS_CLIENT" "$CLIENT_BIN" "$CLIENT_CONFIG" \
        >"$CLIENT_LOG" 2>&1 &
    echo $! > "$CLIENT_PID_FILE"
    echo "[client] PID=$(cat "$CLIENT_PID_FILE")"
}

wait_tunnel_up() {
    local timeout="${1:-20}"
    local elapsed=0

    echo "[client] Waiting for tunnel to establish (timeout=${timeout}s)..."
    while (( elapsed < timeout )); do
        if grep -q "Tunnel active" "$CLIENT_LOG" 2>/dev/null; then
            echo "[client] Tunnel active (${elapsed}s)"
            return 0
        fi
        if grep -q "Authentication rejected" "$CLIENT_LOG" 2>/dev/null; then
            echo "[client] Auth rejected. Log tail:"
            tail -10 "$CLIENT_LOG" 2>/dev/null || true
            return 1
        fi
        # Check if process died
        if [[ -f "$CLIENT_PID_FILE" ]] && ! kill -0 "$(cat "$CLIENT_PID_FILE")" 2>/dev/null; then
            echo "[client] FATAL: process died. Log tail:"
            tail -20 "$CLIENT_LOG" 2>/dev/null || true
            return 1
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
    done

    echo "[client] FATAL: timeout waiting for tunnel. Log tail:"
    tail -20 "$CLIENT_LOG" 2>/dev/null || true
    return 1
}

stop_server() {
    if [[ -f "$SERVER_PID_FILE" ]]; then
        local pid
        pid="$(cat "$SERVER_PID_FILE")"
        echo "[server] Stopping PID=$pid"
        kill -TERM "$pid" 2>/dev/null || true
        # Wait for graceful shutdown
        local i=0
        while (( i < 10 )) && kill -0 "$pid" 2>/dev/null; do
            sleep 0.3
            i=$((i + 1))
        done
        kill -9 "$pid" 2>/dev/null || true
        rm -f "$SERVER_PID_FILE"
    fi
}

stop_client() {
    if [[ -f "$CLIENT_PID_FILE" ]]; then
        local pid
        pid="$(cat "$CLIENT_PID_FILE")"
        echo "[client] Stopping PID=$pid"
        kill -TERM "$pid" 2>/dev/null || true
        local i=0
        while (( i < 10 )) && kill -0 "$pid" 2>/dev/null; do
            sleep 0.3
            i=$((i + 1))
        done
        kill -9 "$pid" 2>/dev/null || true
        rm -f "$CLIENT_PID_FILE"
    fi
}

is_server_running() {
    [[ -f "$SERVER_PID_FILE" ]] && kill -0 "$(cat "$SERVER_PID_FILE")" 2>/dev/null
}

is_client_running() {
    [[ -f "$CLIENT_PID_FILE" ]] && kill -0 "$(cat "$CLIENT_PID_FILE")" 2>/dev/null
}
