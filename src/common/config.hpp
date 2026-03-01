#pragma once

#include <string>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace kirdi {

// ── Server Configuration ────────────────────────────────────────────────────

struct ServerConfig {
    // Network
    std::string   listen_addr   = "0.0.0.0";
    uint16_t      listen_port   = 7777;       // Internal WS port (behind nginx)
    std::string   ws_path       = "/tunnel/";  // WebSocket upgrade path

    // TLS (if running standalone without nginx)
    bool          tls_enabled   = false;
    std::string   tls_cert_path;
    std::string   tls_key_path;

    // TUN / Virtual network
    std::string   tun_subnet    = "10.8.0.0";  // Virtual subnet
    std::string   tun_mask      = "255.255.255.0";
    std::string   tun_server_ip = "10.8.0.1";  // Server's TUN IP
    uint32_t      mtu           = 1400;         // TUN MTU (leave room for WS overhead)

    // Authentication
    std::string   auth_secret;                  // Shared secret for HMAC-based auth

    // Limits
    uint32_t      max_clients       = 64;
    uint32_t      keepalive_sec     = 25;
    uint32_t      session_timeout_sec = 300;

    // Logging
    std::string   log_level = "info";
};

// ── Client Configuration ────────────────────────────────────────────────────

struct ClientConfig {
    // Connection
    std::string   server_host;
    uint16_t      server_port = 443;
    std::string   ws_path     = "/tunnel/";
    bool          tls_enabled = true;
    std::string   sni_override;                 // TLS SNI override for domain fronting

    // Authentication
    std::string   auth_user   = "kirdi";
    std::string   auth_token;

    // TUN
    uint32_t      mtu         = 1400;
    bool          auto_route  = true;           // Auto-configure routes
    std::string   dns_server  = "1.1.1.1";      // DNS to use through tunnel

    // Transport
    std::string   transport   = "websocket";    // "websocket" or "h2"
    uint32_t      keepalive_sec = 25;

    // Logging
    std::string   log_level = "info";
};

// ── JSON Parsing ────────────────────────────────────────────────────────────

ServerConfig parse_server_config(const std::filesystem::path& path);
ClientConfig parse_client_config(const std::filesystem::path& path);

// Inline JSON serialization support
void to_json(nlohmann::json& j, const ServerConfig& c);
void from_json(const nlohmann::json& j, ServerConfig& c);
void to_json(nlohmann::json& j, const ClientConfig& c);
void from_json(const nlohmann::json& j, ClientConfig& c);

} // namespace kirdi
