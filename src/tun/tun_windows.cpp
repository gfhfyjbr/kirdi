#ifdef KIRDI_PLATFORM_WINDOWS

#include "tun/tun_windows.hpp"
#include "common/logger.hpp"

namespace kirdi::tun {

WindowsTunDevice::~WindowsTunDevice() {
    close();
}

std::expected<void, TunError> WindowsTunDevice::open(const TunConfig& config) {
    // TODO: Load wintun.dll, create adapter, start session
    LOG_ERROR("Windows TUN not yet implemented — WinTUN driver integration pending");
    return std::unexpected(TunError::SystemError);
}

void WindowsTunDevice::close() {
    if (open_) {
        LOG_INFO("Closing Windows TUN device");
        open_ = false;
    }
}

std::expected<std::vector<uint8_t>, TunError> WindowsTunDevice::read_packet() {
    return std::unexpected(TunError::SystemError);
}

std::expected<size_t, TunError> WindowsTunDevice::write_packet(const uint8_t* data, size_t len) {
    return std::unexpected(TunError::SystemError);
}

} // namespace kirdi::tun

#endif // KIRDI_PLATFORM_WINDOWS
