#include "common/crypto.hpp"
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace kirdi::crypto {

std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t> key,
                                     std::span<const uint8_t> data)
{
    std::array<uint8_t, 32> result;
    unsigned int len = 0;

    auto* ptr = HMAC(EVP_sha256(),
                     key.data(), static_cast<int>(key.size()),
                     data.data(), data.size(),
                     result.data(), &len);

    if (!ptr || len != 32) {
        throw std::runtime_error("HMAC-SHA256 failed");
    }
    return result;
}

std::array<uint8_t, 32> hmac_sha256(std::string_view key, std::string_view data) {
    return hmac_sha256(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(key.data()), key.size()),
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size())
    );
}

std::string hex_encode(std::span<const uint8_t> data) {
    std::string result;
    result.reserve(data.size() * 2);
    for (auto byte : data) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", byte);
        result += buf;
    }
    return result;
}

std::vector<uint8_t> hex_decode(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("Invalid hex string length");
    }
    std::vector<uint8_t> result(hex.size() / 2);
    for (size_t i = 0; i < result.size(); ++i) {
        auto byte_str = hex.substr(i * 2, 2);
        result[i] = static_cast<uint8_t>(
            std::strtoul(std::string(byte_str).c_str(), nullptr, 16)
        );
    }
    return result;
}

std::vector<uint8_t> random_bytes(size_t count) {
    std::vector<uint8_t> buf(count);
    if (RAND_bytes(buf.data(), static_cast<int>(count)) != 1) {
        throw std::runtime_error("CSPRNG failure in RAND_bytes");
    }
    return buf;
}

std::string random_hex(size_t bytes) {
    return hex_encode(random_bytes(bytes));
}

std::string build_auth_token(std::string_view user, std::string_view secret,
                              uint64_t timestamp)
{
    // Data = "timestamp_hex:user"
    char ts_buf[17];
    std::snprintf(ts_buf, sizeof(ts_buf), "%016llx",
                  static_cast<unsigned long long>(timestamp));
    std::string data = std::string(ts_buf) + ":" + std::string(user);
    auto mac = hmac_sha256(secret, data);
    return hex_encode(mac);
}

bool verify_auth_token(std::string_view user, std::string_view secret,
                        std::string_view token, uint64_t timestamp,
                        uint64_t window_sec)
{
    // Try current timestamp +- window
    for (uint64_t t = timestamp - window_sec; t <= timestamp + window_sec; ++t) {
        auto expected = build_auth_token(user, secret, t);
        // Constant-time comparison
        if (expected.size() == token.size() &&
            CRYPTO_memcmp(expected.data(), token.data(), expected.size()) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace kirdi::crypto
