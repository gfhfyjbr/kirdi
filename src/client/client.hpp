#pragma once

#include "common/config.hpp"
#include "common/protocol.hpp"
#include "transport/ws_transport.hpp"
#include "tun/tun_interface.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <atomic>
#include <thread>

namespace kirdi::client {

namespace net = boost::asio;
namespace ssl = net::ssl;

// ── Kirdi Client ────────────────────────────────────────────────────────────
//
// 1. Connects to server via WebSocket over TLS
// 2. Authenticates with HMAC token
// 3. Receives TUN configuration (virtual IP, subnet)
// 4. Creates local TUN device
// 5. Bidirectional packet forwarding:
//    - TUN → read IP packet → wrap in protocol → send via WS
//    - WS → receive protocol → extract IP packet → write to TUN
//
// All local traffic goes through TUN → server → internet.
// ─────────────────────────────────────────────────────────────────────────────

class Client {
public:
    explicit Client(ClientConfig config);
    ~Client();

    void run();
    void stop();

private:
    ClientConfig config_;
    net::io_context ioc_;
    ssl::context ssl_ctx_{ssl::context::tlsv12_client};

    std::shared_ptr<transport::WsClientTransport> ws_;
    std::unique_ptr<tun::TunDevice> tun_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::string virtual_ip_;
    std::string server_ip_;          // Resolved server IP (for route exclusion)
    std::string server_tun_ip_;      // Server's TUN IP (gateway for Windows routes)
    std::string original_gateway_;   // Original default gateway (saved before route change)
    uint32_t reconnect_attempt_{0};
    static constexpr uint32_t MAX_RECONNECT_DELAY_SEC = 60;

    // WebSocket callbacks
    void on_connected();
    void on_packet(const protocol::PacketHeader& hdr, std::vector<uint8_t> payload);
    void on_error(const std::string& err);

    // TUN read loop
    void tun_read_loop();

    // Auth
    void send_auth();

    // Configure routes (platform-specific)
    void setup_routes();
    void teardown_routes();

    // DNS management
    void setup_dns();
    void teardown_dns();

    // Reconnection
    void schedule_reconnect();
};

} // namespace kirdi::client
