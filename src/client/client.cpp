#include "client/client.hpp"
#include "common/logger.hpp"
#include "common/crypto.hpp"
#include <kirdi/version.hpp>

#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

namespace kirdi::client {

Client::Client(ClientConfig config) : config_(std::move(config)) {
}

Client::~Client() {
    stop();
}

void Client::run() {
    LOG_INFOF("kirdi client v{} starting", KIRDI_VERSION_STRING);
    running_ = true;

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
    LOG_INFO("Client stopping");
    running_ = false;

    if (ws_) ws_->close();
    ioc_.stop();

    teardown_routes();
    if (tun_) tun_->close();
}

void Client::on_connected() {
    LOG_INFO("Connected to server — sending auth");
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
            // Parse auth response
            try {
                auto j = nlohmann::json::parse(payload.begin(), payload.end());
                if (j.value("ok", false)) {
                    virtual_ip_ = j.value("tun_ip", "10.8.0.2");
                    LOG_INFOF("Authenticated! Virtual IP: {}", virtual_ip_);

                    // Create TUN device
                    tun_ = tun::create_tun_device();
                    tun::TunConfig tun_cfg{
                        .name = "kirdi0",
                        .address = virtual_ip_,
                        .netmask = "255.255.255.0",
                        .mtu = config_.mtu,
                    };

                    auto result = tun_->open(tun_cfg);
                    if (!result) {
                        LOG_ERROR("Failed to create TUN device");
                        stop();
                        return;
                    }

                    // Setup routes
                    if (config_.auto_route) {
                        setup_routes();
                    }

                    // Start TUN read loop in background
                    std::thread tun_thread([this]() { tun_read_loop(); });
                    tun_thread.detach();

                    LOG_INFO("Tunnel active — all traffic routed through server");
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
            // Write incoming IP packet to TUN
            if (tun_ && tun_->is_open()) {
                tun_->write_packet(payload.data(), payload.size());
            }
            break;

        case protocol::MsgType::Keepalive:
            // Send keepalive back
            ws_->send(protocol::build_keepalive());
            break;

        case protocol::MsgType::Pong:
            // Latency measurement — ignore for now
            break;

        default:
            LOG_WARNF("Unknown message type 0x{:02x}", static_cast<uint8_t>(hdr.type));
            break;
    }
}

void Client::on_error(const std::string& err) {
    LOG_ERRORF("Transport error: {}", err);
    // TODO: Implement reconnection logic with exponential backoff
}

void Client::tun_read_loop() {
    LOG_INFO("TUN read loop started");

    while (running_ && tun_ && tun_->is_open()) {
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

        // Wrap IP packet and send to server
        auto ws_pkt = protocol::build_ip_packet(pkt);
        if (ws_ && ws_->is_connected()) {
            ws_->send(std::move(ws_pkt));
        }
    }

    LOG_INFO("TUN read loop ended");
}

void Client::setup_routes() {
    LOG_INFO("Configuring routes for full tunnel");

    // Exclude server IP from tunnel (prevent routing loop)
    // Then route all traffic through TUN

    std::string iface = tun_->interface_name();

#if defined(KIRDI_PLATFORM_LINUX)
    // Get current default gateway
    // Route server IP through original gateway
    // Route 0.0.0.0/1 and 128.0.0.0/1 through TUN (covers all traffic without replacing default)
    std::string cmd;
    cmd = "ip route add " + config_.server_host + "/32 via $(ip route | grep default | awk '{print $3}') 2>/dev/null || true";
    std::system(cmd.c_str());

    cmd = "ip route add 0.0.0.0/1 dev " + iface;
    std::system(cmd.c_str());

    cmd = "ip route add 128.0.0.0/1 dev " + iface;
    std::system(cmd.c_str());

    LOG_INFO("Routes configured via ip route");

#elif defined(KIRDI_PLATFORM_MACOS)
    std::string cmd;
    // Route server through original gateway
    cmd = "route add -host " + config_.server_host + " $(route -n get default | grep gateway | awk '{print $2}') 2>/dev/null || true";
    std::system(cmd.c_str());

    cmd = "route add -net 0.0.0.0/1 -interface " + iface;
    std::system(cmd.c_str());

    cmd = "route add -net 128.0.0.0/1 -interface " + iface;
    std::system(cmd.c_str());

    LOG_INFO("Routes configured via route command");
#endif
}

void Client::teardown_routes() {
    if (!config_.auto_route || !tun_ || !tun_->is_open()) return;

    LOG_INFO("Removing tunnel routes");
    std::string iface = tun_->interface_name();

#if defined(KIRDI_PLATFORM_LINUX)
    std::system("ip route del 0.0.0.0/1 2>/dev/null || true");
    std::system("ip route del 128.0.0.0/1 2>/dev/null || true");
    std::string cmd = "ip route del " + config_.server_host + "/32 2>/dev/null || true";
    std::system(cmd.c_str());
#elif defined(KIRDI_PLATFORM_MACOS)
    std::system("route delete -net 0.0.0.0/1 2>/dev/null || true");
    std::system("route delete -net 128.0.0.0/1 2>/dev/null || true");
    std::string cmd = "route delete -host " + config_.server_host + " 2>/dev/null || true";
    std::system(cmd.c_str());
#endif
}

} // namespace kirdi::client
