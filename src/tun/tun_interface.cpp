#include "tun/tun_interface.hpp"
#include "common/logger.hpp"

// Platform-specific includes and factory
#if defined(KIRDI_PLATFORM_LINUX)
#include "tun/tun_linux.hpp"
#elif defined(KIRDI_PLATFORM_MACOS)
#include "tun/tun_macos.hpp"
#elif defined(KIRDI_PLATFORM_WINDOWS)
#include "tun/tun_windows.hpp"
#endif

namespace kirdi::tun {

std::unique_ptr<TunDevice> create_tun_device() {
#if defined(KIRDI_PLATFORM_LINUX)
    LOG_DEBUG("Creating Linux TUN device");
    return std::make_unique<LinuxTunDevice>();
#elif defined(KIRDI_PLATFORM_MACOS)
    LOG_DEBUG("Creating macOS utun device");
    return std::make_unique<MacOSTunDevice>();
#elif defined(KIRDI_PLATFORM_WINDOWS)
    LOG_DEBUG("Creating Windows WinTUN device");
    return std::make_unique<WindowsTunDevice>();
#else
    #error "Unsupported platform for TUN"
#endif
}

} // namespace kirdi::tun
