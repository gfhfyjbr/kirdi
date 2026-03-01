#pragma once

#include "common/config.hpp"
#include "common/protocol.hpp"
#include "transport/ws_transport.hpp"
#include "tun/tun_interface.hpp"
#include "server/session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>

#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace kirdi::server {

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
using tcp = net::ip::tcp;

// ── Kirdi Server ────────────────────────────────────────────────────────────
//
// Accepts WebSocket connections (behind nginx or standalone with TLS).
// Each client gets a virtual IP from the configured subnet.
// Server creates a TUN device and forwards IP packets between
// clients and the internet via NAT (iptables MASQUERADE on Linux).
//
// Data flow:
//   Client WS → parse protocol → extract IP packet → write to TUN
//   TUN → read IP packet → find session by dest IP → send via WS
// ─────────────────────────────────────────────────────────────────────────────

class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    // Start accepting connections (blocks until stopped)
    void run();

    // Stop the server gracefully
    void stop();

private:
    ServerConfig config_;
    net::io_context ioc_;
    ssl::context ssl_ctx_{ssl::context::tlsv12};
    tcp::acceptor acceptor_{ioc_};

    // TUN device for packet forwarding
    std::unique_ptr<tun::TunDevice> tun_;

    // Active sessions: session_id → session
    std::unordered_map<uint32_t, std::shared_ptr<ClientSession>> sessions_;
    // IP → session_id mapping for routing return packets
    std::unordered_map<uint32_t, uint32_t> ip_to_session_;
    std::mutex sessions_mutex_;
    std::atomic<uint32_t> next_session_id_{1};
    std::atomic<uint32_t> next_client_ip_offset_{2};  // .2, .3, .4, ...

    // Accept loop
    void do_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);

    // TUN read loop (runs in separate thread)
    void tun_read_loop();

    // Assign virtual IP to new client
    std::string allocate_client_ip();

    // Route a packet from TUN to the correct client session
    void route_to_client(const uint8_t* data, size_t len);

    // Setup TLS context (if running standalone)
    void setup_tls();
};

} // namespace kirdi::server
