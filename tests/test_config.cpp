#include <gtest/gtest.h>
#include "common/config.hpp"
#include <nlohmann/json.hpp>

using namespace kirdi;

TEST(Config, ServerConfigDefaults) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.listen_port, 7777);
    EXPECT_EQ(cfg.mtu, 1400u);
    EXPECT_EQ(cfg.max_clients, 64u);
    EXPECT_EQ(cfg.tun_server_ip, "10.8.0.1");
}

TEST(Config, ServerConfigJsonRoundtrip) {
    ServerConfig original;
    original.listen_port = 8080;
    original.auth_secret = "test_secret";
    original.tun_subnet = "10.99.0.0";

    nlohmann::json j = original;
    auto parsed = j.get<ServerConfig>();

    EXPECT_EQ(parsed.listen_port, 8080);
    EXPECT_EQ(parsed.auth_secret, "test_secret");
    EXPECT_EQ(parsed.tun_subnet, "10.99.0.0");
}

TEST(Config, ClientConfigDefaults) {
    ClientConfig cfg;
    EXPECT_EQ(cfg.server_port, 443);
    EXPECT_EQ(cfg.tls_enabled, true);
    EXPECT_EQ(cfg.auto_route, true);
    EXPECT_EQ(cfg.transport, "websocket");
}

TEST(Config, ClientConfigJsonRoundtrip) {
    ClientConfig original;
    original.server_host = "vpn.example.com";
    original.auth_token = "abcdef123456";
    original.auto_route = false;

    nlohmann::json j = original;
    auto parsed = j.get<ClientConfig>();

    EXPECT_EQ(parsed.server_host, "vpn.example.com");
    EXPECT_EQ(parsed.auth_token, "abcdef123456");
    EXPECT_EQ(parsed.auto_route, false);
}

TEST(Config, PartialJsonParsing) {
    // Only set a few fields — rest should keep defaults
    nlohmann::json j = {
        {"server_host", "example.com"},
        {"server_port", 8443},
    };

    auto cfg = j.get<ClientConfig>();
    EXPECT_EQ(cfg.server_host, "example.com");
    EXPECT_EQ(cfg.server_port, 8443);
    EXPECT_EQ(cfg.tls_enabled, true);  // Default preserved
    EXPECT_EQ(cfg.mtu, 1400u);         // Default preserved
}
