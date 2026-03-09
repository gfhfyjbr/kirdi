#include "server/server.hpp"
#include "common/logger.hpp"
#include <kirdi/version.hpp>

#include <boost/beast/http.hpp>

#include <thread>
#include <arpa/inet.h>
#include <cstdlib>
#include <fstream>
#include <poll.h>

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

void Server::ensure_system_config() {
#if defined(__linux__)
    // ── ip_forward ──────────────────────────────────────────────────────────
    {
        std::ifstream f("/proc/sys/net/ipv4/ip_forward");
        int val = 0;
        if (f >> val && val == 1) {
            LOG_INFO("ip_forward already enabled");
        } else {
            LOG_WARN("ip_forward is OFF — enabling");
            std::system("sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1");
        }
    }

    // ── Detect default outbound interface ───────────────────────────────────
    std::string iface;
    {
        FILE* fp = popen("ip route show default 2>/dev/null | awk '/default/ {print $5}' | head -1", "r");
        if (fp) {
            char buf[64] = {};
            if (fgets(buf, sizeof(buf), fp)) {
                for (char* p = buf; *p; ++p) if (*p == '\n' || *p == '\r') *p = 0;
                iface = buf;
            }
            pclose(fp);
        }
        if (iface.empty()) iface = "eth0";
    }
    LOG_INFOF("Default interface: {}", iface);

    // ── FORWARD rules for kirdi TUN ─────────────────────────────────────────
    auto ensure_rule = [](const std::string& check, const std::string& add, const std::string& desc) {
        if (std::system(check.c_str()) != 0) {
            LOG_INFOF("Adding iptables rule: {}", desc);
            if (std::system(add.c_str()) != 0) {
                LOG_WARNF("Failed to add rule: {}", desc);
            }
        } else {
            LOG_DEBUGF("iptables rule already present: {}", desc);
        }
    };

    ensure_rule(
        "iptables -C FORWARD -i kirdi0 -j ACCEPT 2>/dev/null",
        "iptables -A FORWARD -i kirdi0 -j ACCEPT",
        "FORWARD -i kirdi0 -j ACCEPT"
    );
    ensure_rule(
        "iptables -C FORWARD -o kirdi0 -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null",
        "iptables -A FORWARD -o kirdi0 -m state --state RELATED,ESTABLISHED -j ACCEPT",
        "FORWARD -o kirdi0 RELATED,ESTABLISHED"
    );
    ensure_rule(
        "iptables -t nat -C POSTROUTING -s 10.8.0.0/24 -o " + iface + " -j MASQUERADE 2>/dev/null",
        "iptables -t nat -A POSTROUTING -s 10.8.0.0/24 -o " + iface + " -j MASQUERADE",
        "MASQUERADE 10.8.0.0/24 -> " + iface
    );

    // TCP MSS clamping — prevent fragmentation for tunneled TCP
    ensure_rule(
        "iptables -t mangle -C FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null",
        "iptables -t mangle -A FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu",
        "MSS clamp-to-pmtu"
    );

    LOG_INFO("System network config verified");
#else
    LOG_WARN("ensure_system_config: not on Linux, skipping iptables/sysctl setup");
#endif
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

    // Ensure iptables FORWARD + MASQUERADE + ip_forward are set
    ensure_system_config();

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

    // Disable Nagle — send packets immediately, critical for VPN latency
    socket.set_option(tcp::no_delay(true));

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

                auto ws = std::make_shared<transport::WsServerSession::ws_stream>(std::move(*ssl_stream));

                ws->set_option(boost::beast::websocket::stream_base::timeout::suggested(
                    beast::role_type::server
                ));
                ws->binary(true);

                ws->async_accept(
                    [this, sid, client_ip, ws]
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
    session->set_auth_secret(config_.auth_secret);

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
        // Wait for data on TUN fd instead of polling with sleep
        struct pollfd pfd{};
        pfd.fd = tun_->native_fd();
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, 100);  // 100ms timeout to check tun_->is_open()
        if (ret <= 0) continue;

        auto result = tun_->read_packet();
        if (!result || result.value().empty()) continue;

        route_to_client(result.value().data(), result.value().size());
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
