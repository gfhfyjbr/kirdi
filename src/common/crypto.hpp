#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <span>
#include <array>

namespace kirdi::crypto {

// ── HMAC-SHA256 Based Authentication ────────────────────────────────────────
//
// Auth flow:
//   1. Client sends AuthRequest with: user + HMAC-SHA256(timestamp + user, secret)
//   2. Server validates HMAC and timestamp (window +-30 sec to prevent replay)
//   3. Server responds with TUN config or error
//
// This prevents token replay and doesn't send the secret over the wire.
// ─────────────────────────────────────────────────────────────────────────────

// HMAC-SHA256 using OpenSSL
std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t> key,
                                     std::span<const uint8_t> data);

// Convenience: string inputs
std::array<uint8_t, 32> hmac_sha256(std::string_view key, std::string_view data);

// Hex encode/decode
std::string hex_encode(std::span<const uint8_t> data);
std::vector<uint8_t> hex_decode(std::string_view hex);

// Generate cryptographically random bytes
std::vector<uint8_t> random_bytes(size_t count);

// Generate random hex string (e.g. for secret paths)
std::string random_hex(size_t bytes);

// Build auth token: HMAC-SHA256(timestamp_hex + ":" + user, secret)
std::string build_auth_token(std::string_view user, std::string_view secret,
                              uint64_t timestamp);

// Verify auth token within a time window
bool verify_auth_token(std::string_view user, std::string_view secret,
                        std::string_view token, uint64_t timestamp,
                        uint64_t window_sec = 30);

} // namespace kirdi::crypto
