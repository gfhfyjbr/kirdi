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
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    void run();
    void stop();

private:
    ServerConfig config_;
    net::io_context ioc_;
    ssl::context ssl_ctx_{ssl::context::tlsv12};
    tcp::acceptor acceptor_{ioc_};

    // TUN device
    std::unique_ptr<tun::TunDevice> tun_;

    // Active sessions
    std::unordered_map<uint32_t, std::shared_ptr<ClientSession>> sessions_;
    std::unordered_map<uint32_t, uint32_t> ip_to_session_;
    std::mutex sessions_mutex_;
    std::atomic<uint32_t> next_session_id_{1};
    std::atomic<uint32_t> next_client_ip_offset_{2};

    void do_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);

    // Register a new authenticated session (shared by SSL and plain paths)
    void register_session(uint32_t sid, const std::string& client_ip,
                          std::shared_ptr<transport::IWsSession> ws_session);

    void tun_read_loop();
    std::string allocate_client_ip();
    void route_to_client(const uint8_t* data, size_t len);
    void setup_tls();
    void ensure_system_config();
};

} // namespace kirdi::server
