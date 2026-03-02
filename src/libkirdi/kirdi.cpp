/**
 * kirdi VPN Client — C API Implementation
 *
 * Compiled as C++23. Wraps the kirdi::client::Client class with extern "C" functions.
 * This is the bridge between any FFI consumer (Rust, Python, etc.) and the C++ VPN core.
 */

#include <kirdi/kirdi.h>
#include <kirdi/version.hpp>
#include "client/client.hpp"
#include "common/config.hpp"
#include "common/logger.hpp"

#include <nlohmann/json.hpp>
#include <memory>
#include <string>

/* ── Opaque handle ─────────────────────────────────────────────────────── */

struct kirdi_client {
    std::unique_ptr<kirdi::client::Client> client;
    kirdi_status_cb_t callback = nullptr;
    void* userdata = nullptr;
};

/* ── Config defaults ───────────────────────────────────────────────────── */

extern "C" void kirdi_config_init(kirdi_config_t* cfg) {
    if (!cfg) return;
    cfg->server_host  = nullptr;
    cfg->server_port  = 443;
    cfg->ws_path      = "/tunnel/";
    cfg->auth_token   = nullptr;
    cfg->auth_user    = "kirdi";
    cfg->tls_enabled  = 1;
    cfg->auto_route   = 1;
    cfg->dns_server   = "1.1.1.1";
    cfg->mtu          = 1400;
    cfg->sni_override = nullptr;
    cfg->log_level    = "info";
}

/* ── Helper: C config → C++ config ─────────────────────────────────────── */

static kirdi::ClientConfig to_cpp_config(const kirdi_config_t* cfg) {
    kirdi::ClientConfig cc;
    if (cfg->server_host)  cc.server_host  = cfg->server_host;
    cc.server_port  = cfg->server_port;
    if (cfg->ws_path)      cc.ws_path      = cfg->ws_path;
    if (cfg->auth_token)   cc.auth_token   = cfg->auth_token;
    if (cfg->auth_user)    cc.auth_user    = cfg->auth_user;
    cc.tls_enabled  = cfg->tls_enabled != 0;
    cc.auto_route   = cfg->auto_route != 0;
    if (cfg->dns_server)   cc.dns_server   = cfg->dns_server;
    cc.mtu          = cfg->mtu > 0 ? cfg->mtu : 1400;
    if (cfg->sni_override) cc.sni_override = cfg->sni_override;
    if (cfg->log_level)    cc.log_level    = cfg->log_level;
    return cc;
}

/* ── Helper: set log level ─────────────────────────────────────────────── */

static void apply_log_level(const std::string& level) {
    if (level == "trace")      kirdi::Logger::instance().set_level(kirdi::LogLevel::Trace);
    else if (level == "debug") kirdi::Logger::instance().set_level(kirdi::LogLevel::Debug);
    else if (level == "warn")  kirdi::Logger::instance().set_level(kirdi::LogLevel::Warn);
    else if (level == "error") kirdi::Logger::instance().set_level(kirdi::LogLevel::Error);
    // default = info (already set)
}

/* ── Create from struct ────────────────────────────────────────────────── */

extern "C" kirdi_client_t* kirdi_create(const kirdi_config_t* cfg) {
    if (!cfg || !cfg->server_host || !cfg->auth_token) {
        return nullptr;
    }

    auto cc = to_cpp_config(cfg);
    apply_log_level(cc.log_level);

    auto handle = new (std::nothrow) kirdi_client();
    if (!handle) return nullptr;

    try {
        handle->client = std::make_unique<kirdi::client::Client>(std::move(cc));
    } catch (...) {
        delete handle;
        return nullptr;
    }

    return handle;
}

/* ── Create from JSON ──────────────────────────────────────────────────── */

extern "C" kirdi_client_t* kirdi_create_json(const char* json_str) {
    if (!json_str) return nullptr;

    try {
        auto j = nlohmann::json::parse(json_str);

        kirdi_config_t cfg;
        kirdi_config_init(&cfg);

        // We need stable strings, so use static thread_local storage
        static thread_local std::string s_host, s_path, s_token, s_user,
                                        s_dns, s_sni, s_log;

        s_host  = j.value("serverHost", "");
        s_path  = j.value("wsPath", "/tunnel/");
        s_token = j.value("authToken", "");
        s_user  = j.value("authUser", "kirdi");
        s_dns   = j.value("dns", "1.1.1.1");
        s_sni   = j.value("sniOverride", "");
        s_log   = j.value("logLevel", "info");

        cfg.server_host  = s_host.c_str();
        cfg.server_port  = j.value("serverPort", static_cast<uint16_t>(443));
        cfg.ws_path      = s_path.c_str();
        cfg.auth_token   = s_token.c_str();
        cfg.auth_user    = s_user.c_str();
        cfg.tls_enabled  = j.value("useTls", true) ? 1 : 0;
        cfg.auto_route   = j.value("autoRoute", true) ? 1 : 0;
        cfg.dns_server   = s_dns.c_str();
        cfg.mtu          = j.value("mtu", static_cast<uint32_t>(1400));
        cfg.sni_override = s_sni.empty() ? nullptr : s_sni.c_str();
        cfg.log_level    = s_log.c_str();

        return kirdi_create(&cfg);
    } catch (...) {
        return nullptr;
    }
}

/* ── Set callback ──────────────────────────────────────────────────────── */

extern "C" void kirdi_set_callback(kirdi_client_t* client, kirdi_status_cb_t cb, void* userdata) {
    if (!client || !client->client) return;

    client->callback = cb;
    client->userdata = userdata;

    client->client->set_status_callback(
        [client](const std::string& event, const std::string& json_data) {
            if (client->callback) {
                client->callback(event.c_str(), json_data.c_str(), client->userdata);
            }
        }
    );
}

/* ── Run (blocking) ────────────────────────────────────────────────────── */

extern "C" int kirdi_run(kirdi_client_t* client) {
    if (!client || !client->client) return -1;

    try {
        client->client->run();
        return 0;
    } catch (const std::exception& e) {
        LOG_ERRORF("kirdi_run exception: {}", e.what());
        if (client->callback) {
            std::string msg = std::string("{\"message\":\"") + e.what() + "\"}";
            client->callback("error", msg.c_str(), client->userdata);
        }
        return -1;
    }
}

/* ── Stop (thread-safe) ───────────────────────────────────────────────── */

extern "C" void kirdi_stop(kirdi_client_t* client) {
    if (!client || !client->client) return;
    client->client->stop();
}

/* ── Destroy ───────────────────────────────────────────────────────────── */

extern "C" void kirdi_destroy(kirdi_client_t* client) {
    if (!client) return;
    if (client->client) {
        try { client->client->stop(); } catch (...) {}
    }
    delete client;
}

/* ── Version ───────────────────────────────────────────────────────────── */

extern "C" const char* kirdi_version(void) {
    return KIRDI_VERSION_STRING;
}
