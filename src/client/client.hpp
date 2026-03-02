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
#include <functional>
#include <mutex>

namespace kirdi::client {

namespace net = boost::asio;
namespace ssl = net::ssl;

// ── GUI Status Callback ─────────────────────────────────────────────────────
// Called from I/O thread whenever status or stats change.
// `event` is one of: "connecting", "connected", "disconnected", "error", "reconnecting"
// `json_data` is a JSON string with additional info (e.g. IPs, error message, attempt)
using StatusCallback = std::function<void(const std::string& event, const std::string& json_data)>;

// ── Kirdi Client ────────────────────────────────────────────────────────────
class Client {
public:
    explicit Client(ClientConfig config);
    ~Client();

    void run();
    void stop();

    // GUI integration: set callback for status/stats updates (thread-safe)
    void set_status_callback(StatusCallback cb);

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
    std::string original_gateway_;   // Original default gateway (saved before route change)
    uint32_t reconnect_attempt_{0};
    static constexpr uint32_t MAX_RECONNECT_DELAY_SEC = 60;

    // GUI callback
    StatusCallback status_cb_;
    std::mutex status_cb_mutex_;
    void notify_status(const std::string& event, const std::string& json_data = "{}");

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
