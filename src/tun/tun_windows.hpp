#pragma once

#include "tun/tun_interface.hpp"
#include <string>

namespace kirdi::tun {

// ── Windows WinTUN Implementation (Stub) ────────────────────────────────────
//
// Windows TUN requires the WinTUN driver (used by WireGuard):
//   https://www.wintun.net/
//
// WinTUN provides a userspace API with ring buffers for packet exchange.
// The driver must be installed separately (wintun.dll).
//
// TODO: Full implementation using WinTUN ring buffer API:
//   - WintunCreateAdapter()
//   - WintunStartSession()
//   - WintunReceivePacket() / WintunSendPacket()
//   - WintunReleaseReceivePacket()
//   - WintunEndSession()
//   - WintunCloseAdapter()
// ─────────────────────────────────────────────────────────────────────────────

class WindowsTunDevice : public TunDevice {
public:
    WindowsTunDevice() = default;
    ~WindowsTunDevice() override;

    std::expected<void, TunError> open(const TunConfig& config) override;
    void close() override;
    std::expected<std::vector<uint8_t>, TunError> read_packet() override;
    std::expected<size_t, TunError> write_packet(const uint8_t* data, size_t len) override;
    int native_fd() const override { return -1; }  // Windows uses HANDLE, not fd
    std::string interface_name() const override { return if_name_; }
    bool is_open() const override { return open_; }

private:
    bool open_ = false;
    std::string if_name_;
    uint32_t mtu_ = 1400;
    // TODO: WINTUN_SESSION_HANDLE, WINTUN_ADAPTER_HANDLE
};

} // namespace kirdi::tun
