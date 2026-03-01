#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <expected>

namespace kirdi::tun {

// ── TUN Device Abstraction ──────────────────────────────────────────────────
//
// L3 (IP-level) virtual network interface.
// Reads/writes raw IP packets (both IPv4 and IPv6).
//
// Platform implementations:
//   Linux:   /dev/net/tun + ioctl(TUNSETIFF, IFF_TUN | IFF_NO_PI)
//   macOS:   utun via sys/kern_control.h + CTLIOCGINFO
//   Windows: WinTUN driver (ring buffer API)
//
// ─────────────────────────────────────────────────────────────────────────────

struct TunConfig {
    std::string name;           // Desired interface name (e.g. "kirdi0"), empty = auto
    std::string address;        // IPv4 address (e.g. "10.8.0.2")
    std::string netmask;        // Netmask (e.g. "255.255.255.0")
    uint32_t    mtu = 1400;     // MTU (leave room for WS + TLS overhead)
};

enum class TunError {
    PermissionDenied,
    DeviceNotFound,
    AlreadyExists,
    ConfigFailed,
    ReadFailed,
    WriteFailed,
    SystemError,
};

class TunDevice {
public:
    virtual ~TunDevice() = default;

    // Create and configure the TUN device. Must be called before read/write.
    virtual std::expected<void, TunError> open(const TunConfig& config) = 0;

    // Close the TUN device and release resources.
    virtual void close() = 0;

    // Read one IP packet from the TUN device (blocking).
    // Returns the raw IP packet data, or error.
    virtual std::expected<std::vector<uint8_t>, TunError> read_packet() = 0;

    // Write one raw IP packet into the TUN device.
    virtual std::expected<size_t, TunError> write_packet(const uint8_t* data, size_t len) = 0;

    // Get the native file descriptor (for epoll/kqueue/IOCP integration)
    virtual int native_fd() const = 0;

    // Get the actual interface name assigned by the OS
    virtual std::string interface_name() const = 0;

    // Is the device open?
    virtual bool is_open() const = 0;
};

// ── Factory ─────────────────────────────────────────────────────────────────
// Creates platform-specific TUN device.
std::unique_ptr<TunDevice> create_tun_device();

} // namespace kirdi::tun
