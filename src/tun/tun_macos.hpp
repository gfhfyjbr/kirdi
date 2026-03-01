#pragma once

#include "tun/tun_interface.hpp"
#include <string>

namespace kirdi::tun {

// ── macOS utun Implementation ───────────────────────────────────────────────
//
// macOS uses the utun kernel control interface:
//   1. Create a PF_SYSTEM socket
//   2. Connect to com.apple.net.utun_control via CTLIOCGINFO
//   3. Set sc_unit to desired utun number (0 = auto-assign)
//
// utun adds a 4-byte AF header before each packet (protocol family):
//   - AF_INET  (0x00000002) for IPv4
//   - AF_INET6 (0x0000001E) for IPv6
// We strip this header on read and prepend on write.
//
// Requires root or specific entitlements.
// ─────────────────────────────────────────────────────────────────────────────

class MacOSTunDevice : public TunDevice {
public:
    MacOSTunDevice() = default;
    ~MacOSTunDevice() override;

    std::expected<void, TunError> open(const TunConfig& config) override;
    void close() override;
    std::expected<std::vector<uint8_t>, TunError> read_packet() override;
    std::expected<size_t, TunError> write_packet(const uint8_t* data, size_t len) override;
    int native_fd() const override { return fd_; }
    std::string interface_name() const override { return if_name_; }
    bool is_open() const override { return fd_ >= 0; }

private:
    int fd_ = -1;
    std::string if_name_;
    uint32_t mtu_ = 1400;

    // Configure via ifconfig (macOS doesn't support netlink)
    std::expected<void, TunError> configure_address(const TunConfig& config);
};

} // namespace kirdi::tun
