#!/usr/bin/env bash
# ============================================================================
# kirdi integration tests — full tunnel lifecycle via network namespaces
#
# Tests:
#   1. Ping through tunnel (client -> server TUN IP)
#   2. Bidirectional ping (server -> client TUN IP)
#   3. Large packet / MTU test (ping -s 1300)
#   4. Graceful shutdown (SIGTERM server, client detects)
#   5. Reconnect (restart server + client, tunnel re-establishes)
#   6. Auth failure (wrong PSK, connection refused)
#
# Requires: root, Linux, ip, ping, openssl, cmake toolchain
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source helper library
# shellcheck source=setup_netns.sh
source "$SCRIPT_DIR/setup_netns.sh"

# ── Test Framework ──────────────────────────────────────────────────────────

TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0
declare -a TEST_RESULTS=()

run_test() {
    local name="$1"
    local func="$2"

    echo ""
    echo "================================================================"
    echo "  TEST: $name"
    echo "================================================================"

    local rc=0
    "$func" || rc=$?

    if (( rc == 0 )); then
        echo "  RESULT: PASS"
        TEST_RESULTS+=("PASS  $name")
        TESTS_PASSED=$((TESTS_PASSED + 1))
    elif (( rc == 77 )); then
        echo "  RESULT: SKIP"
        TEST_RESULTS+=("SKIP  $name")
        TESTS_SKIPPED=$((TESTS_SKIPPED + 1))
    else
        echo "  RESULT: FAIL (rc=$rc)"
        TEST_RESULTS+=("FAIL  $name")
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

print_report() {
    echo ""
    echo "================================================================"
    echo "  TEST REPORT"
    echo "================================================================"
    for r in "${TEST_RESULTS[@]}"; do
        echo "  $r"
    done
    echo "----------------------------------------------------------------"
    echo "  Passed:  $TESTS_PASSED"
    echo "  Failed:  $TESTS_FAILED"
    echo "  Skipped: $TESTS_SKIPPED"
    echo "  Total:   $(( TESTS_PASSED + TESTS_FAILED + TESTS_SKIPPED ))"
    echo "================================================================"
}

# ── Preflight Checks ───────────────────────────────────────────────────────

preflight() {
    if (( EUID != 0 )); then
        echo "FATAL: Must run as root (need network namespaces + TUN)"
        exit 1
    fi

    for cmd in ip ping openssl cmake; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            echo "FATAL: Required command '$cmd' not found"
            exit 1
        fi
    done

    # Check kernel supports network namespaces
    if ! ip netns list >/dev/null 2>&1; then
        echo "FATAL: Kernel does not support network namespaces"
        exit 1
    fi
}

# ── Cleanup Trap ────────────────────────────────────────────────────────────

cleanup() {
    local rc=$?
    echo ""
    echo "[cleanup] Cleaning up (exit code=$rc)..."
    stop_client 2>/dev/null || true
    stop_server 2>/dev/null || true
    teardown_namespaces 2>/dev/null || true
    cleanup_test_tmp 2>/dev/null || true
    return 0
}
trap cleanup EXIT

# ── Test Implementations ────────────────────────────────────────────────────

test_ping_through_tunnel() {
    # Client namespace pings server's TUN IP (10.8.0.1)
    # This verifies: WS connection, auth, TUN creation, IP packet forwarding
    echo "  Pinging $TUN_SERVER_IP from client namespace..."
    if ip netns exec "$NS_CLIENT" ping -c 3 -W 3 -I "$TUN_CLIENT_IP" "$TUN_SERVER_IP" 2>&1; then
        echo "  Tunnel ping successful"
        return 0
    else
        echo "  Tunnel ping failed"
        echo "  --- Server log tail ---"
        tail -30 "$SERVER_LOG" 2>/dev/null || true
        echo "  --- Client log tail ---"
        tail -30 "$CLIENT_LOG" 2>/dev/null || true
        return 1
    fi
}

test_bidirectional_ping() {
    # Server namespace pings client's TUN IP (10.8.0.2)
    # This verifies: server->client routing via ip_to_session_ lookup
    echo "  Pinging $TUN_CLIENT_IP from server namespace..."
    if ip netns exec "$NS_SERVER" ping -c 3 -W 3 "$TUN_CLIENT_IP" 2>&1; then
        echo "  Bidirectional ping successful"
        return 0
    else
        echo "  Bidirectional ping failed"
        echo "  --- Server log tail ---"
        tail -20 "$SERVER_LOG" 2>/dev/null || true
        return 1
    fi
}

test_large_packet_mtu() {
    # Send large ICMP packets (1300 bytes payload + 28 header = 1328, fits in MTU 1400)
    echo "  Sending large packets (1300 byte payload) through tunnel..."
    if ip netns exec "$NS_CLIENT" ping -c 3 -W 5 -s 1300 -I "$TUN_CLIENT_IP" "$TUN_SERVER_IP" 2>&1; then
        echo "  Large packet test passed"
        return 0
    else
        echo "  Large packet test failed"
        return 1
    fi
}

test_graceful_shutdown() {
    # Send SIGTERM to server, verify client detects disconnection
    echo "  Sending SIGTERM to server..."

    if ! is_server_running; then
        echo "  Server not running, cannot test shutdown"
        return 1
    fi

    local server_pid
    server_pid="$(cat "$SERVER_PID_FILE")"
    kill -TERM "$server_pid"

    # Wait for server to exit
    local elapsed=0
    while (( elapsed < 10 )); do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "  Server exited after ${elapsed}s"
            break
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
    done

    if kill -0 "$server_pid" 2>/dev/null; then
        echo "  Server did not exit gracefully, force killing"
        kill -9 "$server_pid" 2>/dev/null || true
    fi

    # Give client time to detect disconnection
    sleep 2

    # Client should still be running (reconnect loop) or have logged the error
    if grep -qE "(Transport error|error|shutting down)" "$CLIENT_LOG" 2>/dev/null; then
        echo "  Client detected server shutdown"
        # Verify client process is still alive (in reconnect loop)
        if is_client_running; then
            echo "  Client still running (reconnect mode) -- good"
        else
            echo "  Client exited (acceptable)"
        fi
        return 0
    else
        echo "  Client did not detect server shutdown within timeout"
        return 1
    fi
}

test_reconnect() {
    # After shutdown test: restart server, restart client, verify tunnel re-establishes
    echo "  Stopping any remaining processes..."
    stop_client
    stop_server

    # Clear old logs
    : > "$SERVER_LOG"
    : > "$CLIENT_LOG"

    echo "  Restarting server..."
    start_server
    if ! wait_server_ready 15; then
        echo "  Server failed to restart"
        return 1
    fi

    echo "  Restarting client..."
    start_client
    if ! wait_tunnel_up 20; then
        echo "  Tunnel failed to re-establish"
        return 1
    fi

    # Verify connectivity
    sleep 1
    echo "  Verifying tunnel connectivity after reconnect..."
    if ip netns exec "$NS_CLIENT" ping -c 2 -W 3 -I "$TUN_CLIENT_IP" "$TUN_SERVER_IP" >/dev/null 2>&1; then
        echo "  Reconnect test passed"
        return 0
    else
        echo "  Ping failed after reconnect"
        return 1
    fi
}

test_auth_failure() {
    # Stop everything, restart server, start client with WRONG PSK
    echo "  Stopping any remaining processes..."
    stop_client
    stop_server

    # Clear logs
    : > "$SERVER_LOG"
    : > "$CLIENT_LOG"

    echo "  Restarting server with correct PSK..."
    start_server
    if ! wait_server_ready 15; then
        echo "  Server failed to start for auth failure test"
        return 1
    fi

    # Create a client config with wrong PSK
    local bad_config="$TEST_TMP/client_bad.json"
    cat > "$bad_config" <<EOJSON
{
    "server_host": "${VETH_SERVER_IP}",
    "server_port": ${SERVER_PORT},
    "ws_path": "${WS_PATH}",
    "tls_enabled": true,
    "auth_user": "test-user",
    "auth_token": "completely_wrong_secret_key",
    "mtu": ${MTU},
    "auto_route": false,
    "dns_server": "",
    "transport": "websocket",
    "keepalive_sec": 10,
    "log_level": "debug"
}
EOJSON

    local bad_log="$TEST_TMP/client_bad.log"
    echo "  Starting client with wrong PSK..."
    ip netns exec "$NS_CLIENT" "$CLIENT_BIN" "$bad_config" \
        >"$bad_log" 2>&1 &
    local bad_pid=$!

    # Wait for the client to attempt auth and get rejected
    local elapsed=0
    local auth_failed=false
    while (( elapsed < 15 )); do
        if grep -q "Authentication rejected" "$bad_log" 2>/dev/null; then
            auth_failed=true
            break
        fi
        # Server-side check: auth FAILED log
        if grep -q "auth FAILED" "$SERVER_LOG" 2>/dev/null; then
            auth_failed=true
            break
        fi
        # Check if client exited (expected after auth failure)
        if ! kill -0 "$bad_pid" 2>/dev/null; then
            auth_failed=true
            break
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
    done

    # Clean up the bad client
    kill -9 "$bad_pid" 2>/dev/null || true
    wait "$bad_pid" 2>/dev/null || true

    if $auth_failed; then
        echo "  Auth failure correctly detected"
        # Also verify the "Tunnel active" message is NOT in the bad client log
        if grep -q "Tunnel active" "$bad_log" 2>/dev/null; then
            echo "  ERROR: Tunnel was established despite wrong PSK!"
            return 1
        fi
        echo "  Confirmed: no tunnel was established"
        return 0
    else
        echo "  Auth failure was NOT detected within timeout"
        echo "  --- Bad client log ---"
        cat "$bad_log" 2>/dev/null || true
        echo "  --- Server log tail ---"
        tail -20 "$SERVER_LOG" 2>/dev/null || true
        return 1
    fi
}

# ── Main ────────────────────────────────────────────────────────────────────

main() {
    echo "============================================================"
    echo "  kirdi integration tests"
    echo "============================================================"
    echo "  Project: $PROJECT_ROOT"
    echo "  Temp:    $TEST_TMP"
    echo ""

    preflight
    build_if_needed
    setup_namespaces
    generate_test_configs

    # ── Start server + client for tests 1-3 ─────────────────────────────
    start_server
    if ! wait_server_ready 15; then
        echo "FATAL: Server failed to start"
        exit 1
    fi

    start_client
    if ! wait_tunnel_up 20; then
        echo "FATAL: Tunnel failed to establish"
        echo "--- Server log ---"
        cat "$SERVER_LOG"
        echo "--- Client log ---"
        cat "$CLIENT_LOG"
        exit 1
    fi

    # Allow tunnel to stabilize
    sleep 1

    # ── Run tests ───────────────────────────────────────────────────────
    run_test "Ping through tunnel"          test_ping_through_tunnel
    run_test "Bidirectional ping"           test_bidirectional_ping
    run_test "Large packet / MTU test"      test_large_packet_mtu
    run_test "Graceful shutdown (SIGTERM)"   test_graceful_shutdown
    run_test "Reconnect after restart"       test_reconnect
    run_test "Auth failure (wrong PSK)"      test_auth_failure

    # ── Report ──────────────────────────────────────────────────────────
    print_report

    if (( TESTS_FAILED > 0 )); then
        exit 1
    fi
    exit 0
}

main "$@"
