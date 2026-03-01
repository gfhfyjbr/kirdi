#include <gtest/gtest.h>
#include "common/protocol.hpp"

using namespace kirdi::protocol;

TEST(Protocol, HeaderSerializeDeserialize) {
    PacketHeader hdr{MsgType::IpPacket, 1500};

    std::array<uint8_t, HEADER_SIZE> buf{};
    serialize_header(buf, hdr);

    auto parsed = deserialize_header(buf);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, MsgType::IpPacket);
    EXPECT_EQ(parsed->length, 1500u);
}

TEST(Protocol, HeaderTooSmall) {
    std::array<uint8_t, 3> buf{};
    auto parsed = deserialize_header(buf);
    EXPECT_FALSE(parsed.has_value());
}

TEST(Protocol, HeaderPayloadTooLarge) {
    std::array<uint8_t, HEADER_SIZE> buf{};
    PacketHeader hdr{MsgType::IpPacket, MAX_PAYLOAD + 1};
    serialize_header(buf, hdr);

    auto parsed = deserialize_header(buf);
    EXPECT_FALSE(parsed.has_value());
}

TEST(Protocol, BuildKeepalive) {
    auto pkt = build_keepalive();
    ASSERT_EQ(pkt.size(), HEADER_SIZE);

    auto hdr = deserialize_header(pkt);
    ASSERT_TRUE(hdr.has_value());
    EXPECT_EQ(hdr->type, MsgType::Keepalive);
    EXPECT_EQ(hdr->length, 0u);
}

TEST(Protocol, BuildIpPacket) {
    // Fake IPv4 packet header (20 bytes minimum)
    std::array<uint8_t, 20> fake_ip{};
    fake_ip[0] = 0x45;  // IPv4, IHL=5
    fake_ip[9] = PROTO_TCP;
    // Source: 10.0.0.1
    fake_ip[12] = 10; fake_ip[13] = 0; fake_ip[14] = 0; fake_ip[15] = 1;
    // Dest: 10.0.0.2
    fake_ip[16] = 10; fake_ip[17] = 0; fake_ip[18] = 0; fake_ip[19] = 2;

    auto pkt = build_ip_packet(fake_ip);
    ASSERT_EQ(pkt.size(), HEADER_SIZE + 20);

    auto hdr = deserialize_header(pkt);
    ASSERT_TRUE(hdr.has_value());
    EXPECT_EQ(hdr->type, MsgType::IpPacket);
    EXPECT_EQ(hdr->length, 20u);

    // Verify IP helpers
    auto payload = std::span<const uint8_t>(pkt.data() + HEADER_SIZE, 20);
    EXPECT_EQ(ip_version(payload), 4);
    EXPECT_EQ(ip4_protocol(payload), PROTO_TCP);
}

TEST(Protocol, IpVersion) {
    uint8_t ipv4_pkt[] = {0x45, 0x00};
    EXPECT_EQ(ip_version({ipv4_pkt, 2}), 4);

    uint8_t ipv6_pkt[] = {0x60, 0x00};
    EXPECT_EQ(ip_version({ipv6_pkt, 2}), 6);

    EXPECT_EQ(ip_version({}), 0);
}
