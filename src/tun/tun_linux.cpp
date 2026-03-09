#ifdef KIRDI_PLATFORM_LINUX

#include "tun/tun_linux.hpp"
#include "common/logger.hpp"

#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace kirdi::tun {

// RAII wrapper for socket fd — prevents leaks in configure_address error paths
struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd() { if (fd >= 0) ::close(fd); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int release() { int f = fd; fd = -1; return f; }
};

LinuxTunDevice::~LinuxTunDevice() {
    close();
}

std::expected<void, TunError> LinuxTunDevice::open(const TunConfig& config) {
    if (fd_ >= 0) {
        LOG_ERROR("TUN device already open");
        return std::unexpected(TunError::AlreadyExists);
    }

    // Open /dev/net/tun
    fd_ = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        if (errno == EACCES || errno == EPERM) {
            LOG_ERROR("Permission denied opening /dev/net/tun — need root or CAP_NET_ADMIN");
            return std::unexpected(TunError::PermissionDenied);
        }
        LOG_ERRORF("Failed to open /dev/net/tun: {}", strerror(errno));
        return std::unexpected(TunError::DeviceNotFound);
    }

    // Configure TUN device — IFF_NO_PI strips the 4-byte packet info header
    struct ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

    if (!config.name.empty()) {
        std::strncpy(ifr.ifr_name, config.name.c_str(), IFNAMSIZ - 1);
    }

    if (ioctl(fd_, TUNSETIFF, &ifr) < 0) {
        LOG_ERRORF("ioctl TUNSETIFF failed: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return std::unexpected(TunError::ConfigFailed);
    }

    if_name_ = ifr.ifr_name;
    mtu_ = config.mtu;

    LOG_INFOF("TUN device created: {} (fd={})", if_name_, fd_);

    // Set non-blocking for async integration (poll/epoll)
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        LOG_ERRORF("fcntl F_GETFL failed: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return std::unexpected(TunError::SystemError);
    }
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERRORF("fcntl F_SETFL O_NONBLOCK failed: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return std::unexpected(TunError::SystemError);
    }

    // Configure IP address and bring up
    auto result = configure_address(config);
    if (!result) {
        ::close(fd_);
        fd_ = -1;
        return result;
    }

    return {};
}

void LinuxTunDevice::close() {
    if (fd_ >= 0) {
        LOG_INFOF("Closing TUN device: {}", if_name_);
        ::close(fd_);
        fd_ = -1;
    }
}

std::expected<std::vector<uint8_t>, TunError> LinuxTunDevice::read_packet() {
    if (fd_ < 0) {
        return std::unexpected(TunError::ReadFailed);
    }

    std::vector<uint8_t> buf(mtu_ + 64);  // Extra space for oversized packets

    ssize_t n = ::read(fd_, buf.data(), buf.size());
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::vector<uint8_t>{};  // No data available (non-blocking)
        }
        LOG_ERRORF("TUN read failed: {}", strerror(errno));
        return std::unexpected(TunError::ReadFailed);
    }

    buf.resize(static_cast<size_t>(n));
    return buf;
}

std::expected<size_t, TunError> LinuxTunDevice::write_packet(const uint8_t* data, size_t len) {
    if (fd_ < 0) {
        return std::unexpected(TunError::WriteFailed);
    }
    if (data == nullptr || len == 0) {
        return 0;
    }

    ssize_t n = ::write(fd_, data, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // Would block
        }
        LOG_ERRORF("TUN write failed: {}", strerror(errno));
        return std::unexpected(TunError::WriteFailed);
    }
    return static_cast<size_t>(n);
}

std::expected<void, TunError> LinuxTunDevice::configure_address(const TunConfig& config) {
    int raw_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (raw_fd < 0) {
        LOG_ERRORF("Failed to create config socket: {}", strerror(errno));
        return std::unexpected(TunError::SystemError);
    }
    ScopedFd sockfd(raw_fd);

    // Validate MTU range — ifr_mtu is int, typical range 68..65535
    if (config.mtu < 68 || config.mtu > 65535) {
        LOG_ERRORF("Invalid MTU {}: must be 68..65535", config.mtu);
        return std::unexpected(TunError::ConfigFailed);
    }

    // ── Set IP address ──────────────────────────────────────────────────────
    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, if_name_.c_str(), IFNAMSIZ - 1);

    auto* addr_in = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
    addr_in->sin_family = AF_INET;
    if (inet_pton(AF_INET, config.address.c_str(), &addr_in->sin_addr) != 1) {
        LOG_ERRORF("Invalid TUN address: '{}'", config.address);
        return std::unexpected(TunError::ConfigFailed);
    }

    if (ioctl(sockfd.fd, SIOCSIFADDR, &ifr) < 0) {
        LOG_ERRORF("Failed to set TUN address '{}': {}", config.address, strerror(errno));
        return std::unexpected(TunError::ConfigFailed);
    }

    // ── Set netmask ─────────────────────────────────────────────────────────
    // Re-zero the sockaddr union (ifr_netmask aliases ifr_addr)
    std::memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    auto* mask_in = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_netmask);
    mask_in->sin_family = AF_INET;
    if (inet_pton(AF_INET, config.netmask.c_str(), &mask_in->sin_addr) != 1) {
        LOG_ERRORF("Invalid TUN netmask: '{}'", config.netmask);
        return std::unexpected(TunError::ConfigFailed);
    }

    if (ioctl(sockfd.fd, SIOCSIFNETMASK, &ifr) < 0) {
        LOG_ERRORF("Failed to set TUN netmask '{}': {}", config.netmask, strerror(errno));
        return std::unexpected(TunError::ConfigFailed);
    }

    // ── Set MTU ─────────────────────────────────────────────────────────────
    ifr.ifr_mtu = static_cast<int>(config.mtu);
    if (ioctl(sockfd.fd, SIOCSIFMTU, &ifr) < 0) {
        LOG_ERRORF("Failed to set TUN MTU {}: {}", config.mtu, strerror(errno));
        return std::unexpected(TunError::ConfigFailed);
    }

    // ── Bring interface up ──────────────────────────────────────────────────
    if (ioctl(sockfd.fd, SIOCGIFFLAGS, &ifr) < 0) {
        LOG_ERRORF("Failed to get interface flags: {}", strerror(errno));
        return std::unexpected(TunError::ConfigFailed);
    }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sockfd.fd, SIOCSIFFLAGS, &ifr) < 0) {
        LOG_ERRORF("Failed to bring TUN interface up: {}", strerror(errno));
        return std::unexpected(TunError::ConfigFailed);
    }

    LOG_INFOF("TUN {} configured: addr={} mask={} mtu={}",
              if_name_, config.address, config.netmask, config.mtu);
    return {};
}

} // namespace kirdi::tun

#endif // KIRDI_PLATFORM_LINUX
