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

LinuxTunDevice::~LinuxTunDevice() {
    close();
}

std::expected<void, TunError> LinuxTunDevice::open(const TunConfig& config) {
    // Open /dev/net/tun
    fd_ = ::open("/dev/net/tun", O_RDWR);
    if (fd_ < 0) {
        if (errno == EACCES || errno == EPERM) {
            LOG_ERROR("Permission denied opening /dev/net/tun — need root or CAP_NET_ADMIN");
            return std::unexpected(TunError::PermissionDenied);
        }
        LOG_ERRORF("Failed to open /dev/net/tun: {}", strerror(errno));
        return std::unexpected(TunError::DeviceNotFound);
    }

    // Configure TUN device
    struct ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;  // TUN mode, no packet info header

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

    // Set non-blocking for async integration
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

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
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        LOG_ERRORF("Failed to create socket for TUN config: {}", strerror(errno));
        return std::unexpected(TunError::SystemError);
    }

    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, if_name_.c_str(), IFNAMSIZ - 1);

    // Set IP address
    auto* addr_in = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
    addr_in->sin_family = AF_INET;
    inet_pton(AF_INET, config.address.c_str(), &addr_in->sin_addr);

    if (ioctl(sockfd, SIOCSIFADDR, &ifr) < 0) {
        LOG_ERRORF("Failed to set TUN address: {}", strerror(errno));
        ::close(sockfd);
        return std::unexpected(TunError::ConfigFailed);
    }

    // Set netmask
    auto* mask_in = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_netmask);
    mask_in->sin_family = AF_INET;
    inet_pton(AF_INET, config.netmask.c_str(), &mask_in->sin_addr);

    if (ioctl(sockfd, SIOCSIFNETMASK, &ifr) < 0) {
        LOG_ERRORF("Failed to set TUN netmask: {}", strerror(errno));
        ::close(sockfd);
        return std::unexpected(TunError::ConfigFailed);
    }

    // Set MTU
    ifr.ifr_mtu = static_cast<int>(config.mtu);
    if (ioctl(sockfd, SIOCSIFMTU, &ifr) < 0) {
        LOG_ERRORF("Failed to set TUN MTU: {}", strerror(errno));
        ::close(sockfd);
        return std::unexpected(TunError::ConfigFailed);
    }

    // Bring interface up
    if (ioctl(sockfd, SIOCGIFFLAGS, &ifr) < 0) {
        ::close(sockfd);
        return std::unexpected(TunError::ConfigFailed);
    }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sockfd, SIOCSIFFLAGS, &ifr) < 0) {
        LOG_ERRORF("Failed to bring TUN interface up: {}", strerror(errno));
        ::close(sockfd);
        return std::unexpected(TunError::ConfigFailed);
    }

    ::close(sockfd);
    LOG_INFOF("TUN {} configured: addr={} mask={} mtu={}",
              if_name_, config.address, config.netmask, config.mtu);
    return {};
}

} // namespace kirdi::tun

#endif // KIRDI_PLATFORM_LINUX
