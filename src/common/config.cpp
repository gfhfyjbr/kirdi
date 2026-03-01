#include "common/config.hpp"
#include "common/logger.hpp"
#include <fstream>

namespace kirdi {

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
    if (j.contains("listen_addr"))        j.at("listen_addr").get_to(c.listen_addr);
    if (j.contains("listen_port"))        j.at("listen_port").get_to(c.listen_port);
    if (j.contains("ws_path"))            j.at("ws_path").get_to(c.ws_path);
    if (j.contains("tls_enabled"))        j.at("tls_enabled").get_to(c.tls_enabled);
    if (j.contains("tls_cert_path"))      j.at("tls_cert_path").get_to(c.tls_cert_path);
    if (j.contains("tls_key_path"))       j.at("tls_key_path").get_to(c.tls_key_path);
    if (j.contains("tun_subnet"))         j.at("tun_subnet").get_to(c.tun_subnet);
    if (j.contains("tun_mask"))           j.at("tun_mask").get_to(c.tun_mask);
    if (j.contains("tun_server_ip"))      j.at("tun_server_ip").get_to(c.tun_server_ip);
    if (j.contains("mtu"))                j.at("mtu").get_to(c.mtu);
    if (j.contains("auth_secret"))        j.at("auth_secret").get_to(c.auth_secret);
    if (j.contains("max_clients"))        j.at("max_clients").get_to(c.max_clients);
    if (j.contains("keepalive_sec"))      j.at("keepalive_sec").get_to(c.keepalive_sec);
    if (j.contains("session_timeout_sec")) j.at("session_timeout_sec").get_to(c.session_timeout_sec);
    if (j.contains("log_level"))          j.at("log_level").get_to(c.log_level);
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
    if (j.contains("server_host"))   j.at("server_host").get_to(c.server_host);
    if (j.contains("server_port"))   j.at("server_port").get_to(c.server_port);
    if (j.contains("ws_path"))       j.at("ws_path").get_to(c.ws_path);
    if (j.contains("tls_enabled"))   j.at("tls_enabled").get_to(c.tls_enabled);
    if (j.contains("sni_override"))  j.at("sni_override").get_to(c.sni_override);
    if (j.contains("auth_user"))     j.at("auth_user").get_to(c.auth_user);
    if (j.contains("auth_token"))    j.at("auth_token").get_to(c.auth_token);
    if (j.contains("mtu"))           j.at("mtu").get_to(c.mtu);
    if (j.contains("auto_route"))    j.at("auto_route").get_to(c.auto_route);
    if (j.contains("dns_server"))    j.at("dns_server").get_to(c.dns_server);
    if (j.contains("transport"))     j.at("transport").get_to(c.transport);
    if (j.contains("keepalive_sec")) j.at("keepalive_sec").get_to(c.keepalive_sec);
    if (j.contains("log_level"))     j.at("log_level").get_to(c.log_level);
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
