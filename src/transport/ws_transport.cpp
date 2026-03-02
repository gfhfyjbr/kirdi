#include "transport/ws_transport.hpp"
#include <boost/beast/core/buffers_to_string.hpp>

namespace kirdi::transport {

// Helper: parse protocol frame from a read buffer
static void dispatch_packet(const beast::flat_buffer& buf, const OnPacketCallback& cb) {
    if (!cb) return;
    auto data = buf.data();
    auto buf_data = static_cast<const uint8_t*>(data.data());
    size_t buf_size = data.size();

    if (buf_size >= protocol::HEADER_SIZE) {
        auto hdr = protocol::deserialize_header({buf_data, buf_size});
        if (hdr && buf_size >= protocol::HEADER_SIZE + hdr->length) {
            std::vector<uint8_t> payload(
                buf_data + protocol::HEADER_SIZE,
                buf_data + protocol::HEADER_SIZE + hdr->length
            );
            cb(*hdr, std::move(payload));
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// WsClientTransport
// ══════════════════════════════════════════════════════════════════════════════

WsClientTransport::WsClientTransport(net::io_context& ioc, ssl::context& ssl_ctx)
    : ioc_(ioc), ssl_ctx_(ssl_ctx), resolver_(net::make_strand(ioc))
{
}

void WsClientTransport::connect(const std::string& host, uint16_t port, const std::string& path) {
    host_ = host;
    port_ = port;
    path_ = path;

    LOG_INFOF("Connecting to wss://{}:{}{}", host_, port_, path_);
    do_resolve();
}

void WsClientTransport::do_resolve() {
    resolver_.async_resolve(
        host_, std::to_string(port_),
        beast::bind_front_handler(&WsClientTransport::on_resolved, shared_from_this())
    );
}

void WsClientTransport::on_resolved(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) return fail(ec, "resolve");

    ws_ = std::make_unique<ws_stream>(net::make_strand(ioc_), ssl_ctx_);

    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));

    beast::get_lowest_layer(*ws_).async_connect(
        results,
        beast::bind_front_handler(&WsClientTransport::on_tcp_connect, shared_from_this())
    );
}

void WsClientTransport::on_tcp_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
    if (ec) return fail(ec, "tcp_connect");

    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
        return fail(beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()), "ssl_sni");
    }

    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));

    ws_->next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&WsClientTransport::on_ssl_handshake, shared_from_this())
    );
}

void WsClientTransport::on_ssl_handshake(beast::error_code ec) {
    if (ec) return fail(ec, "ssl_handshake");

    beast::get_lowest_layer(*ws_).expires_never();

    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_->set_option(websocket::stream_base::decorator(
        [this](websocket::request_type& req) {
            req.set(boost::beast::http::field::user_agent, "Mozilla/5.0");
            req.set(boost::beast::http::field::host, host_);
        }
    ));

    ws_->binary(true);

    ws_->async_handshake(
        host_ + ":" + std::to_string(port_), path_,
        beast::bind_front_handler(&WsClientTransport::on_ws_handshake, shared_from_this())
    );
}

void WsClientTransport::on_ws_handshake(beast::error_code ec) {
    if (ec) return fail(ec, "ws_handshake");

    connected_ = true;
    LOG_INFO("WebSocket connected");

    if (on_connect_) on_connect_();
    do_read();
}

void WsClientTransport::send(std::vector<uint8_t> data) {
    if (closing_.load()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    write_queue_.push(std::move(data));
    if (!writing_) {
        writing_ = true;
        net::post(ws_->get_executor(), [self = shared_from_this()]() { self->do_write(); });
    }
}

void WsClientTransport::do_read() {
    if (closing_.load()) return;

    ws_->async_read(
        read_buf_,
        beast::bind_front_handler(&WsClientTransport::on_read, shared_from_this())
    );
}

void WsClientTransport::on_read(beast::error_code ec, std::size_t bytes) {
    if (ec) {
        connected_ = false;
        if (!closing_.load()) fail(ec, "read");
        return;
    }

    dispatch_packet(read_buf_, on_packet_);
    read_buf_.consume(read_buf_.size());
    do_read();
}

void WsClientTransport::do_write() {
    if (closing_.load()) return;

    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        write_pending_ = std::move(write_queue_.front());
        write_queue_.pop();
    }

    ws_->async_write(
        net::buffer(write_pending_),
        beast::bind_front_handler(&WsClientTransport::on_write, shared_from_this())
    );
}

void WsClientTransport::on_write(beast::error_code ec, std::size_t bytes) {
    if (ec) {
        if (!closing_.load()) fail(ec, "write");
        return;
    }
    do_write();
}

void WsClientTransport::close() {
    if (closing_.exchange(true)) return;  // already closing
    connected_ = false;

    if (ws_) {
        try {
            beast::error_code ec;
            ws_->close(websocket::close_code::normal, ec);
        } catch (...) {}
    }
}

void WsClientTransport::fail(beast::error_code ec, const char* what) {
    if (closing_.load()) return;
    LOG_ERRORF("WS client {}: {}", what, ec.message());
    connected_ = false;
    if (on_error_) on_error_(std::string(what) + ": " + ec.message());
}

// ══════════════════════════════════════════════════════════════════════════════
// WsServerSession (SSL)
// ══════════════════════════════════════════════════════════════════════════════

WsServerSession::WsServerSession(ws_stream&& ws, uint32_t session_id)
    : ws_(std::move(ws)), session_id_(session_id)
{
}

void WsServerSession::start() {
    ws_.binary(true);
    connected_ = true;
    LOG_INFOF("Session {} started (SSL)", session_id_);
    do_read();
}

void WsServerSession::send(std::vector<uint8_t> data) {
    if (!connected_.load()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    write_queue_.push(std::move(data));
    if (!writing_) {
        writing_ = true;
        net::post(ws_.get_executor(), [self = shared_from_this()]() { self->do_write(); });
    }
}

void WsServerSession::do_read() {
    ws_.async_read(
        read_buf_,
        beast::bind_front_handler(&WsServerSession::on_read, shared_from_this())
    );
}

void WsServerSession::on_read(beast::error_code ec, std::size_t bytes) {
    if (ec) {
        connected_ = false;
        if (ec != websocket::error::closed) {
            LOG_WARNF("Session {} read error: {}", session_id_, ec.message());
        }
        if (on_error_) on_error_("read: " + ec.message());
        return;
    }

    dispatch_packet(read_buf_, on_packet_);
    read_buf_.consume(read_buf_.size());
    do_read();
}

void WsServerSession::do_write() {
    if (!connected_.load()) return;

    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        write_pending_ = std::move(write_queue_.front());
        write_queue_.pop();
    }

    ws_.async_write(
        net::buffer(write_pending_),
        beast::bind_front_handler(&WsServerSession::on_write, shared_from_this())
    );
}

void WsServerSession::on_write(beast::error_code ec, std::size_t bytes) {
    if (ec) {
        connected_ = false;
        LOG_WARNF("Session {} write error: {}", session_id_, ec.message());
        return;
    }
    do_write();
}

void WsServerSession::close() {
    bool expected = true;
    if (!connected_.compare_exchange_strong(expected, false)) return;

    try {
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
    } catch (...) {}
}

// ══════════════════════════════════════════════════════════════════════════════
// WsPlainServerSession (no SSL — behind nginx)
// ══════════════════════════════════════════════════════════════════════════════

WsPlainServerSession::WsPlainServerSession(ws_stream&& ws, uint32_t session_id)
    : ws_(std::move(ws)), session_id_(session_id)
{
}

void WsPlainServerSession::start() {
    ws_.binary(true);
    connected_ = true;
    LOG_INFOF("Session {} started (plain)", session_id_);
    do_read();
}

void WsPlainServerSession::send(std::vector<uint8_t> data) {
    if (!connected_.load()) return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    write_queue_.push(std::move(data));
    if (!writing_) {
        writing_ = true;
        net::post(ws_.get_executor(), [self = shared_from_this()]() { self->do_write(); });
    }
}

void WsPlainServerSession::do_read() {
    ws_.async_read(
        read_buf_,
        beast::bind_front_handler(&WsPlainServerSession::on_read, shared_from_this())
    );
}

void WsPlainServerSession::on_read(beast::error_code ec, std::size_t bytes) {
    if (ec) {
        connected_ = false;
        if (ec != websocket::error::closed) {
            LOG_WARNF("Session {} read error: {}", session_id_, ec.message());
        }
        if (on_error_) on_error_("read: " + ec.message());
        return;
    }

    dispatch_packet(read_buf_, on_packet_);
    read_buf_.consume(read_buf_.size());
    do_read();
}

void WsPlainServerSession::do_write() {
    if (!connected_.load()) return;

    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        write_pending_ = std::move(write_queue_.front());
        write_queue_.pop();
    }

    ws_.async_write(
        net::buffer(write_pending_),
        beast::bind_front_handler(&WsPlainServerSession::on_write, shared_from_this())
    );
}

void WsPlainServerSession::on_write(beast::error_code ec, std::size_t bytes) {
    if (ec) {
        connected_ = false;
        LOG_WARNF("Session {} write error: {}", session_id_, ec.message());
        return;
    }
    do_write();
}

void WsPlainServerSession::close() {
    bool expected = true;
    if (!connected_.compare_exchange_strong(expected, false)) return;

    try {
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
    } catch (...) {}
}

} // namespace kirdi::transport
