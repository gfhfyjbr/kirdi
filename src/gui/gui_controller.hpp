#pragma once

// ── GUI Controller Header ────────────────────────────────────────────────────
// ZERO kirdi internals, ZERO webview includes.
// Only standard library + bridge types.
// This file is safe to include from C++17 .mm files.

#include "gui/gui_bridge.hpp"

#include <memory>
#include <thread>
#include <atomic>
#include <string>
#include <mutex>

namespace kirdi::gui {

class GuiController {
public:
    GuiController();
    ~GuiController();

    // Run the GUI (blocks on main thread). Call this from main().
    void run(const std::string& gui_dist_path);

private:
    // VPN handle (opaque, managed by bridge)
    bridge::VpnHandle* vpn_{nullptr};
    std::thread vpn_thread_;
    std::atomic<bool> vpn_running_{false};

    // Cached status for getStatus() calls
    std::string current_status_{"disconnected"};
    std::string current_data_{"{}"};
    std::mutex state_mutex_;

    // Handlers called from webview bindings
    void handle_connect(void* wv_opaque, const std::string& config_json);
    void handle_disconnect(void* wv_opaque);
    std::string handle_get_status();

    // Push status update to JS
    void push_status_to_js(void* wv_opaque, const std::string& event, const std::string& json_data);

    // Cleanup VPN
    void stop_vpn();
};

} // namespace kirdi::gui
