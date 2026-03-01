#pragma once

#include "common/protocol.hpp"
#include "common/logger.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <string>
#include <functional>
#include <memory>
#include <queue>
#include <mutex>

namespace kirdi::transport {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// ── WebSocket Transport ─────────────────────────────────────────────────────
//
// Handles WebSocket + TLS connection (client or server side).
// Sends/receives binary frames containing protocol::Packet data.
//
// Built on Boost.Beast for WebSocket and Boost.Asio for async I/O.
// ─────────────────────────────────────────────────────────────────────────────

// Callback types
using OnPacketCallback = std::function<void(const protocol::PacketHeader&, std::vector<uint8_t>)>;
using OnErrorCallback  = std::function<void(const std::string&)>;
using OnConnectCallback = std::function<void()>;

// ── Client-side WebSocket Transport ─────────────────────────────────────────

class WsClientTransport : public std::enable_shared_from_this<WsClientTransport> {
public:
    WsClientTransport(net::io_context& ioc, ssl::context& ssl_ctx);

    // Connect to server
    void connect(const std::string& host, uint16_t port, const std::string& path);

    // Send a binary frame (protocol packet)
    void send(std::vector<uint8_t> data);

    // Close the connection
    void close();

    // Callbacks
    void on_packet(OnPacketCallback cb)   { on_packet_ = std::move(cb); }
    void on_error(OnErrorCallback cb)     { on_error_ = std::move(cb); }
    void on_connect(OnConnectCallback cb) { on_connect_ = std::move(cb); }

    bool is_connected() const { return connected_; }

private:
    using ws_stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    net::io_context& ioc_;
    ssl::context& ssl_ctx_;
    std::unique_ptr<ws_stream> ws_;
    tcp::resolver resolver_;

    std::string host_;
    uint16_t port_ = 443;
    std::string path_;
    bool connected_ = false;

    beast::flat_buffer read_buf_;
    std::queue<std::vector<uint8_t>> write_queue_;
    std::mutex write_mutex_;
    bool writing_ = false;

    OnPacketCallback  on_packet_;
    OnErrorCallback   on_error_;
    OnConnectCallback on_connect_;

    void do_resolve();
    void on_resolved(beast::error_code ec, tcp::resolver::results_type results);
    void on_tcp_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_ws_handshake(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes);
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes);
    void fail(beast::error_code ec, const char* what);
};

// ── Server-side WebSocket Session ───────────────────────────────────────────
// One session per connected client.

class WsServerSession : public std::enable_shared_from_this<WsServerSession> {
public:
    using ws_stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    WsServerSession(ws_stream&& ws, uint32_t session_id);

    void start();
    void send(std::vector<uint8_t> data);
    void close();

    uint32_t session_id() const { return session_id_; }
    bool is_connected() const { return connected_; }

    void on_packet(OnPacketCallback cb)  { on_packet_ = std::move(cb); }
    void on_error(OnErrorCallback cb)    { on_error_ = std::move(cb); }

private:
    ws_stream ws_;
    uint32_t session_id_;
    bool connected_ = false;

    beast::flat_buffer read_buf_;
    std::queue<std::vector<uint8_t>> write_queue_;
    std::mutex write_mutex_;
    bool writing_ = false;

    OnPacketCallback on_packet_;
    OnErrorCallback  on_error_;

    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes);
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes);
};

} // namespace kirdi::transport
