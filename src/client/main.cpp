#include "client/client.hpp"
#include "common/logger.hpp"
#include "common/config.hpp"
#include <kirdi/version.hpp>

#include <iostream>
#include <csignal>
#include <memory>
#include <atomic>

static std::unique_ptr<kirdi::client::Client> g_client;
static std::atomic<bool> g_shutdown{false};

static void signal_handler(int sig) {
    if (g_shutdown.exchange(true)) return;  // prevent double-stop
    LOG_INFOF("Signal {} received, shutting down", sig);
    try {
        if (g_client) g_client->stop();
    } catch (...) {}
}

static void print_usage(const char* argv0) {
    std::cerr << "kirdi-client v" << kirdi::version() << "\n\n"
              << "USAGE:\n"
              << "  " << argv0 << " [config.json]\n"
              << "  " << argv0 << " --host HOST --token TOKEN [options]\n"
              << "  " << argv0 << " --help\n\n"
              << "OPTIONS:\n"
              << "  --host HOST       Server hostname\n"
              << "  --port PORT       Server port (default: 443)\n"
              << "  --path PATH       WebSocket path (default: /tunnel/)\n"
              << "  --token TOKEN     Auth token\n"
              << "  --user USER       Auth user (default: kirdi)\n"
              << "  --no-route        Don't configure routes automatically\n"
              << "  --dns DNS         DNS server to use (default: 1.1.1.1)\n"
              << "  --log-level LVL   Log level: trace/debug/info/warn/error\n";
}

int main(int argc, char* argv[]) {
    kirdi::ClientConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }

        if (i == 1 && arg[0] != '-') {
            try {
                config = kirdi::parse_client_config(arg);
                continue;
            } catch (...) {}
        }

        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--host")       config.server_host = next();
        else if (arg == "--port")  config.server_port = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--path")  config.ws_path = next();
        else if (arg == "--token") config.auth_token = next();
        else if (arg == "--user")  config.auth_user = next();
        else if (arg == "--dns")   config.dns_server = next();
        else if (arg == "--no-route")   config.auto_route = false;
        else if (arg == "--log-level")  config.log_level = next();
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (config.server_host.empty()) {
        std::cerr << "Error: --host is required\n";
        print_usage(argv[0]);
        return 1;
    }
    if (config.auth_token.empty()) {
        std::cerr << "Error: --token is required\n";
        print_usage(argv[0]);
        return 1;
    }

    if (config.log_level == "trace")      kirdi::Logger::instance().set_level(kirdi::LogLevel::Trace);
    else if (config.log_level == "debug") kirdi::Logger::instance().set_level(kirdi::LogLevel::Debug);
    else if (config.log_level == "warn")  kirdi::Logger::instance().set_level(kirdi::LogLevel::Warn);
    else if (config.log_level == "error") kirdi::Logger::instance().set_level(kirdi::LogLevel::Error);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    g_client = std::make_unique<kirdi::client::Client>(std::move(config));

    try {
        g_client->run();
    } catch (const std::exception& e) {
        LOG_ERRORF("Fatal: {}", e.what());
    }

    g_client.reset();
    return 0;
}
