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
#include <atomic>

namespace kirdi::transport {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// Callback types
using OnPacketCallback = std::function<void(const protocol::PacketHeader&, std::vector<uint8_t>)>;
using OnErrorCallback  = std::function<void(const std::string&)>;
using OnConnectCallback = std::function<void()>;

// ── Abstract server-side session interface ──────────────────────────────────
// Both SSL and plain WS sessions implement this.

class IWsSession {
public:
    virtual ~IWsSession() = default;
    virtual void start() = 0;
    virtual void send(std::vector<uint8_t> data) = 0;
    virtual void close() = 0;
    virtual bool is_connected() const = 0;
    virtual uint32_t session_id() const = 0;
    virtual void on_packet(OnPacketCallback cb) = 0;
    virtual void on_error(OnErrorCallback cb) = 0;
};

// ── Client-side WebSocket Transport ─────────────────────────────────────────

class WsClientTransport : public std::enable_shared_from_this<WsClientTransport> {
public:
    WsClientTransport(net::io_context& ioc, ssl::context& ssl_ctx);

    void connect(const std::string& host, uint16_t port, const std::string& path);
    void send(std::vector<uint8_t> data);
    void close();

    void on_packet(OnPacketCallback cb)   { on_packet_ = std::move(cb); }
    void on_error(OnErrorCallback cb)     { on_error_ = std::move(cb); }
    void on_connect(OnConnectCallback cb) { on_connect_ = std::move(cb); }

    bool is_connected() const { return connected_.load(); }

private:
    using ws_stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    net::io_context& ioc_;
    ssl::context& ssl_ctx_;
    std::unique_ptr<ws_stream> ws_;
    tcp::resolver resolver_;

    std::string host_;
    uint16_t port_ = 443;
    std::string path_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> closing_{false};

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

// ── Server-side SSL WebSocket Session ───────────────────────────────────────

class WsServerSession : public IWsSession, public std::enable_shared_from_this<WsServerSession> {
public:
    using ws_stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    WsServerSession(ws_stream&& ws, uint32_t session_id);

    void start() override;
    void send(std::vector<uint8_t> data) override;
    void close() override;
    bool is_connected() const override { return connected_.load(); }
    uint32_t session_id() const override { return session_id_; }
    void on_packet(OnPacketCallback cb) override { on_packet_ = std::move(cb); }
    void on_error(OnErrorCallback cb) override   { on_error_ = std::move(cb); }

private:
    ws_stream ws_;
    uint32_t session_id_;
    std::atomic<bool> connected_{false};

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

// ── Server-side Plain WebSocket Session (behind nginx, no SSL) ──────────────

class WsPlainServerSession : public IWsSession, public std::enable_shared_from_this<WsPlainServerSession> {
public:
    using ws_stream = websocket::stream<beast::tcp_stream>;

    WsPlainServerSession(ws_stream&& ws, uint32_t session_id);

    void start() override;
    void send(std::vector<uint8_t> data) override;
    void close() override;
    bool is_connected() const override { return connected_.load(); }
    uint32_t session_id() const override { return session_id_; }
    void on_packet(OnPacketCallback cb) override { on_packet_ = std::move(cb); }
    void on_error(OnErrorCallback cb) override   { on_error_ = std::move(cb); }

private:
    ws_stream ws_;
    uint32_t session_id_;
    std::atomic<bool> connected_{false};

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
