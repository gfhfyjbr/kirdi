#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include <stdexcept>
#include <array>

namespace kirdi::protocol {

// ── Wire Protocol ───────────────────────────────────────────────────────────
//
// All messages are binary WebSocket frames with the following structure:
//
//   [1 byte: msg_type] [4 bytes: payload_length (big-endian)] [payload]
//
// Total header: 5 bytes. Max payload: 65535 bytes (fits in single WS frame).
// IP packets are passed raw (no additional framing inside payload).
//
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t HEADER_SIZE     = 5;
static constexpr size_t MAX_PAYLOAD     = 65535;
static constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + MAX_PAYLOAD;
static constexpr size_t MTU             = 1500;

// ── Message Types ───────────────────────────────────────────────────────────

enum class MsgType : uint8_t {
    // Authentication
    AuthRequest     = 0x01,  // Client → Server: { "user": "...", "token": "..." }
    AuthResponse    = 0x02,  // Server → Client: { "ok": true, "tun_ip": "10.0.0.2", "tun_mask": "..." }

    // Tunnel data
    IpPacket        = 0x03,  // Bidirectional: raw IP packet (IPv4 or IPv6)

    // Control
    Keepalive       = 0x04,  // Bidirectional: empty payload
    TunConfig       = 0x05,  // Server → Client: TUN configuration JSON
    Disconnect      = 0x06,  // Either side: graceful disconnect

    // Diagnostics
    Ping            = 0x07,  // Client → Server: timestamp payload
    Pong            = 0x08,  // Server → Client: echo timestamp
};

// ── Packet Header ───────────────────────────────────────────────────────────

struct PacketHeader {
    MsgType  type;
    uint32_t length;  // payload length only (not including header)
};

// ── Serialization ───────────────────────────────────────────────────────────

// Serialize header into first 5 bytes of `buf`. Returns bytes written (always 5).
inline size_t serialize_header(std::span<uint8_t> buf, const PacketHeader& hdr) {
    if (buf.size() < HEADER_SIZE) {
        throw std::runtime_error("Buffer too small for header");
    }
    buf[0] = static_cast<uint8_t>(hdr.type);
    buf[1] = static_cast<uint8_t>((hdr.length >> 24) & 0xFF);
    buf[2] = static_cast<uint8_t>((hdr.length >> 16) & 0xFF);
    buf[3] = static_cast<uint8_t>((hdr.length >> 8)  & 0xFF);
    buf[4] = static_cast<uint8_t>((hdr.length)       & 0xFF);
    return HEADER_SIZE;
}

// Deserialize header from first 5 bytes.
inline std::optional<PacketHeader> deserialize_header(std::span<const uint8_t> buf) {
    if (buf.size() < HEADER_SIZE) return std::nullopt;

    PacketHeader hdr;
    hdr.type = static_cast<MsgType>(buf[0]);
    hdr.length = (static_cast<uint32_t>(buf[1]) << 24)
               | (static_cast<uint32_t>(buf[2]) << 16)
               | (static_cast<uint32_t>(buf[3]) << 8)
               | (static_cast<uint32_t>(buf[4]));

    if (hdr.length > MAX_PAYLOAD) return std::nullopt;
    return hdr;
}

// ── Full Packet Builder ─────────────────────────────────────────────────────

// Build a complete wire packet: [header][payload]
inline std::vector<uint8_t> build_packet(MsgType type, std::span<const uint8_t> payload) {
    if (payload.size() > MAX_PAYLOAD) {
        throw std::runtime_error("Payload exceeds MAX_PAYLOAD");
    }
    std::vector<uint8_t> pkt(HEADER_SIZE + payload.size());
    PacketHeader hdr{type, static_cast<uint32_t>(payload.size())};
    serialize_header(pkt, hdr);
    std::memcpy(pkt.data() + HEADER_SIZE, payload.data(), payload.size());
    return pkt;
}

// Build keepalive (no payload)
inline std::vector<uint8_t> build_keepalive() {
    return build_packet(MsgType::Keepalive, {});
}

// Build IP packet message (wraps raw IP packet)
inline std::vector<uint8_t> build_ip_packet(std::span<const uint8_t> ip_data) {
    return build_packet(MsgType::IpPacket, ip_data);
}

// ── IP Header Inspection ────────────────────────────────────────────────────
// Quick utilities for inspecting IP packets without full parsing.

inline uint8_t ip_version(std::span<const uint8_t> pkt) {
    if (pkt.empty()) return 0;
    return (pkt[0] >> 4) & 0x0F;
}

inline uint8_t ip4_protocol(std::span<const uint8_t> pkt) {
    if (pkt.size() < 20) return 0;
    return pkt[9];
}

// IP protocol numbers
static constexpr uint8_t PROTO_ICMP = 1;
static constexpr uint8_t PROTO_TCP  = 6;
static constexpr uint8_t PROTO_UDP  = 17;

// Extract source/dest IP from IPv4 header (network byte order)
inline uint32_t ip4_src(std::span<const uint8_t> pkt) {
    if (pkt.size() < 20) return 0;
    uint32_t addr;
    std::memcpy(&addr, pkt.data() + 12, 4);
    return addr;
}

inline uint32_t ip4_dst(std::span<const uint8_t> pkt) {
    if (pkt.size() < 20) return 0;
    uint32_t addr;
    std::memcpy(&addr, pkt.data() + 16, 4);
    return addr;
}

} // namespace kirdi::protocol
