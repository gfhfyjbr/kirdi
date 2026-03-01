#include "server/server.hpp"
#include "common/logger.hpp"
#include <kirdi/version.hpp>

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

    // Parse subnet base
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

    // Setup TLS if standalone
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

    LOG_INFOF("Listening on {}:{}", config_.listen_addr, config_.listen_port);

    do_accept();

    // Run the I/O context (blocks)
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
    } else {
        uint32_t sid = next_session_id_.fetch_add(1);
        std::string client_ip = allocate_client_ip();

        LOG_INFOF("New connection from {} → session {} (vip={})",
                  socket.remote_endpoint().address().to_string(), sid, client_ip);

        // Create SSL stream → WS stream → session
        auto ssl_stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(
            std::move(socket), ssl_ctx_
        );

        // Async SSL handshake
        ssl_stream->async_handshake(
            ssl::stream_base::server,
            [this, sid, client_ip, ssl_stream = std::move(ssl_stream)]
            (beast::error_code ec) mutable {
                if (ec) {
                    LOG_WARNF("Session {} SSL handshake failed: {}", sid, ec.message());
                    return;
                }

                // Create WebSocket stream
                auto ws = transport::WsServerSession::ws_stream(std::move(*ssl_stream));

                // Accept WebSocket upgrade
                ws.set_option(boost::beast::websocket::stream_base::timeout::suggested(
                    beast::role_type::server
                ));
                ws.binary(true);

                ws.async_accept(
                    [this, sid, client_ip, ws = std::make_shared<transport::WsServerSession::ws_stream>(std::move(ws))]
                    (beast::error_code ec) mutable {
                        if (ec) {
                            LOG_WARNF("Session {} WS accept failed: {}", sid, ec.message());
                            return;
                        }

                        auto ws_session = std::make_shared<transport::WsServerSession>(std::move(*ws), sid);
                        auto session = std::make_shared<ClientSession>(sid, client_ip, ws_session);

                        session->on_ip_packet([this](uint32_t sid, std::vector<uint8_t> pkt) {
                            // Write client's IP packet to TUN → internet
                            if (tun_ && tun_->is_open()) {
                                tun_->write_packet(pkt.data(), pkt.size());
                            }
                        });

                        {
                            std::lock_guard<std::mutex> lock(sessions_mutex_);
                            sessions_[sid] = session;

                            // Register IP → session mapping
                            struct in_addr addr{};
                            inet_pton(AF_INET, client_ip.c_str(), &addr);
                            ip_to_session_[addr.s_addr] = sid;
                        }

                        session->start();
                    }
                );
            }
        );
    }

    do_accept();
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
    if (len < 20) return;  // Too small for IPv4

    // Extract destination IP from IPv4 header
    uint32_t dst_ip = protocol::ip4_dst({data, len});

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = ip_to_session_.find(dst_ip);
    if (it != ip_to_session_.end()) {
        auto sit = sessions_.find(it->second);
        if (sit != sessions_.end() && sit->second->is_authenticated()) {
            sit->second->send_ip_packet(data, len);
        }
    }
}

} // namespace kirdi::server
