#include "transport/ws_transport.hpp"
#include <boost/beast/core/buffers_to_string.hpp>

namespace kirdi::transport {

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

    // Set TCP timeout
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));

    beast::get_lowest_layer(*ws_).async_connect(
        results,
        beast::bind_front_handler(&WsClientTransport::on_tcp_connect, shared_from_this())
    );
}

void WsClientTransport::on_tcp_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
    if (ec) return fail(ec, "tcp_connect");

    // Set SNI hostname for TLS
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

    // Set WebSocket options
    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_->set_option(websocket::stream_base::decorator(
        [this](websocket::request_type& req) {
            req.set(boost::beast::http::field::user_agent, "Mozilla/5.0");
            req.set(boost::beast::http::field::host, host_);
        }
    ));

    // Binary mode for IP packets
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
    std::lock_guard<std::mutex> lock(write_mutex_);
    write_queue_.push(std::move(data));
    if (!writing_) {
        writing_ = true;
        net::post(ws_->get_executor(), [self = shared_from_this()]() { self->do_write(); });
    }
}

void WsClientTransport::do_read() {
    ws_->async_read(
        read_buf_,
        beast::bind_front_handler(&WsClientTransport::on_read, shared_from_this())
    );
}

void WsClientTransport::on_read(beast::error_code ec, std::size_t bytes) {
    if (ec) {
        connected_ = false;
        return fail(ec, "read");
    }

    // Parse protocol header + payload from the binary frame
    auto data = read_buf_.data();
    auto buf_data = static_cast<const uint8_t*>(data.data());
    size_t buf_size = data.size();

    if (buf_size >= protocol::HEADER_SIZE) {
        auto hdr = protocol::deserialize_header({buf_data, buf_size});
        if (hdr && buf_size >= protocol::HEADER_SIZE + hdr->length) {
            std::vector<uint8_t> payload(
                buf_data + protocol::HEADER_SIZE,
                buf_data + protocol::HEADER_SIZE + hdr->length
            );
            if (on_packet_) on_packet_(*hdr, std::move(payload));
        }
    }

    read_buf_.consume(read_buf_.size());
    do_read();
}

void WsClientTransport::do_write() {
    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        data = std::move(write_queue_.front());
        write_queue_.pop();
    }

    ws_->async_write(
        net::buffer(data),
        beast::bind_front_handler(&WsClientTransport::on_write, shared_from_this())
    );
}

void WsClientTransport::on_write(beast::error_code ec, std::size_t bytes) {
    if (ec) return fail(ec, "write");
    do_write();
}

void WsClientTransport::close() {
    if (connected_) {
        connected_ = false;
        beast::error_code ec;
        ws_->close(websocket::close_code::normal, ec);
    }
}

void WsClientTransport::fail(beast::error_code ec, const char* what) {
    LOG_ERRORF("WS client {}: {}", what, ec.message());
    connected_ = false;
    if (on_error_) on_error_(std::string(what) + ": " + ec.message());
}

// ══════════════════════════════════════════════════════════════════════════════
// WsServerSession
// ══════════════════════════════════════════════════════════════════════════════

WsServerSession::WsServerSession(ws_stream&& ws, uint32_t session_id)
    : ws_(std::move(ws)), session_id_(session_id)
{
}

void WsServerSession::start() {
    ws_.binary(true);
    connected_ = true;
    LOG_INFOF("Session {} started", session_id_);
    do_read();
}

void WsServerSession::send(std::vector<uint8_t> data) {
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

    auto data = read_buf_.data();
    auto buf_data = static_cast<const uint8_t*>(data.data());
    size_t buf_size = data.size();

    if (buf_size >= protocol::HEADER_SIZE) {
        auto hdr = protocol::deserialize_header({buf_data, buf_size});
        if (hdr && buf_size >= protocol::HEADER_SIZE + hdr->length) {
            std::vector<uint8_t> payload(
                buf_data + protocol::HEADER_SIZE,
                buf_data + protocol::HEADER_SIZE + hdr->length
            );
            if (on_packet_) on_packet_(*hdr, std::move(payload));
        }
    }

    read_buf_.consume(read_buf_.size());
    do_read();
}

void WsServerSession::do_write() {
    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        data = std::move(write_queue_.front());
        write_queue_.pop();
    }

    ws_.async_write(
        net::buffer(data),
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
    if (connected_) {
        connected_ = false;
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
    }
}

} // namespace kirdi::transport
