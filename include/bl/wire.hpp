#pragma once

// UDP payload format shared by the sender and all four receive paths.
//
//   +--------------------------- FeedHeader (32 B, host byte order) ----------+
//   | magic u32 | flags u16 | msg_count u16 | seq u64 | sched_ns i64 | send_ns i64 |
//   +-----------------------------------------------------------------------+
//   | [len u16 BE][ITCH message] x msg_count     (MoldUDP64 message block)  |
//
// The header is ours, not MoldUDP64's: it carries the two timestamps the
// latency measurement needs. Both ends are x86-64 Linux, so it is host order
// rather than paying for byte swaps on fields that never cross an
// architecture boundary. The message block is exactly MoldUDP64's.
//
// sched_ns is when the packet was *due* on the sender's open-loop schedule,
// send_ns when it actually left. Round-trip latency is measured on the sender's
// clock against sched_ns, so a sender or receiver that falls behind is charged
// for the backlog (no coordinated omission), and no cross-host clock sync is
// needed.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bl::wire {

inline constexpr std::uint32_t kFeedMagic = 0x42414C42;  // "BLAB"
inline constexpr std::uint32_t kEchoMagic = 0x4F484345;  // "ECHO"
inline constexpr std::uint16_t kDefaultPort = 9000;      // receiver
inline constexpr std::uint16_t kDefaultSrcPort = 9001;   // sender (echoes return here)

enum Flags : std::uint16_t {
    kWantEcho = 1u << 0,  // receiver must answer with an Echo
    kEnd = 1u << 1,       // end of run; seq carries the number of data packets sent
};

struct FeedHeader {
    std::uint32_t magic;
    std::uint16_t flags;
    std::uint16_t msg_count;
    std::uint64_t seq;
    std::int64_t sched_ns;
    std::int64_t send_ns;
};
static_assert(sizeof(FeedHeader) == 32);

// What the receiver sends back. Copies the sender's timestamps so the sender
// does not need per-seq state to compute latency.
struct Echo {
    std::uint32_t magic;
    std::uint16_t path;
    std::uint16_t reserved;
    std::uint64_t seq;
    std::int64_t sched_ns;
    std::int64_t send_ns;
    std::uint64_t checksum;  // receiver's running checksum, for spot checks
};
static_assert(sizeof(Echo) == 40);

// Receive paths, numbered for the Echo and for output files.
enum class Path : std::uint16_t { Recvmsg = 1, Recvmmsg = 2, AfXdp = 3, Dpdk = 4 };

inline const char* path_name(Path p) {
    switch (p) {
        case Path::Recvmsg: return "recvmsg";
        case Path::Recvmmsg: return "recvmmsg";
        case Path::AfXdp: return "afxdp";
        case Path::Dpdk: return "dpdk";
    }
    return "?";
}

inline std::uint16_t get_be16(const unsigned char* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline void put_be16(unsigned char* p, std::uint16_t v) {
    p[0] = static_cast<unsigned char>(v >> 8);
    p[1] = static_cast<unsigned char>(v);
}

}  // namespace bl::wire
