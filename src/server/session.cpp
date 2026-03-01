#include "server/session.hpp"
#include "common/logger.hpp"

#include <arpa/inet.h>

namespace kirdi::server {

ClientSession::ClientSession(uint32_t id, const std::string& virtual_ip,
                             std::shared_ptr<transport::IWsSession> ws)
    : id_(id), virtual_ip_(virtual_ip), ws_(std::move(ws))
{
    // Convert string IP to network byte order uint32_t
    inet_pton(AF_INET, virtual_ip_.c_str(), &virtual_ip_n_);
}

void ClientSession::start() {
    ws_->on_packet([this](const protocol::PacketHeader& hdr, std::vector<uint8_t> payload) {
        handle_packet(hdr, std::move(payload));
    });

    ws_->on_error([this](const std::string& err) {
        LOG_WARNF("Session {} error: {}", id_, err);
    });

    ws_->start();
}

void ClientSession::handle_packet(const protocol::PacketHeader& hdr, std::vector<uint8_t> payload) {
    switch (hdr.type) {
        case protocol::MsgType::AuthRequest:
            // TODO: Validate HMAC token, set authenticated_ = true
            LOG_INFOF("Session {} auth request (len={})", id_, payload.size());
            authenticated_ = true;  // Placeholder — implement proper auth
            {
                // Send auth response with TUN config
                std::string resp = R"({"ok":true,"tun_ip":")" + virtual_ip_ + R"("})";
                auto pkt = protocol::build_packet(
                    protocol::MsgType::AuthResponse,
                    {reinterpret_cast<const uint8_t*>(resp.data()), resp.size()}
                );
                ws_->send(std::move(pkt));
            }
            break;

        case protocol::MsgType::IpPacket:
            if (!authenticated_) {
                LOG_WARNF("Session {} sent IP packet before auth -- dropping", id_);
                return;
            }
            LOG_DEBUGF("Session {} rx IP packet: {} bytes", id_, payload.size());
            if (on_ip_packet_) {
                on_ip_packet_(id_, std::move(payload));
            }
            break;

        case protocol::MsgType::Keepalive:
            // Echo keepalive back
            ws_->send(protocol::build_keepalive());
            break;

        case protocol::MsgType::Ping: {
            auto pong = protocol::build_packet(protocol::MsgType::Pong, payload);
            ws_->send(std::move(pong));
            break;
        }

        case protocol::MsgType::Disconnect:
            LOG_INFOF("Session {} requested disconnect", id_);
            close();
            break;

        default:
            LOG_WARNF("Session {} unknown message type 0x{:02x}", id_, static_cast<uint8_t>(hdr.type));
            break;
    }
}

void ClientSession::send_ip_packet(const uint8_t* data, size_t len) {
    auto pkt = protocol::build_ip_packet({data, len});
    ws_->send(std::move(pkt));
}

void ClientSession::close() {
    ws_->close();
}

} // namespace kirdi::server
