#pragma once

#include "tun/tun_interface.hpp"
#include <string>

namespace kirdi::tun {

// ── Windows WinTUN Implementation ───────────────────────────────────────────
//
// Uses the WinTUN driver (https://www.wintun.net/) for userspace TUN.
// wintun.dll is loaded dynamically at runtime via LoadLibrary.
//
// WinTUN ring buffer API (zero-copy where possible):
//   WintunReceivePacket     — returns pointer into ring buffer (zero-copy read)
//   WintunReleaseReceivePacket — releases the ring buffer slot after read
//   WintunAllocateSendPacket   — allocates space in send ring
//   WintunSendPacket           — commits the packet into the send ring
//   WintunGetReadWaitEvent     — HANDLE for WaitForSingleObject polling
//
// IP configuration via netsh commands (consistent with Linux/macOS approach).
// Requires Administrator privileges.
// ─────────────────────────────────────────────────────────────────────────────

class WindowsTunDevice : public TunDevice {
public:
    WindowsTunDevice();
    ~WindowsTunDevice() override;

    // Non-copyable, non-movable (RAII handles)
    WindowsTunDevice(const WindowsTunDevice&) = delete;
    WindowsTunDevice& operator=(const WindowsTunDevice&) = delete;

    std::expected<void, TunError> open(const TunConfig& config) override;
    void close() override;
    std::expected<std::vector<uint8_t>, TunError> read_packet() override;
    std::expected<size_t, TunError> write_packet(const uint8_t* data, size_t len) override;
    int native_fd() const override { return -1; }  // Windows uses HANDLE, not fd
    std::string interface_name() const override { return if_name_; }
    bool is_open() const override { return session_ != nullptr; }

private:
    std::string if_name_;
    uint32_t mtu_ = 1400;

    // Opaque handles (void* to avoid #include <windows.h> in header)
    void* wintun_dll_ = nullptr;     // HMODULE
    void* adapter_ = nullptr;        // WINTUN_ADAPTER_HANDLE
    void* session_ = nullptr;        // WINTUN_SESSION_HANDLE
    void* read_event_ = nullptr;     // HANDLE from WintunGetReadWaitEvent

    // WinTUN API function table (defined in .cpp, forward-declared here).
    // unique_ptr is safe with incomplete type because ~WindowsTunDevice()
    // is defined in the .cpp where WintunFuncs is complete.
    struct WintunFuncs;
    std::unique_ptr<WintunFuncs> funcs_;

    bool load_wintun();
    void unload_wintun();
    std::expected<void, TunError> configure_address(const TunConfig& config);
};

} // namespace kirdi::tun
