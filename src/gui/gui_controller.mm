// ── GUI Controller Implementation ────────────────────────────────────────────
// Compiled as Objective-C++ with C++17 standard.
// Includes ONLY webview.h and gui_bridge.hpp — NO kirdi internals.
// This is the key to solving the C++17/C++23 split.

#include "webview/webview.h"
#include "gui/gui_controller.hpp"

#include <kirdi/version.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>

namespace kirdi::gui {

// ── Helper: cast opaque pointer to webview ──────────────────────────────────
static webview::webview& wv(void* p) {
    return *static_cast<webview::webview*>(p);
}

// ── Constructor / Destructor ─────────────────────────────────────────────────
GuiController::GuiController() = default;

GuiController::~GuiController() {
    stop_vpn();
}

// ── Main entry point ─────────────────────────────────────────────────────────
void GuiController::run(const std::string& gui_dist_path) {
    std::cout << "[kirdi] GUI v" << KIRDI_VERSION_STRING << " starting" << std::endl;

    // Create webview (debug mode in Debug builds)
#ifdef NDEBUG
    webview::webview w(false, nullptr);
#else
    webview::webview w(true, nullptr);
#endif

    w.set_title("kirdi VPN");
    w.set_size(520, 780, WEBVIEW_HINT_NONE);
    w.set_size(420, 600, WEBVIEW_HINT_MIN);

    void* wv_ptr = static_cast<void*>(&w);

    // ── Bind JS → C++ ───────────────────────────────────────────────────────
    w.bind("__kirdi_connect", [this, wv_ptr](const std::string& seq, const std::string& req, void*) {
        try {
            auto args = nlohmann::json::parse(req);
            std::string config_json = args.at(0).get<std::string>();
            handle_connect(wv_ptr, config_json);
            wv(wv_ptr).resolve(seq, 0, "\"ok\"");
        } catch (const std::exception& e) {
            wv(wv_ptr).resolve(seq, 1, std::string("\"") + e.what() + "\"");
        }
    }, nullptr);

    w.bind("__kirdi_disconnect", [this, wv_ptr](const std::string& seq, const std::string&, void*) {
        handle_disconnect(wv_ptr);
        wv(wv_ptr).resolve(seq, 0, "\"ok\"");
    }, nullptr);

    w.bind("__kirdi_getStatus", [this, wv_ptr](const std::string& seq, const std::string&, void*) {
        std::string result = handle_get_status();
        wv(wv_ptr).resolve(seq, 0, result);
    }, nullptr);

    // Inject JS bridge object
    w.init(R"(
        window.__kirdi = {
            connect: function(config) {
                return __kirdi_connect(typeof config === 'string' ? config : JSON.stringify(config));
            },
            disconnect: function() {
                return __kirdi_disconnect();
            },
            getStatus: function() {
                return __kirdi_getStatus();
            }
        };
    )");

    // ── Navigate to React app ────────────────────────────────────────────────
    std::string dist_path;
    {
        namespace fs = std::filesystem;
        std::vector<std::string> candidates = {
            gui_dist_path,
            "../gui/dist/index.html",
            "gui/dist/index.html",
            "./dist/index.html",
            "../../gui/dist/index.html",
        };
        for (const auto& path : candidates) {
            if (!path.empty() && fs::exists(path)) {
                dist_path = fs::canonical(path).string();
                break;
            }
        }
    }

    if (dist_path.empty()) {
        std::cerr << "[kirdi] Cannot find gui/dist/index.html" << std::endl;
        w.set_html(
            "<html><body style='background:#000;color:#fff;font-family:sans-serif;"
            "display:flex;justify-content:center;align-items:center;height:100vh;margin:0'>"
            "<div style='text-align:center'><h2>Build Error</h2>"
            "<p>Cannot find gui/dist/index.html</p>"
            "<p style='color:#666'>Run <code>cd gui && npm run build</code></p>"
            "</div></body></html>");
    } else {
        std::cout << "[kirdi] Loading GUI from: " << dist_path << std::endl;
        w.navigate("file://" + dist_path);
    }

    // ── Run event loop (blocks) ──────────────────────────────────────────────
    w.run();
    std::cout << "[kirdi] GUI shutting down" << std::endl;
    stop_vpn();
}

// ── Connect handler ──────────────────────────────────────────────────────────
void GuiController::handle_connect(void* wv_opaque, const std::string& config_json) {
    stop_vpn();
    std::cout << "[kirdi] GUI: Starting VPN connection" << std::endl;

    std::string error;
    vpn_ = bridge::vpn_create(config_json, error);
    if (!vpn_) {
        push_status_to_js(wv_opaque, "error", "{\"message\":\"" + error + "\"}");
        return;
    }

    // Set callback to push status updates to JS
    bridge::vpn_set_callback(vpn_, [this, wv_opaque](const std::string& event, const std::string& data) {
        push_status_to_js(wv_opaque, event, data);
    });

    // Run VPN in worker thread
    vpn_running_ = true;
    vpn_thread_ = std::thread([this, wv_opaque]() {
        bridge::vpn_start(vpn_);
        vpn_running_ = false;
        push_status_to_js(wv_opaque, "disconnected", "{}");
    });
}

// ── Disconnect handler ───────────────────────────────────────────────────────
void GuiController::handle_disconnect(void* wv_opaque) {
    std::cout << "[kirdi] GUI: Disconnecting" << std::endl;
    stop_vpn();
    push_status_to_js(wv_opaque, "disconnected", "{}");
}

// ── Get status ───────────────────────────────────────────────────────────────
std::string GuiController::handle_get_status() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    nlohmann::json result = {
        {"status", current_status_},
        {"data", nlohmann::json::parse(current_data_, nullptr, false)}
    };
    return result.dump();
}

// ── Push status to JS ────────────────────────────────────────────────────────
void GuiController::push_status_to_js(void* wv_opaque, const std::string& event, const std::string& json_data) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_status_ = event;
        current_data_ = json_data;
    }

    std::string payload = "{\"status\":\"" + event + "\",\"data\":" + json_data + "}";

    wv(wv_opaque).dispatch([wv_opaque, payload]() {
        std::string js = "if(window.__kirdi_update){window.__kirdi_update('" + payload + "');}";
        wv(wv_opaque).eval(js);
    });
}

// ── Stop VPN ─────────────────────────────────────────────────────────────────
void GuiController::stop_vpn() {
    if (vpn_) {
        bridge::vpn_stop(vpn_);
    }
    if (vpn_thread_.joinable()) {
        vpn_thread_.join();
    }
    if (vpn_) {
        bridge::vpn_destroy(vpn_);
        vpn_ = nullptr;
    }
    vpn_running_ = false;
}

} // namespace kirdi::gui
