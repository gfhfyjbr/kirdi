#include "client/client.hpp"
#include "common/logger.hpp"
#include "common/crypto.hpp"
#include <kirdi/version.hpp>

#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <netdb.h>
#include <arpa/inet.h>

namespace kirdi::client {

Client::Client(ClientConfig config) : config_(std::move(config)) {
}

Client::~Client() {
    try { stop(); } catch (...) {}
}

// Resolve hostname to IP string (needed for route exclusion)
static std::string resolve_host(const std::string& host) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        return host;  // fallback: assume it's already an IP
    }
    char ip[INET_ADDRSTRLEN];
    auto* sa = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
    freeaddrinfo(res);
    return std::string(ip);
}

// Get current default gateway IP
static std::string get_default_gateway() {
#if defined(KIRDI_PLATFORM_MACOS)
    // macOS: route -n get default
    FILE* fp = popen("route -n get default 2>/dev/null | grep gateway | awk '{print $2}'", "r");
    if (!fp) return "";
    char buf[128] = {};
    if (fgets(buf, sizeof(buf), fp)) {
        // strip newline
        for (char* p = buf; *p; ++p) if (*p == '\n' || *p == '\r') *p = 0;
    }
    pclose(fp);
    return std::string(buf);
#elif defined(KIRDI_PLATFORM_LINUX)
    FILE* fp = popen("ip route show default 2>/dev/null | awk '/default/ {print $3}'", "r");
    if (!fp) return "";
    char buf[128] = {};
    if (fgets(buf, sizeof(buf), fp)) {
        for (char* p = buf; *p; ++p) if (*p == '\n' || *p == '\r') *p = 0;
    }
    pclose(fp);
    return std::string(buf);
#else
    return "";
#endif
}

void Client::run() {
    LOG_INFOF("kirdi client v{} starting", KIRDI_VERSION_STRING);
    running_ = true;

    // Resolve server IP and get gateway BEFORE connecting
    // (needed later for route exclusion to prevent routing loop)
    server_ip_ = resolve_host(config_.server_host);
    original_gateway_ = get_default_gateway();
    LOG_INFOF("Server IP: {} ({}), default gateway: {}", server_ip_, config_.server_host, original_gateway_);

    // Configure TLS
    ssl_ctx_.set_default_verify_paths();
    if (!config_.sni_override.empty()) {
        LOG_INFOF("Using SNI override: {}", config_.sni_override);
    }

    // Create WebSocket transport
    ws_ = std::make_shared<transport::WsClientTransport>(ioc_, ssl_ctx_);

    ws_->on_connect([this]() { on_connected(); });
    ws_->on_packet([this](const protocol::PacketHeader& hdr, std::vector<uint8_t> payload) {
        on_packet(hdr, std::move(payload));
    });
    ws_->on_error([this](const std::string& err) { on_error(err); });

    // Connect
    ws_->connect(config_.server_host, config_.server_port, config_.ws_path);

    // Run I/O context
    ioc_.run();
}

void Client::stop() {
    if (!running_.exchange(false)) return;

    LOG_INFO("Client stopping");

    if (ws_) ws_->close();
    ioc_.stop();

    try { teardown_routes(); } catch (...) {}
    if (tun_) {
        try { tun_->close(); } catch (...) {}
    }
}

void Client::on_connected() {
    LOG_INFO("Connected to server -- sending auth");
    send_auth();
}

void Client::send_auth() {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    auto token = crypto::build_auth_token(config_.auth_user, config_.auth_token,
                                           static_cast<uint64_t>(now));

    nlohmann::json auth_json = {
        {"user", config_.auth_user},
        {"token", token},
        {"timestamp", now},
    };
    std::string auth_str = auth_json.dump();

    auto pkt = protocol::build_packet(
        protocol::MsgType::AuthRequest,
        {reinterpret_cast<const uint8_t*>(auth_str.data()), auth_str.size()}
    );
    ws_->send(std::move(pkt));
}

void Client::on_packet(const protocol::PacketHeader& hdr, std::vector<uint8_t> payload) {
    switch (hdr.type) {
        case protocol::MsgType::AuthResponse: {
            try {
                auto j = nlohmann::json::parse(payload.begin(), payload.end());
                if (j.value("ok", false)) {
                    virtual_ip_ = j.value("tun_ip", "10.8.0.2");
                    std::string server_tun_ip = j.value("tun_server_ip", "10.8.0.1");
                    std::string tun_mask = j.value("tun_mask", "255.255.255.0");
                    uint32_t tun_mtu = j.value("mtu", config_.mtu);

                    LOG_INFOF("Authenticated! VIP={} server_tun={} mask={} mtu={}",
                              virtual_ip_, server_tun_ip, tun_mask, tun_mtu);

                    tun_ = tun::create_tun_device();
                    tun::TunConfig tun_cfg{
                        .name = "kirdi0",
                        .address = virtual_ip_,
                        .peer_address = server_tun_ip,
                        .netmask = tun_mask,
                        .mtu = tun_mtu,
                    };

                    auto result = tun_->open(tun_cfg);
                    if (!result) {
                        LOG_ERROR("Failed to create TUN device");
                        stop();
                        return;
                    }

                    if (config_.auto_route) {
                        setup_routes();
                    }

                    std::thread tun_thread([this]() { tun_read_loop(); });
                    tun_thread.detach();

                    LOG_INFO("Tunnel active -- all traffic routed through server");
                } else {
                    LOG_ERROR("Authentication rejected by server");
                    stop();
                }
            } catch (const std::exception& e) {
                LOG_ERRORF("Failed to parse auth response: {}", e.what());
            }
            break;
        }

        case protocol::MsgType::IpPacket:
            if (tun_ && tun_->is_open()) {
                tun_->write_packet(payload.data(), payload.size());
            }
            break;

        case protocol::MsgType::Keepalive:
            if (ws_ && ws_->is_connected()) {
                ws_->send(protocol::build_keepalive());
            }
            break;

        case protocol::MsgType::Pong:
            break;

        default:
            LOG_WARNF("Unknown message type 0x{:02x}", static_cast<uint8_t>(hdr.type));
            break;
    }
}

void Client::on_error(const std::string& err) {
    LOG_ERRORF("Transport error: {}", err);
}

void Client::tun_read_loop() {
    LOG_INFO("TUN read loop started");

    while (running_.load() && tun_ && tun_->is_open()) {
        auto result = tun_->read_packet();
        if (!result) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto& pkt = result.value();
        if (pkt.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto ws_pkt = protocol::build_ip_packet(pkt);
        if (ws_ && ws_->is_connected()) {
            ws_->send(std::move(ws_pkt));
        }
    }

    LOG_INFO("TUN read loop ended");
}

void Client::setup_routes() {
    LOG_INFO("Configuring routes for full tunnel");

    std::string iface = tun_->interface_name();

    if (server_ip_.empty() || original_gateway_.empty()) {
        LOG_ERROR("Cannot setup routes: server IP or gateway unknown");
        return;
    }

    LOG_INFOF("Route exclusion: {} via gateway {}", server_ip_, original_gateway_);

#if defined(KIRDI_PLATFORM_LINUX)
    // 1. Exclude server IP from tunnel (MUST succeed before adding catch-all)
    std::string cmd = "ip route add " + server_ip_ + "/32 via " + original_gateway_;
    LOG_DEBUGF("Running: {}", cmd);
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        LOG_WARNF("Failed to add server exclude route (ret={}), trying with metric", ret);
        cmd = "ip route add " + server_ip_ + "/32 via " + original_gateway_ + " metric 0";
        std::system(cmd.c_str());
    }

    // 2. Add catch-all routes through TUN
    cmd = "ip route add 0.0.0.0/1 dev " + iface;
    LOG_DEBUGF("Running: {}", cmd);
    std::system(cmd.c_str());

    cmd = "ip route add 128.0.0.0/1 dev " + iface;
    LOG_DEBUGF("Running: {}", cmd);
    std::system(cmd.c_str());

    LOG_INFO("Routes configured via ip route");

#elif defined(KIRDI_PLATFORM_MACOS)
    // 1. Exclude server IP from tunnel — route to server via original gateway
    //    This MUST succeed, otherwise we get a routing loop!
    std::string cmd = "route -n add -host " + server_ip_ + " " + original_gateway_;
    LOG_DEBUGF("Running: {}", cmd);
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        LOG_ERRORF("CRITICAL: Failed to add server exclude route (ret={})", ret);
        LOG_ERROR("Aborting route setup to prevent routing loop");
        return;
    }

    // 2. Add catch-all routes through TUN
    cmd = "route -n add -net 0.0.0.0/1 -interface " + iface;
    LOG_DEBUGF("Running: {}", cmd);
    std::system(cmd.c_str());

    cmd = "route -n add -net 128.0.0.0/1 -interface " + iface;
    LOG_DEBUGF("Running: {}", cmd);
    std::system(cmd.c_str());

    LOG_INFO("Routes configured via route command");
#endif
}

void Client::teardown_routes() {
    if (!config_.auto_route) return;

    LOG_INFO("Removing tunnel routes");

#if defined(KIRDI_PLATFORM_LINUX)
    std::system("ip route del 0.0.0.0/1 2>/dev/null || true");
    std::system("ip route del 128.0.0.0/1 2>/dev/null || true");
    if (!server_ip_.empty()) {
        std::string cmd = "ip route del " + server_ip_ + "/32 2>/dev/null || true";
        std::system(cmd.c_str());
    }
#elif defined(KIRDI_PLATFORM_MACOS)
    std::system("route -n delete -net 0.0.0.0/1 2>/dev/null || true");
    std::system("route -n delete -net 128.0.0.0/1 2>/dev/null || true");
    if (!server_ip_.empty()) {
        std::string cmd = "route -n delete -host " + server_ip_ + " 2>/dev/null || true";
        std::system(cmd.c_str());
    }
#endif
}

} // namespace kirdi::client
