// Copied from tick2trade (github.com/Other-Now/tick2trade) so CI can generate
// a feed without the non-redistributable NASDAQ capture.
//
// Synthetic ITCH 5.0 feed generator, so the repo produces numbers on clone.
//
// The real benchmark data is a 214 MB NASDAQ TotalView-ITCH file that cannot be
// redistributed. This writes a smaller stream in the same BinaryFILE framing
// (2-byte big-endian length + message) with the same message mix, so
// `t2t_bench` runs immediately after cloning and the harness's own correctness
// checks stay meaningful.
//
// Two properties are deliberate:
//   * every Execute / Cancel / Delete / Replace references an order that is
//     actually live, so a correct pipeline reports ZERO unknown references;
//   * every message is written at its exact ITCH canonical length, so a correct
//     pipeline reports ZERO length mismatches.
// A generator that got either wrong would mask the very bugs those counters
// exist to catch.
//
// Output is deterministic for a given seed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

std::vector<unsigned char> g_out;

void put8(unsigned char v) { g_out.push_back(v); }
void put16(std::uint16_t v) { put8(v >> 8); put8(v & 0xFF); }
void put32(std::uint32_t v) { for (int i = 3; i >= 0; --i) put8((v >> (i * 8)) & 0xFF); }
void put48(std::uint64_t v) { for (int i = 5; i >= 0; --i) put8((v >> (i * 8)) & 0xFF); }
void put64(std::uint64_t v) { for (int i = 7; i >= 0; --i) put8((v >> (i * 8)) & 0xFF); }
void putsym(const std::string& s) {
    char b[8];
    std::memset(b, ' ', 8);
    std::memcpy(b, s.data(), s.size() < 8 ? s.size() : 8);
    for (int i = 0; i < 8; ++i) put8(static_cast<unsigned char>(b[i]));
}

// Each message is framed by its length; we back-fill it once the body is done.
std::size_t begin_msg() {
    g_out.push_back(0);
    g_out.push_back(0);
    return g_out.size();  // index of first body byte
}
void end_msg(std::size_t body_start, int expect) {
    const std::size_t len = g_out.size() - body_start;
    if (static_cast<int>(len) != expect) {
        std::fprintf(stderr, "internal error: message length %zu != canonical %d\n", len, expect);
        std::exit(2);
    }
    g_out[body_start - 2] = static_cast<unsigned char>((len >> 8) & 0xFF);
    g_out[body_start - 1] = static_cast<unsigned char>(len & 0xFF);
}

struct Live {
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint32_t price;
    std::uint16_t locate;
    bool          buy;
};

}  // namespace

int main(int argc, char** argv) {
    const char* path   = (argc > 1) ? argv[1] : "data/sample.NASDAQ_ITCH50";
    const std::size_t target = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 150000;
    const unsigned seed = (argc > 3) ? static_cast<unsigned>(std::atoi(argv[3])) : 7;

    std::mt19937_64 rng(seed);

    // A modest symbol universe with per-symbol reference prices, so books stay
    // shallow and near the touch the way a real feed behaves.
    const int kSymbols = 400;
    std::vector<std::string> syms;
    std::vector<std::uint32_t> ref_px;
    for (int i = 0; i < kSymbols; ++i) {
        std::string s;
        s += char('A' + (i / 676) % 26);
        s += char('A' + (i / 26) % 26);
        s += char('A' + i % 26);
        s += char('A' + (i * 7) % 26);
        syms.push_back(s);
        ref_px.push_back(100000u + std::uint32_t(i) * 2500u);  // $10.00 upward
    }

    std::vector<Live> live;
    live.reserve(65536);
    std::uint64_t next_ref = 1;
    std::uint64_t ts = 34200ull * 1000000000ull;  // 09:30:00 in ns since midnight
    std::uint64_t match_no = 1;

    std::uniform_int_distribution<int> pick_sym(0, kSymbols - 1);
    std::uniform_int_distribution<int> action(0, 99);
    std::uniform_int_distribution<int> lots(1, 40);
    std::uniform_int_distribution<int> tick_off(0, 3);

    g_out.reserve(target * 40);

    while (g_out.size() < target * 30 && live.size() + 1 < 1u << 20) {
        ts += 1000 + (rng() % 50000);
        const int a = action(rng);

        // Keep a working population of resting orders before leaning on the
        // lifecycle messages, otherwise the early stream is all adds.
        const bool force_add = live.size() < 24000;

        if (force_add || a < 38) {
            // --- Add Order (A), or with attribution (F) ---
            const int si = pick_sym(rng);
            const bool buy = (rng() & 1) != 0;
            const std::uint32_t px =
                buy ? ref_px[si] - std::uint32_t(tick_off(rng)) * 100u
                    : ref_px[si] + std::uint32_t(tick_off(rng)) * 100u;
            const std::uint32_t sh = std::uint32_t(lots(rng)) * 100u;
            const std::uint16_t locate = std::uint16_t(si + 1);
            const bool mpid = (a % 7) == 0;

            const std::size_t b = begin_msg();
            put8(mpid ? 'F' : 'A');
            put16(locate);
            put16(0);
            put48(ts);
            put64(next_ref);
            put8(buy ? 'B' : 'S');
            put32(sh);
            putsym(syms[si]);
            put32(px);
            if (mpid) { put8('M'); put8('P'); put8('I'); put8('D'); }
            end_msg(b, mpid ? 40 : 36);

            live.push_back(Live{next_ref, sh, px, locate, buy});
            ++next_ref;
            continue;
        }

        if (live.empty()) continue;
        const std::size_t idx = rng() % live.size();
        Live& o = live[idx];

        if (a < 55) {
            // --- Order Executed (E) / with price (C): partial or full ---
            const std::uint32_t ex = (o.shares > 100 && (rng() & 1))
                                         ? (std::uint32_t(1 + rng() % (o.shares / 100)) * 100u)
                                         : o.shares;
            const bool priced = (a % 5) == 0;
            const std::size_t b = begin_msg();
            put8(priced ? 'C' : 'E');
            put16(o.locate);
            put16(0);
            put48(ts);
            put64(o.ref);
            put32(ex);
            put64(match_no++);
            if (priced) { put8('Y'); put32(o.price); }
            end_msg(b, priced ? 36 : 31);

            o.shares -= ex;
            if (o.shares == 0) { live[idx] = live.back(); live.pop_back(); }
        } else if (a < 68) {
            // --- Order Cancel (X): partial reduction ---
            const std::uint32_t cx = (o.shares > 100)
                                         ? (std::uint32_t(1 + rng() % (o.shares / 100)) * 100u)
                                         : o.shares;
            const std::size_t b = begin_msg();
            put8('X');
            put16(o.locate);
            put16(0);
            put48(ts);
            put64(o.ref);
            put32(cx);
            end_msg(b, 23);

            o.shares -= cx;
            if (o.shares == 0) { live[idx] = live.back(); live.pop_back(); }
        } else if (a < 90) {
            // --- Order Delete (D) ---
            const std::size_t b = begin_msg();
            put8('D');
            put16(o.locate);
            put16(0);
            put48(ts);
            put64(o.ref);
            end_msg(b, 19);

            live[idx] = live.back();
            live.pop_back();
        } else if (a < 97) {
            // --- Order Replace (U): retire old ref, create a new one ---
            const std::uint32_t np =
                o.buy ? o.price - std::uint32_t(tick_off(rng)) * 100u
                      : o.price + std::uint32_t(tick_off(rng)) * 100u;
            const std::uint32_t ns = std::uint32_t(lots(rng)) * 100u;
            const std::size_t b = begin_msg();
            put8('U');
            put16(o.locate);
            put16(0);
            put48(ts);
            put64(o.ref);
            put64(next_ref);
            put32(ns);
            put32(np);
            end_msg(b, 35);

            o.ref = next_ref++;
            o.shares = ns;
            o.price = np;
        } else {
            // --- Trade, non-cross (P): no book effect, but it is in the mix ---
            const int si = pick_sym(rng);
            const std::size_t b = begin_msg();
            put8('P');
            put16(std::uint16_t(si + 1));
            put16(0);
            put48(ts);
            put64(next_ref++);
            put8('B');
            put32(std::uint32_t(lots(rng)) * 100u);
            putsym(syms[si]);
            put32(ref_px[si]);
            put64(match_no++);
            end_msg(b, 44);
        }
    }

    // Clean EOF: a zero-length frame, which the parser stops on.
    put16(0);

    std::FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror("fopen"); return 1; }
    std::fwrite(g_out.data(), 1, g_out.size(), f);
    std::fclose(f);

    std::printf("wrote %s  (%.2f MB, %zu live orders left resting)\n", path,
                double(g_out.size()) / (1024 * 1024), live.size());
    return 0;
}
