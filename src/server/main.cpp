#include "server/server.hpp"
#include "common/logger.hpp"
#include "common/config.hpp"
#include <kirdi/version.hpp>

#include <iostream>
#include <csignal>
#include <memory>

static std::unique_ptr<kirdi::server::Server> g_server;

static void signal_handler(int sig) {
    LOG_INFOF("Signal {} received, shutting down", sig);
    if (g_server) g_server->stop();
}

static void print_usage(const char* argv0) {
    std::cerr << "kirdi-server v" << kirdi::version() << "\n\n"
              << "USAGE:\n"
              << "  " << argv0 << " [config.json]\n"
              << "  " << argv0 << " --help\n\n"
              << "If no config file is specified, looks for /etc/kirdi/server.json\n";
}

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        print_usage(argv[0]);
        return 0;
    }

    std::string config_path = "/etc/kirdi/server.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    // Parse config
    kirdi::ServerConfig config;
    try {
        config = kirdi::parse_server_config(config_path);
    } catch (const std::exception& e) {
        LOG_ERRORF("Failed to load config: {}", e.what());
        LOG_INFO("Using default configuration");
        config.auth_secret = "CHANGE_ME";
    }

    // Set log level
    if (config.log_level == "trace")      kirdi::Logger::instance().set_level(kirdi::LogLevel::Trace);
    else if (config.log_level == "debug") kirdi::Logger::instance().set_level(kirdi::LogLevel::Debug);
    else if (config.log_level == "warn")  kirdi::Logger::instance().set_level(kirdi::LogLevel::Warn);
    else if (config.log_level == "error") kirdi::Logger::instance().set_level(kirdi::LogLevel::Error);

    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Run server
    g_server = std::make_unique<kirdi::server::Server>(std::move(config));
    g_server->run();

    return 0;
}
