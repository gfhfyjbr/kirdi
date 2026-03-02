#include "server/server.hpp"
#include "common/logger.hpp"
#include <kirdi/version.hpp>

#include <boost/beast/http.hpp>

#include <thread>
#include <arpa/inet.h>

namespace kirdi::server {

Server::Server(ServerConfig config) : config_(std::move(config)) {
}

Server::~Server() {
    stop();
}

void Server::setup_tls() {
    if (config_.tls_enabled) {
        ssl_ctx_.set_options(
            ssl::context::default_workarounds |
            ssl::context::no_sslv2 |
            ssl::context::no_sslv3
        );
        ssl_ctx_.use_certificate_chain_file(config_.tls_cert_path);
        ssl_ctx_.use_private_key_file(config_.tls_key_path, ssl::context::pem);
        LOG_INFO("TLS configured with certificate and key");
    }
}

std::string Server::allocate_client_ip() {
    uint32_t offset = next_client_ip_offset_.fetch_add(1);

    struct in_addr base{};
    inet_pton(AF_INET, config_.tun_subnet.c_str(), &base);
    uint32_t ip_host = ntohl(base.s_addr) + offset;
    struct in_addr client_addr{};
    client_addr.s_addr = htonl(ip_host);

    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr, buf, sizeof(buf));
    return std::string(buf);
}

void Server::run() {
    LOG_INFOF("kirdi server v{} starting", KIRDI_VERSION_STRING);
    LOG_INFOF("Config: listen={}:{} tls={} tun_subnet={} tun_ip={} tun_mask={} ws_path={}",
              config_.listen_addr, config_.listen_port, config_.tls_enabled,
              config_.tun_subnet, config_.tun_server_ip, config_.tun_mask, config_.ws_path);

    setup_tls();

    // Create and configure TUN device
    tun_ = tun::create_tun_device();
    tun::TunConfig tun_cfg{
        .name = "kirdi0",
        .address = config_.tun_server_ip,
        .netmask = config_.tun_mask,
        .mtu = config_.mtu,
    };
    auto tun_result = tun_->open(tun_cfg);
    if (!tun_result) {
        LOG_ERROR("Failed to create TUN device — aborting");
        return;
    }

    // Start TUN read loop in background thread
    std::thread tun_thread([this]() { tun_read_loop(); });
    tun_thread.detach();

    // Setup TCP acceptor
    auto ep = tcp::endpoint(net::ip::make_address(config_.listen_addr), config_.listen_port);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen(net::socket_base::max_listen_connections);

    LOG_INFOF("Listening on {}:{} (tls={})", config_.listen_addr, config_.listen_port,
              config_.tls_enabled ? "on" : "off");

    do_accept();

    // Run I/O context
    auto threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> io_threads;
    for (unsigned i = 1; i < threads; ++i) {
        io_threads.emplace_back([this]() { ioc_.run(); });
    }
    ioc_.run();
    for (auto& t : io_threads) t.join();
}

void Server::stop() {
    LOG_INFO("Server stopping");
    ioc_.stop();
    if (tun_) tun_->close();
}

void Server::do_accept() {
    acceptor_.async_accept(
        net::make_strand(ioc_),
        beast::bind_front_handler(&Server::on_accept, this)
    );
}

void Server::on_accept(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        LOG_ERRORF("Accept error: {}", ec.message());
        do_accept();
        return;
    }

    uint32_t sid = next_session_id_.fetch_add(1);
    std::string client_ip = allocate_client_ip();

    LOG_INFOF("New connection from {} -> session {} (vip={})",
              socket.remote_endpoint().address().to_string(), sid, client_ip);

    if (config_.tls_enabled) {
        // ── Standalone mode: do SSL handshake, then WS accept ──────────────
        auto ssl_stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(
            std::move(socket), ssl_ctx_
        );

        ssl_stream->async_handshake(
            ssl::stream_base::server,
            [this, sid, client_ip, ssl_stream = std::move(ssl_stream)]
            (beast::error_code ec) mutable {
                if (ec) {
                    LOG_WARNF("Session {} SSL handshake failed: {}", sid, ec.message());
                    return;
                }

                auto ws = transport::WsServerSession::ws_stream(std::move(*ssl_stream));

                ws.set_option(boost::beast::websocket::stream_base::timeout::suggested(
                    beast::role_type::server
                ));
                ws.binary(true);

                ws.async_accept(
                    [this, sid, client_ip,
                     ws = std::make_shared<transport::WsServerSession::ws_stream>(std::move(ws))]
                    (beast::error_code ec) mutable {
                        if (ec) {
                            LOG_WARNF("Session {} WS accept failed: {}", sid, ec.message());
                            return;
                        }
                        auto session = std::make_shared<transport::WsServerSession>(std::move(*ws), sid);
                        register_session(sid, client_ip, session);
                    }
                );
            }
        );
    } else {
        // ── Behind nginx: plain WebSocket (no SSL) ─────────────────────────
        auto ws = std::make_shared<transport::WsPlainServerSession::ws_stream>(
            std::move(socket)
        );

        ws->set_option(boost::beast::websocket::stream_base::timeout::suggested(
            beast::role_type::server
        ));
        ws->binary(true);

        // Accept the WebSocket handshake, validating the path
        ws->set_option(websocket::stream_base::decorator(
            [this](websocket::response_type& res) {
                res.set(boost::beast::http::field::server, "kirdi");
            }
        ));

        ws->async_accept(
            [this, sid, client_ip, ws](beast::error_code ec) mutable {
                if (ec) {
                    LOG_WARNF("Session {} WS accept failed: {}", sid, ec.message());
                    return;
                }
                auto session = std::make_shared<transport::WsPlainServerSession>(std::move(*ws), sid);
                register_session(sid, client_ip, session);
            }
        );
    }

    do_accept();
}

void Server::register_session(uint32_t sid, const std::string& client_ip,
                               std::shared_ptr<transport::IWsSession> ws_session) {
    auto session = std::make_shared<ClientSession>(sid, client_ip, ws_session);
    session->set_tun_info({
        .server_ip = config_.tun_server_ip,
        .mask = config_.tun_mask,
        .mtu = config_.mtu,
    });

    session->on_ip_packet([this](uint32_t id, std::vector<uint8_t> pkt) {
        if (tun_ && tun_->is_open()) {
            tun_->write_packet(pkt.data(), pkt.size());
        }
    });

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[sid] = session;

        struct in_addr addr{};
        inet_pton(AF_INET, client_ip.c_str(), &addr);
        ip_to_session_[addr.s_addr] = sid;
    }

    session->start();
    LOG_INFOF("Session {} registered with vip={}", sid, client_ip);
}

void Server::tun_read_loop() {
    LOG_INFO("TUN read loop started");

    while (tun_ && tun_->is_open()) {
        auto result = tun_->read_packet();
        if (!result) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto& pkt = result.value();
        if (pkt.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        route_to_client(pkt.data(), pkt.size());
    }

    LOG_INFO("TUN read loop ended");
}

void Server::route_to_client(const uint8_t* data, size_t len) {
    if (len < 20) return;

    uint32_t dst_ip = protocol::ip4_dst({data, len});

    // Debug: log dest IP in dotted notation
    struct in_addr da{};
    da.s_addr = dst_ip;
    char dst_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &da, dst_str, sizeof(dst_str));
    LOG_DEBUGF("TUN->client: {} bytes dst={}", len, dst_str);

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = ip_to_session_.find(dst_ip);
    if (it != ip_to_session_.end()) {
        auto sit = sessions_.find(it->second);
        if (sit != sessions_.end() && sit->second->is_authenticated()) {
            sit->second->send_ip_packet(data, len);
        }
    } else {
        LOG_DEBUGF("TUN->client: no session for dst={}", dst_str);
    }
}

} // namespace kirdi::server
