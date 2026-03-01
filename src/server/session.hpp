#pragma once

#include "common/protocol.hpp"
#include "transport/ws_transport.hpp"

#include <string>
#include <cstdint>
#include <memory>
#include <functional>

namespace kirdi::server {

// ── Client Session ──────────────────────────────────────────────────────────
// Represents one connected client with its virtual IP and WS session.

using OnIpPacketCallback = std::function<void(uint32_t session_id, std::vector<uint8_t> packet)>;

class ClientSession {
public:
    ClientSession(uint32_t id, const std::string& virtual_ip,
                  std::shared_ptr<transport::IWsSession> ws);

    uint32_t id() const { return id_; }
    const std::string& virtual_ip() const { return virtual_ip_; }
    uint32_t virtual_ip_n() const { return virtual_ip_n_; }  // Network byte order

    bool is_authenticated() const { return authenticated_; }
    void set_authenticated(bool v) { authenticated_ = v; }

    // Send an IP packet to this client
    void send_ip_packet(const uint8_t* data, size_t len);

    // Set callback for when client sends an IP packet
    void on_ip_packet(OnIpPacketCallback cb) { on_ip_packet_ = std::move(cb); }

    // Start processing messages
    void start();

    // Close session
    void close();

private:
    uint32_t id_;
    std::string virtual_ip_;
    uint32_t virtual_ip_n_;  // Network byte order for fast lookup
    bool authenticated_ = false;
    std::shared_ptr<transport::IWsSession> ws_;
    OnIpPacketCallback on_ip_packet_;

    void handle_packet(const protocol::PacketHeader& hdr, std::vector<uint8_t> payload);
};

} // namespace kirdi::server
