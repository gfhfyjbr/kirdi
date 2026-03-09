#ifdef KIRDI_PLATFORM_WINDOWS

#include "tun/tun_windows.hpp"
#include "common/logger.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <format>

namespace kirdi::tun {

// ── Constants ───────────────────────────────────────────────────────────────

// Ring buffer capacity for WinTUN session.
// Must be a power of 2 between 0x20000 (128 KB) and 0x4000000 (64 MB).
// 4 MB is a solid default — handles burst traffic without excessive memory.
static constexpr DWORD WINTUN_RING_CAPACITY = 0x400000;

// Read wait timeout in milliseconds.
// Controls latency/CPU tradeoff when no packets are available.
static constexpr DWORD READ_WAIT_TIMEOUT_MS = 100;

// Stable GUID for the Kirdi VPN adapter — survives adapter recreation.
static const GUID KIRDI_ADAPTER_GUID = {
    0x8b5c5e4a, 0x3c9f, 0x4e71,
    {0xb8, 0x42, 0xf1, 0x23, 0x45, 0x67, 0x89, 0xab}
};

// ── WinTUN Function Pointer Types ───────────────────────────────────────────
// Resolved from wintun.dll at runtime via GetProcAddress.

using WintunCreateAdapterFn        = void* (WINAPI*)(LPCWSTR, LPCWSTR, const GUID*);
using WintunCloseAdapterFn         = void  (WINAPI*)(void*);
using WintunStartSessionFn         = void* (WINAPI*)(void*, DWORD);
using WintunEndSessionFn           = void  (WINAPI*)(void*);
using WintunGetReadWaitEventFn     = HANDLE (WINAPI*)(void*);
using WintunReceivePacketFn        = BYTE* (WINAPI*)(void*, DWORD*);
using WintunReleaseReceivePacketFn = void  (WINAPI*)(void*, const BYTE*);
using WintunAllocateSendPacketFn   = BYTE* (WINAPI*)(void*, DWORD);
using WintunSendPacketFn           = void  (WINAPI*)(void*, const BYTE*);

// ── WinTUN Function Table ───────────────────────────────────────────────────

struct WindowsTunDevice::WintunFuncs {
    WintunCreateAdapterFn        create_adapter        = nullptr;
    WintunCloseAdapterFn         close_adapter         = nullptr;
    WintunStartSessionFn         start_session         = nullptr;
    WintunEndSessionFn           end_session           = nullptr;
    WintunGetReadWaitEventFn     get_read_wait_event   = nullptr;
    WintunReceivePacketFn        receive_packet        = nullptr;
    WintunReleaseReceivePacketFn release_receive_packet = nullptr;
    WintunAllocateSendPacketFn   allocate_send_packet  = nullptr;
    WintunSendPacketFn           send_packet           = nullptr;

    bool is_valid() const {
        return create_adapter && close_adapter
            && start_session && end_session
            && get_read_wait_event
            && receive_packet && release_receive_packet
            && allocate_send_packet && send_packet;
    }
};

// ── Helpers ─────────────────────────────────────────────────────────────────

// UTF-8 narrow string → UTF-16 wide string
static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    // MultiByteToWideChar takes int — guard against overflow on 64-bit
    if (s.size() > static_cast<size_t>(INT_MAX)) return {};
    int src_len = static_cast<int>(s.size());
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), src_len, nullptr, 0);
    if (len <= 0) return {};
    std::wstring ws(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), src_len, ws.data(), len);
    return ws;
}

// Format a Windows error code as a human-readable string
static std::string win_error_string(DWORD err) {
    char* msg = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&msg), 0, nullptr
    );
    std::string result;
    if (len > 0 && msg) {
        result.assign(msg, len);
        LocalFree(msg);
        // Strip trailing newline/whitespace
        while (!result.empty()
               && (result.back() == '\n' || result.back() == '\r'
                   || result.back() == ' ')) {
            result.pop_back();
        }
    } else {
        result = std::format("error code {}", err);
    }
    return result;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

// Defined out-of-line so unique_ptr<WintunFuncs> can see the complete type.
WindowsTunDevice::WindowsTunDevice() = default;

WindowsTunDevice::~WindowsTunDevice() {
    close();
    unload_wintun();
}

bool WindowsTunDevice::load_wintun() {
    auto dll = LoadLibraryA("wintun.dll");
    if (!dll) {
        LOG_ERRORF("Failed to load wintun.dll: {}", win_error_string(GetLastError()));
        return false;
    }
    wintun_dll_ = dll;

    funcs_ = std::make_unique<WintunFuncs>();

    // Resolve every exported function from wintun.dll.
    // Names match the official WinTUN API exactly.
    funcs_->create_adapter =
        reinterpret_cast<WintunCreateAdapterFn>(GetProcAddress(dll, "WintunCreateAdapter"));
    funcs_->close_adapter =
        reinterpret_cast<WintunCloseAdapterFn>(GetProcAddress(dll, "WintunCloseAdapter"));
    funcs_->start_session =
        reinterpret_cast<WintunStartSessionFn>(GetProcAddress(dll, "WintunStartSession"));
    funcs_->end_session =
        reinterpret_cast<WintunEndSessionFn>(GetProcAddress(dll, "WintunEndSession"));
    funcs_->get_read_wait_event =
        reinterpret_cast<WintunGetReadWaitEventFn>(GetProcAddress(dll, "WintunGetReadWaitEvent"));
    funcs_->receive_packet =
        reinterpret_cast<WintunReceivePacketFn>(GetProcAddress(dll, "WintunReceivePacket"));
    funcs_->release_receive_packet =
        reinterpret_cast<WintunReleaseReceivePacketFn>(GetProcAddress(dll, "WintunReleaseReceivePacket"));
    funcs_->allocate_send_packet =
        reinterpret_cast<WintunAllocateSendPacketFn>(GetProcAddress(dll, "WintunAllocateSendPacket"));
    funcs_->send_packet =
        reinterpret_cast<WintunSendPacketFn>(GetProcAddress(dll, "WintunSendPacket"));

    if (!funcs_->is_valid()) {
        LOG_ERROR("Failed to resolve all WinTUN functions — is wintun.dll version compatible?");
        unload_wintun();
        return false;
    }

    LOG_DEBUG("WinTUN API loaded successfully from wintun.dll");
    return true;
}

void WindowsTunDevice::unload_wintun() {
    funcs_.reset();

    if (wintun_dll_) {
        FreeLibrary(static_cast<HMODULE>(wintun_dll_));
        wintun_dll_ = nullptr;
    }
}

// ── Open / Close ────────────────────────────────────────────────────────────

std::expected<void, TunError> WindowsTunDevice::open(const TunConfig& config) {
    if (!load_wintun()) {
        return std::unexpected(TunError::DeviceNotFound);
    }

    // Determine adapter name (default "kirdi0")
    std::string adapter_name = config.name.empty() ? "kirdi0" : config.name;
    std::wstring wide_name = to_wide(adapter_name);
    std::wstring tunnel_type = L"Kirdi VPN";

    // Create the WinTUN adapter.
    // Uses a stable GUID so the adapter is reused across sessions.
    adapter_ = funcs_->create_adapter(wide_name.c_str(), tunnel_type.c_str(),
                                       &KIRDI_ADAPTER_GUID);
    if (!adapter_) {
        DWORD err = GetLastError();
        LOG_ERRORF("WintunCreateAdapter failed: {}", win_error_string(err));
        if (err == ERROR_ACCESS_DENIED) {
            return std::unexpected(TunError::PermissionDenied);
        }
        return std::unexpected(TunError::ConfigFailed);
    }

    if_name_ = adapter_name;
    mtu_ = config.mtu;
    LOG_INFOF("WinTUN adapter created: {}", if_name_);

    // Start session with ring buffer.
    session_ = funcs_->start_session(adapter_, WINTUN_RING_CAPACITY);
    if (!session_) {
        LOG_ERRORF("WintunStartSession failed: {}", win_error_string(GetLastError()));
        funcs_->close_adapter(adapter_);
        adapter_ = nullptr;
        return std::unexpected(TunError::ConfigFailed);
    }

    // Cache the read wait event handle for efficient polling.
    // This HANDLE is signaled when packets are available in the receive ring.
    read_event_ = funcs_->get_read_wait_event(session_);

    LOG_INFOF("WinTUN session started (ring capacity {} bytes)", WINTUN_RING_CAPACITY);

    // Configure IP address, netmask, MTU via netsh
    auto result = configure_address(config);
    if (!result) {
        close();
        return result;
    }

    return {};
}

void WindowsTunDevice::close() {
    if (session_ && funcs_) {
        LOG_INFOF("Ending WinTUN session: {}", if_name_);
        funcs_->end_session(session_);
    }
    session_ = nullptr;
    read_event_ = nullptr;  // Owned by session, not separately freed

    if (adapter_ && funcs_) {
        LOG_INFOF("Closing WinTUN adapter: {}", if_name_);
        funcs_->close_adapter(adapter_);
    }
    adapter_ = nullptr;
}

// ── Packet I/O ──────────────────────────────────────────────────────────────

std::expected<std::vector<uint8_t>, TunError> WindowsTunDevice::read_packet() {
    if (!session_ || !funcs_) {
        return std::unexpected(TunError::ReadFailed);
    }

    // First attempt — check if a packet is already in the ring buffer.
    DWORD packet_size = 0;
    BYTE* packet = funcs_->receive_packet(session_, &packet_size);

    if (!packet) {
        DWORD err = GetLastError();
        if (err == ERROR_NO_MORE_ITEMS) {
            // Ring empty — wait on the read event, then retry once.
            // This blocks the calling thread for up to READ_WAIT_TIMEOUT_MS,
            // providing the same latency/CPU tradeoff as poll() on POSIX.
            if (!read_event_) {
                return std::vector<uint8_t>{};
            }
            WaitForSingleObject(static_cast<HANDLE>(read_event_), READ_WAIT_TIMEOUT_MS);

            packet = funcs_->receive_packet(session_, &packet_size);
            if (!packet) {
                return std::vector<uint8_t>{};  // Still nothing — return empty
            }
        } else {
            LOG_ERRORF("WintunReceivePacket failed: {}", win_error_string(err));
            return std::unexpected(TunError::ReadFailed);
        }
    }

    // Copy data out of the ring buffer before releasing the slot.
    // WinTUN ring buffer pointers are only valid until release.
    std::vector<uint8_t> result(packet, packet + packet_size);
    funcs_->release_receive_packet(session_, packet);
    return result;
}

std::expected<size_t, TunError> WindowsTunDevice::write_packet(const uint8_t* data, size_t len) {
    if (!session_ || !funcs_) {
        return std::unexpected(TunError::WriteFailed);
    }

    if (len == 0) {
        return static_cast<size_t>(0);
    }

    // Guard against overflow — WinTUN uses DWORD for packet size.
    if (len > static_cast<size_t>(UINT32_MAX)) {
        LOG_ERROR("Packet too large for WinTUN send ring");
        return std::unexpected(TunError::WriteFailed);
    }

    // Allocate space in the send ring buffer.
    BYTE* send_buf = funcs_->allocate_send_packet(session_, static_cast<DWORD>(len));
    if (!send_buf) {
        DWORD err = GetLastError();
        if (err == ERROR_BUFFER_OVERFLOW) {
            LOG_WARN("WinTUN send ring full — dropping packet");
            return static_cast<size_t>(0);
        }
        LOG_ERRORF("WintunAllocateSendPacket failed: {}", win_error_string(err));
        return std::unexpected(TunError::WriteFailed);
    }

    // Copy payload into the ring buffer slot and commit.
    std::memcpy(send_buf, data, len);
    funcs_->send_packet(session_, send_buf);
    return len;
}

// ── IP Configuration ────────────────────────────────────────────────────────

// Validate that a string looks like a dotted-decimal IPv4 address.
// This is a safety check to prevent command injection in netsh calls —
// only digits and dots are allowed, with exactly 3 dots.
static bool is_safe_ipv4_string(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    int dots = 0;
    for (char c : s) {
        if (c == '.') { ++dots; continue; }
        if (c < '0' || c > '9') return false;
    }
    return dots == 3;
}

std::expected<void, TunError> WindowsTunDevice::configure_address(const TunConfig& config) {
    // Validate inputs before constructing shell commands
    if (!is_safe_ipv4_string(config.address)) {
        LOG_ERRORF("Invalid TUN address: '{}'", config.address);
        return std::unexpected(TunError::ConfigFailed);
    }
    if (!is_safe_ipv4_string(config.netmask)) {
        LOG_ERRORF("Invalid TUN netmask: '{}'", config.netmask);
        return std::unexpected(TunError::ConfigFailed);
    }

    // Set IPv4 address and netmask via netsh
    std::string cmd = "netsh interface ip set address name=\"" + if_name_
        + "\" static " + config.address + " " + config.netmask;

    LOG_DEBUGF("Running: {}", cmd);
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        LOG_ERRORF("netsh set address failed (exit code {})", ret);
        return std::unexpected(TunError::ConfigFailed);
    }

    // Set MTU via netsh (non-fatal if it fails — some Windows versions
    // don't support setting MTU on WinTUN adapters this way)
    cmd = "netsh interface ipv4 set subinterface \"" + if_name_
        + "\" mtu=" + std::to_string(config.mtu) + " store=active";

    LOG_DEBUGF("Running: {}", cmd);
    ret = std::system(cmd.c_str());
    if (ret != 0) {
        LOG_WARNF("netsh set mtu returned {} (non-fatal)", ret);
    }

    LOG_INFOF("WinTUN {} configured: addr={} mask={} mtu={}",
              if_name_, config.address, config.netmask, config.mtu);
    return {};
}

} // namespace kirdi::tun

#endif // KIRDI_PLATFORM_WINDOWS
