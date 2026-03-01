#pragma once

#include "tun/tun_interface.hpp"
#include <string>
#include <array>

namespace kirdi::tun {

// ── Linux TUN Implementation ────────────────────────────────────────────────
//
// Uses /dev/net/tun with ioctl(TUNSETIFF, IFF_TUN | IFF_NO_PI).
// IFF_NO_PI strips the 4-byte packet info header — we get raw IP packets.
//
// Requires CAP_NET_ADMIN or root.
// ─────────────────────────────────────────────────────────────────────────────

class LinuxTunDevice : public TunDevice {
public:
    LinuxTunDevice() = default;
    ~LinuxTunDevice() override;

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

    // Configure IP address and bring interface up using netlink/ioctl
    std::expected<void, TunError> configure_address(const TunConfig& config);
};

} // namespace kirdi::tun
