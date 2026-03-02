// ── VPN Bridge Implementation ────────────────────────────────────────────────
// Compiled as C++23 — full access to kirdi internals (client, config, etc.)
// This file is the ONLY place where webview-free C++23 code meets the GUI.

#include "gui/gui_bridge.hpp"
#include "client/client.hpp"
#include "common/config.hpp"
#include "common/logger.hpp"

#include <nlohmann/json.hpp>

namespace kirdi::gui::bridge {

// ── Opaque handle ────────────────────────────────────────────────────────────
struct VpnHandle {
    std::unique_ptr<client::Client> client;
    StatusCallback callback;
};

// ── Create ───────────────────────────────────────────────────────────────────
VpnHandle* vpn_create(const std::string& config_json, std::string& error_out) {
    ClientConfig config;
    try {
        auto j = nlohmann::json::parse(config_json);
        config.server_host = j.value("serverHost", "");
        config.server_port = j.value("serverPort", static_cast<uint16_t>(443));
        config.ws_path     = j.value("wsPath", "/ws/");
        config.auth_token  = j.value("authToken", "");
        config.auth_user   = j.value("authUser", "gui-client");
        config.tls_enabled = j.value("useTls", true);
        config.auto_route  = j.value("autoRoute", true);
        config.dns_server  = j.value("dns", "1.1.1.1");
        config.mtu         = j.value("mtu", static_cast<uint32_t>(1400));
    } catch (const std::exception& e) {
        error_out = std::string("Config parse error: ") + e.what();
        return nullptr;
    }

    if (config.server_host.empty()) {
        error_out = "Server host is required";
        return nullptr;
    }
    if (config.auth_token.empty()) {
        error_out = "Auth token is required";
        return nullptr;
    }

    auto handle = new VpnHandle();
    handle->client = std::make_unique<client::Client>(std::move(config));
    return handle;
}

// ── Set callback ─────────────────────────────────────────────────────────────
void vpn_set_callback(VpnHandle* h, StatusCallback cb) {
    if (!h) return;
    h->callback = std::move(cb);
    h->client->set_status_callback([h](const std::string& event, const std::string& json_data) {
        if (h->callback) {
            h->callback(event, json_data);
        }
    });
}

// ── Start (blocking) ─────────────────────────────────────────────────────────
void vpn_start(VpnHandle* h) {
    if (!h || !h->client) return;
    LOG_INFO("Bridge: Starting VPN client");
    try {
        h->client->run();
    } catch (const std::exception& e) {
        LOG_ERRORF("Bridge: Client exception: {}", e.what());
        if (h->callback) {
            h->callback("error", "{\"message\":\"" + std::string(e.what()) + "\"}");
        }
    }
}

// ── Stop (thread-safe) ──────────────────────────────────────────────────────
void vpn_stop(VpnHandle* h) {
    if (!h || !h->client) return;
    LOG_INFO("Bridge: Stopping VPN client");
    h->client->stop();
}

// ── Destroy ─────────────────────────────────────────────────────────────────
void vpn_destroy(VpnHandle* h) {
    if (!h) return;
    vpn_stop(h);
    delete h;
}

} // namespace kirdi::gui::bridge
