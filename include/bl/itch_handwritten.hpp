#pragma once

// The baseline the generated decoder is measured against: a hand-written
// switch with hand-typed offsets, in the style of tick2trade's itch.hpp. It
// produces the same structs, so tests can require byte-for-byte agreement and
// the benchmark compares like with like.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "bl/itch.hpp"

namespace bl::itch::handwritten {

inline std::uint16_t be16(const unsigned char* p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}
inline std::uint32_t be32(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
inline std::uint64_t be64(const unsigned char* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}
inline std::uint64_t be48(const unsigned char* p) {
    return (std::uint64_t(be16(p)) << 32) | std::uint64_t(be32(p + 2));
}
inline std::uint64_t raw64(const unsigned char* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
inline std::uint32_t raw32(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline int wire_len(unsigned char t) {
    switch (t) {
        case 'S': return 12; case 'R': return 39; case 'H': return 25; case 'Y': return 20;
        case 'L': return 26; case 'V': return 35; case 'W': return 12; case 'K': return 28;
        case 'J': return 35; case 'h': return 21; case 'A': return 36; case 'F': return 40;
        case 'E': return 31; case 'C': return 36; case 'X': return 23; case 'D': return 19;
        case 'U': return 35; case 'P': return 44; case 'Q': return 40; case 'B': return 19;
        case 'I': return 50; case 'N': return 20; case 'O': return 48;
        default:  return 0;
    }
}

// Same forced inlining as the generated decoder (BL_INLINE), so the benchmark
// compares code generation, not the inliner's size heuristic -- without it GCC 13
// leaves this switch out of line (two calls per message) and it measured 3-8%
// slower for that reason alone.
template <class V>
BL_INLINE schema::Result dispatch(const unsigned char* m, std::size_t len, V& v) {
    const int want = wire_len(m[0]);
    if (want != static_cast<int>(len)) [[unlikely]]
        return want == 0 ? schema::Result::Unknown : schema::Result::BadLength;
    switch (m[0]) {
        case 'A':
            v(AddOrder{be16(m + 1), be48(m + 5), be64(m + 11), char(m[19]), be32(m + 20),
                       raw64(m + 24), be32(m + 32)});
            break;
        case 'F':
            v(AddOrderMPID{be16(m + 1), be48(m + 5), be64(m + 11), char(m[19]), be32(m + 20),
                           raw64(m + 24), be32(m + 32), raw32(m + 36)});
            break;
        case 'E':
            v(OrderExecuted{be16(m + 1), be48(m + 5), be64(m + 11), be32(m + 19), be64(m + 23)});
            break;
        case 'C':
            v(OrderExecutedPrice{be16(m + 1), be48(m + 5), be64(m + 11), be32(m + 19),
                                 be64(m + 23), char(m[31]), be32(m + 32)});
            break;
        case 'X':
            v(OrderCancel{be16(m + 1), be48(m + 5), be64(m + 11), be32(m + 19)});
            break;
        case 'D':
            v(OrderDelete{be16(m + 1), be48(m + 5), be64(m + 11)});
            break;
        case 'U':
            v(OrderReplace{be16(m + 1), be48(m + 5), be64(m + 11), be64(m + 19), be32(m + 27),
                           be32(m + 31)});
            break;
        case 'P':
            v(Trade{be16(m + 1), be48(m + 5), be64(m + 11), char(m[19]), be32(m + 20),
                    raw64(m + 24), be32(m + 32), be64(m + 36)});
            break;
        default:
            break;
    }
    return schema::Result::Ok;
}

}  // namespace bl::itch::handwritten
