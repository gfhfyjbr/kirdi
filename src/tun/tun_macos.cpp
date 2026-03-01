#ifdef KIRDI_PLATFORM_MACOS

#include "tun/tun_macos.hpp"
#include "common/logger.hpp"

#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <net/if_utun.h>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace kirdi::tun {

// utun protocol family header (4 bytes, host byte order on macOS)
static constexpr uint32_t UTUN_AF_INET  = AF_INET;   // 2
static constexpr uint32_t UTUN_AF_INET6 = AF_INET6;  // 30
static constexpr size_t UTUN_HEADER_SIZE = 4;

MacOSTunDevice::~MacOSTunDevice() {
    close();
}

std::expected<void, TunError> MacOSTunDevice::open(const TunConfig& config) {
    // Create PF_SYSTEM socket
    fd_ = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd_ < 0) {
        LOG_ERRORF("Failed to create PF_SYSTEM socket: {}", strerror(errno));
        return std::unexpected(TunError::PermissionDenied);
    }

    // Look up utun control by name
    struct ctl_info ci{};
    std::strncpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof(ci.ctl_name) - 1);

    if (ioctl(fd_, CTLIOCGINFO, &ci) < 0) {
        LOG_ERRORF("ioctl CTLIOCGINFO failed: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return std::unexpected(TunError::DeviceNotFound);
    }

    // Connect to utun control — sc_unit 0 = auto-assign
    struct sockaddr_ctl sc{};
    sc.sc_id = ci.ctl_id;
    sc.sc_len = sizeof(sc);
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = AF_SYS_CONTROL;
    sc.sc_unit = 0;  // Auto-assign utun number

    if (connect(fd_, reinterpret_cast<struct sockaddr*>(&sc), sizeof(sc)) < 0) {
        LOG_ERRORF("Failed to connect utun control: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return std::unexpected(TunError::ConfigFailed);
    }

    // Get assigned interface name
    char ifname[IFNAMSIZ] = {};
    socklen_t ifname_len = sizeof(ifname);
    if (getsockopt(fd_, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, ifname, &ifname_len) < 0) {
        LOG_ERRORF("getsockopt UTUN_OPT_IFNAME failed: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return std::unexpected(TunError::ConfigFailed);
    }
    if_name_ = ifname;
    mtu_ = config.mtu;

    LOG_INFOF("utun device created: {} (fd={})", if_name_, fd_);

    // Set non-blocking
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    // Configure address via ifconfig command (macOS way)
    auto result = configure_address(config);
    if (!result) {
        ::close(fd_);
        fd_ = -1;
        return result;
    }

    return {};
}

void MacOSTunDevice::close() {
    if (fd_ >= 0) {
        LOG_INFOF("Closing utun device: {}", if_name_);
        ::close(fd_);
        fd_ = -1;
    }
}

std::expected<std::vector<uint8_t>, TunError> MacOSTunDevice::read_packet() {
    // utun prepends 4-byte AF header
    std::vector<uint8_t> buf(mtu_ + UTUN_HEADER_SIZE + 64);

    ssize_t n = ::read(fd_, buf.data(), buf.size());
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::vector<uint8_t>{};
        }
        LOG_ERRORF("utun read failed: {}", strerror(errno));
        return std::unexpected(TunError::ReadFailed);
    }

    if (static_cast<size_t>(n) <= UTUN_HEADER_SIZE) {
        return std::vector<uint8_t>{};  // Too small
    }

    // Strip the 4-byte AF header — return raw IP packet
    std::vector<uint8_t> ip_pkt(buf.begin() + UTUN_HEADER_SIZE, buf.begin() + n);
    return ip_pkt;
}

std::expected<size_t, TunError> MacOSTunDevice::write_packet(const uint8_t* data, size_t len) {
    if (len == 0) return 0;

    // Determine AF from IP version field
    uint8_t ip_ver = (data[0] >> 4) & 0x0F;
    uint32_t af;
    if (ip_ver == 4) {
        af = UTUN_AF_INET;
    } else if (ip_ver == 6) {
        af = UTUN_AF_INET6;
    } else {
        LOG_WARNF("Unknown IP version {} in write_packet", ip_ver);
        return std::unexpected(TunError::WriteFailed);
    }

    // Prepend 4-byte AF header
    std::vector<uint8_t> buf(UTUN_HEADER_SIZE + len);
    std::memcpy(buf.data(), &af, UTUN_HEADER_SIZE);
    std::memcpy(buf.data() + UTUN_HEADER_SIZE, data, len);

    ssize_t n = ::write(fd_, buf.data(), buf.size());
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        LOG_ERRORF("utun write failed: {}", strerror(errno));
        return std::unexpected(TunError::WriteFailed);
    }

    // Return bytes of IP payload written (subtract header)
    return (static_cast<size_t>(n) > UTUN_HEADER_SIZE)
        ? static_cast<size_t>(n) - UTUN_HEADER_SIZE
        : 0;
}

std::expected<void, TunError> MacOSTunDevice::configure_address(const TunConfig& config) {
    // macOS: use ifconfig to set address and bring up
    // ifconfig utunN inet ADDR PEER_ADDR netmask MASK mtu MTU up
    // For point-to-point: peer = server IP (config.address is our side)
    std::string cmd = "ifconfig " + if_name_
        + " inet " + config.address
        + " " + config.address  // peer address (point-to-point)
        + " netmask " + config.netmask
        + " mtu " + std::to_string(config.mtu)
        + " up";

    LOG_DEBUGF("Running: {}", cmd);
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        LOG_ERRORF("ifconfig failed with exit code {}", ret);
        return std::unexpected(TunError::ConfigFailed);
    }

    LOG_INFOF("utun {} configured: addr={} mask={} mtu={}",
              if_name_, config.address, config.netmask, config.mtu);
    return {};
}

} // namespace kirdi::tun

#endif // KIRDI_PLATFORM_MACOS
