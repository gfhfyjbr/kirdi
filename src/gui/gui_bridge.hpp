#pragma once

// ── VPN Bridge Interface ─────────────────────────────────────────────────────
// Pure C++ header with ZERO dependencies on kirdi internals or webview.
// This is the firewall between C++17 (webview/.mm) and C++23 (kirdi core).
// Only standard library types here.

#include <string>
#include <functional>
#include <memory>

namespace kirdi::gui::bridge {

// Callback from VPN core → GUI controller (runs on I/O thread)
// event: "connecting", "authenticating", "connected", "disconnected", "error", "reconnecting"
// json_data: additional info as JSON string
using StatusCallback = std::function<void(const std::string& event, const std::string& json_data)>;

// Opaque handle to the VPN client
struct VpnHandle;

// Create a VPN client from JSON config string.
// Returns nullptr on parse error (sets error_out).
VpnHandle* vpn_create(const std::string& config_json, std::string& error_out);

// Set status callback (must be called before vpn_start)
void vpn_set_callback(VpnHandle* h, StatusCallback cb);

// Start VPN client (blocks until stopped — run in a thread!)
void vpn_start(VpnHandle* h);

// Stop VPN client (thread-safe, can be called from any thread)
void vpn_stop(VpnHandle* h);

// Destroy VPN client handle (stops first if running)
void vpn_destroy(VpnHandle* h);

} // namespace kirdi::gui::bridge
