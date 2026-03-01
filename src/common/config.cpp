#include "common/config.hpp"
#include "common/logger.hpp"
#include <fstream>

namespace kirdi {

// Helper: get value from json with primary key or fallback alias
template<typename T>
static void json_get(const nlohmann::json& j, const char* key, const char* alias, T& out) {
    if (j.contains(key))        { j.at(key).get_to(out); return; }
    if (alias && j.contains(alias)) { j.at(alias).get_to(out); return; }
}

// Helper: strip CIDR suffix from subnet string ("10.8.0.0/24" → "10.8.0.0")
static std::string strip_cidr(const std::string& s) {
    auto pos = s.find('/');
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// ── ServerConfig JSON ───────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const ServerConfig& c) {
    j = nlohmann::json{
        {"listen_addr",       c.listen_addr},
        {"listen_port",       c.listen_port},
        {"ws_path",           c.ws_path},
        {"tls_enabled",       c.tls_enabled},
        {"tls_cert_path",     c.tls_cert_path},
        {"tls_key_path",      c.tls_key_path},
        {"tun_subnet",        c.tun_subnet},
        {"tun_mask",          c.tun_mask},
        {"tun_server_ip",     c.tun_server_ip},
        {"mtu",               c.mtu},
        {"auth_secret",       c.auth_secret},
        {"max_clients",       c.max_clients},
        {"keepalive_sec",     c.keepalive_sec},
        {"session_timeout_sec", c.session_timeout_sec},
        {"log_level",         c.log_level},
    };
}

void from_json(const nlohmann::json& j, ServerConfig& c) {
    // Accept both canonical and common alias keys
    json_get(j, "listen_addr",    "listen_address",      c.listen_addr);
    json_get(j, "listen_port",    nullptr,               c.listen_port);
    json_get(j, "ws_path",        nullptr,               c.ws_path);
    json_get(j, "tls_enabled",    nullptr,               c.tls_enabled);
    json_get(j, "tls_cert_path",  "tls_cert",            c.tls_cert_path);
    json_get(j, "tls_key_path",   "tls_key",             c.tls_key_path);
    json_get(j, "tun_subnet",     nullptr,               c.tun_subnet);
    json_get(j, "tun_mask",       "tun_netmask",         c.tun_mask);
    json_get(j, "tun_server_ip",  nullptr,               c.tun_server_ip);
    json_get(j, "mtu",            nullptr,               c.mtu);
    json_get(j, "auth_secret",    "auth_token",          c.auth_secret);
    json_get(j, "max_clients",    nullptr,               c.max_clients);
    json_get(j, "keepalive_sec",  "keepalive_interval",  c.keepalive_sec);
    json_get(j, "session_timeout_sec", nullptr,           c.session_timeout_sec);
    json_get(j, "log_level",      nullptr,               c.log_level);

    // Strip CIDR notation from subnet if present ("10.8.0.0/24" → "10.8.0.0")
    c.tun_subnet = strip_cidr(c.tun_subnet);
}

// ── ClientConfig JSON ───────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const ClientConfig& c) {
    j = nlohmann::json{
        {"server_host",   c.server_host},
        {"server_port",   c.server_port},
        {"ws_path",       c.ws_path},
        {"tls_enabled",   c.tls_enabled},
        {"sni_override",  c.sni_override},
        {"auth_user",     c.auth_user},
        {"auth_token",    c.auth_token},
        {"mtu",           c.mtu},
        {"auto_route",    c.auto_route},
        {"dns_server",    c.dns_server},
        {"transport",     c.transport},
        {"keepalive_sec", c.keepalive_sec},
        {"log_level",     c.log_level},
    };
}

void from_json(const nlohmann::json& j, ClientConfig& c) {
    json_get(j, "server_host",   "host",             c.server_host);
    json_get(j, "server_port",   "port",             c.server_port);
    json_get(j, "ws_path",       "path",             c.ws_path);
    json_get(j, "tls_enabled",   nullptr,            c.tls_enabled);
    json_get(j, "sni_override",  "sni",              c.sni_override);
    json_get(j, "auth_user",     "user",             c.auth_user);
    json_get(j, "auth_token",    "token",            c.auth_token);
    json_get(j, "mtu",           nullptr,            c.mtu);
    json_get(j, "auto_route",    nullptr,            c.auto_route);
    json_get(j, "dns_server",    "dns",              c.dns_server);
    json_get(j, "transport",     nullptr,            c.transport);
    json_get(j, "keepalive_sec", "keepalive",        c.keepalive_sec);
    json_get(j, "log_level",     nullptr,            c.log_level);
}

// ── File Parsers ────────────────────────────────────────────────────────────

ServerConfig parse_server_config(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open server config: " + path.string());
    }
    auto j = nlohmann::json::parse(ifs);
    return j.get<ServerConfig>();
}

ClientConfig parse_client_config(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open client config: " + path.string());
    }
    auto j = nlohmann::json::parse(ifs);
    return j.get<ClientConfig>();
}

} // namespace kirdi
