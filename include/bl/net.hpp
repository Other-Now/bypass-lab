#pragma once

// Raw-frame helpers for the two paths that see Ethernet frames instead of UDP
// payloads (AF_XDP and DPDK): find the UDP payload, and turn a received frame
// into the echo reply *in place* -- swap MACs, IPs and ports, overwrite the
// payload, fix the lengths and the IPv4 header checksum -- so the reply goes
// out of the same buffer the request arrived in, with no copy and no
// allocation.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "bl/wire.hpp"

namespace bl::net {

inline constexpr std::size_t kEthLen = 14;
inline constexpr std::size_t kIpMinLen = 20;
inline constexpr std::size_t kUdpLen = 8;
inline constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
inline constexpr std::uint8_t kProtoUdp = 17;

struct UdpView {
    std::size_t l3;       // offset of the IPv4 header
    std::size_t l4;       // offset of the UDP header
    std::size_t payload;  // offset of the UDP payload
    std::uint16_t payload_len;
    std::uint16_t dst_port;
};

// Returns false for anything that is not an unfragmented IPv4/UDP datagram that
// fits inside `len`. Every length the packet claims is checked against the
// bytes actually present.
inline bool parse_udp(const unsigned char* f, std::size_t len, UdpView& out) {
    if (len < kEthLen + kIpMinLen + kUdpLen) return false;
    if (wire::get_be16(f + 12) != kEtherTypeIpv4) return false;
    const unsigned char* ip = f + kEthLen;
    if ((ip[0] >> 4) != 4) return false;
    const std::size_t ihl = std::size_t(ip[0] & 0x0F) * 4;
    if (ihl < kIpMinLen || kEthLen + ihl + kUdpLen > len) return false;
    if (ip[9] != kProtoUdp) return false;
    if ((wire::get_be16(ip + 6) & 0x3FFF) != 0) return false;  // MF flag or frag offset
    const std::size_t ip_total = wire::get_be16(ip + 2);
    if (ip_total < ihl + kUdpLen || kEthLen + ip_total > len) return false;
    const unsigned char* udp = ip + ihl;
    const std::size_t udp_len = wire::get_be16(udp + 4);
    if (udp_len < kUdpLen || udp_len > ip_total - ihl) return false;
    out.l3 = kEthLen;
    out.l4 = kEthLen + ihl;
    out.payload = out.l4 + kUdpLen;
    out.payload_len = static_cast<std::uint16_t>(udp_len - kUdpLen);
    out.dst_port = wire::get_be16(udp + 2);
    return true;
}

inline std::uint16_t ipv4_checksum(const unsigned char* hdr, std::size_t len) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < len; i += 2) sum += wire::get_be16(hdr + i);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum);
}

// Rewrite frame `f` (already parsed into `v`) as the reply carrying `payload`.
// The IP header is rebuilt without options. Returns the new frame length. The
// caller guarantees the buffer has room (frames here are 2-4 KB buffers and the
// echo is 82 bytes). The UDP checksum is set to 0, which IPv4 permits.
inline std::size_t make_reply_inplace(unsigned char* f, const UdpView& v, const void* payload,
                                      std::size_t payload_len) {
    unsigned char mac[6];
    std::memcpy(mac, f, 6);
    std::memcpy(f, f + 6, 6);
    std::memcpy(f + 6, mac, 6);

    unsigned char* ip = f + kEthLen;
    unsigned char src[4], dst[4];
    std::memcpy(src, ip + 12, 4);
    std::memcpy(dst, ip + 16, 4);
    unsigned char* udp_old = f + v.l4;
    const std::uint16_t sport = wire::get_be16(udp_old);
    const std::uint16_t dport = wire::get_be16(udp_old + 2);

    ip[0] = 0x45;  // v4, no options
    ip[1] = 0;
    wire::put_be16(ip + 2, static_cast<std::uint16_t>(kIpMinLen + kUdpLen + payload_len));
    // ip[4..5] id: keep; flags/frag: DF, no fragment
    wire::put_be16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = kProtoUdp;
    std::memcpy(ip + 12, dst, 4);
    std::memcpy(ip + 16, src, 4);
    ip[10] = ip[11] = 0;
    wire::put_be16(ip + 10, ipv4_checksum(ip, kIpMinLen));

    unsigned char* udp = ip + kIpMinLen;
    wire::put_be16(udp, dport);
    wire::put_be16(udp + 2, sport);
    wire::put_be16(udp + 4, static_cast<std::uint16_t>(kUdpLen + payload_len));
    udp[6] = udp[7] = 0;
    std::memmove(udp + kUdpLen, payload, payload_len);
    return kEthLen + kIpMinLen + kUdpLen + payload_len;
}

}  // namespace bl::net
