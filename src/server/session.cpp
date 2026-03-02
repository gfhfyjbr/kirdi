#include "server/session.hpp"
#include "common/logger.hpp"
#include "common/crypto.hpp"

#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <chrono>

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
        case protocol::MsgType::AuthRequest: {
            LOG_INFOF("Session {} auth request (len={})", id_, payload.size());

            bool auth_ok = false;
            try {
                auto j = nlohmann::json::parse(payload.begin(), payload.end());
                std::string user = j.value("user", "");
                std::string token = j.value("token", "");
                uint64_t timestamp = j.value("timestamp", uint64_t(0));

                if (!auth_secret_.empty() && !token.empty()) {
                    auth_ok = crypto::verify_auth_token(user, auth_secret_, token, timestamp, 30);
                    if (!auth_ok) {
                        // also try current server time in case client clock is off
                        auto now = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()
                            ).count()
                        );
                        auth_ok = crypto::verify_auth_token(user, auth_secret_, token, now, 30);
                    }
                } else if (auth_secret_.empty()) {
                    // No secret configured — accept anyone (dev mode)
                    LOG_WARN("No auth_secret configured — accepting without validation");
                    auth_ok = true;
                }

                if (auth_ok) {
                    LOG_INFOF("Session {} auth OK (user={})", id_, user);
                } else {
                    LOG_WARNF("Session {} auth FAILED (user={}, ts={})", id_, user, timestamp);
                }
            } catch (const std::exception& e) {
                LOG_WARNF("Session {} auth parse error: {}", id_, e.what());
            }

            authenticated_ = auth_ok;

            // Send auth response
            nlohmann::json resp;
            if (auth_ok) {
                resp = {
                    {"ok", true},
                    {"tun_ip", virtual_ip_},
                    {"tun_server_ip", tun_info_.server_ip},
                    {"tun_mask", tun_info_.mask},
                    {"mtu", tun_info_.mtu},
                };
            } else {
                resp = {{"ok", false}, {"error", "authentication failed"}};
            }
            std::string resp_str = resp.dump();
            LOG_INFOF("Session {} auth response: {}", id_, resp_str);
            auto pkt = protocol::build_packet(
                protocol::MsgType::AuthResponse,
                {reinterpret_cast<const uint8_t*>(resp_str.data()), resp_str.size()}
            );
            ws_->send(std::move(pkt));

            if (!auth_ok) {
                // Close connection after sending rejection
                close();
            }
            break;
        }

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
