#pragma once

#include "common/protocol.hpp"
#include "transport/ws_transport.hpp"

#include <string>
#include <cstdint>
#include <memory>
#include <functional>

namespace kirdi::server {

using OnIpPacketCallback = std::function<void(uint32_t session_id, std::vector<uint8_t> packet)>;

// TUN config to send to client in auth response
struct TunInfo {
    std::string server_ip;   // Server TUN IP (e.g. "10.8.0.1")
    std::string mask;        // Netmask (e.g. "255.255.255.0")
    uint32_t    mtu = 1400;
};

class ClientSession {
public:
    ClientSession(uint32_t id, const std::string& virtual_ip,
                  std::shared_ptr<transport::IWsSession> ws);

    uint32_t id() const { return id_; }
    const std::string& virtual_ip() const { return virtual_ip_; }
    uint32_t virtual_ip_n() const { return virtual_ip_n_; }

    bool is_authenticated() const { return authenticated_; }
    void set_authenticated(bool v) { authenticated_ = v; }

    // Set server TUN info (sent to client in auth response)
    void set_tun_info(TunInfo info) { tun_info_ = std::move(info); }
    void set_auth_secret(std::string secret) { auth_secret_ = std::move(secret); }

    void send_ip_packet(const uint8_t* data, size_t len);
    void on_ip_packet(OnIpPacketCallback cb) { on_ip_packet_ = std::move(cb); }
    void start();
    void close();

private:
    uint32_t id_;
    std::string virtual_ip_;
    uint32_t virtual_ip_n_;
    bool authenticated_ = false;
    std::shared_ptr<transport::IWsSession> ws_;
    OnIpPacketCallback on_ip_packet_;
    TunInfo tun_info_;
    std::string auth_secret_;

    void handle_packet(const protocol::PacketHeader& hdr, std::vector<uint8_t> payload);
};

} // namespace kirdi::server
