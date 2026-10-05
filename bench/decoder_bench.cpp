// Generated decoder vs hand-written switch, on a real ITCH capture.
//
// Same visitor (the order-independent Checksum), same loop, same message
// array; only the dispatch differs:
//
//   framing   -- length-table check only, no decode (the floor)
//   handwritten -- switch with hand-typed offsets (tick2trade style)
//   gen-fold  -- Schema::dispatch: fold-expression compare chain
//   gen-table -- Schema::dispatch_table: 256-entry function-pointer table
//
// Each variant runs `passes` times over the whole feed; we report the best and
// median pass in ns/message and require all decoding variants to produce the
// same checksum.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "bl/feed.hpp"
#include "bl/itch.hpp"
#include "bl/itch_handwritten.hpp"

namespace {

struct Msg {
    const unsigned char* p;
    std::size_t len;
};

struct Framing {
    std::uint64_t ok = 0;
};

enum class Variant { Framing, Handwritten, GenFold, GenTable };

template <Variant V>
[[gnu::noinline]] std::uint64_t run_pass(const std::vector<Msg>& msgs) {
    if constexpr (V == Variant::Framing) {
        std::uint64_t ok = 0;
        for (const auto& m : msgs) ok += bl::itch::Schema::check(m.p, m.len) == bl::schema::Result::Ok;
        return ok;
    } else {
        bl::itch::Checksum cs;
        for (const auto& m : msgs) {
            if constexpr (V == Variant::Handwritten) bl::itch::handwritten::dispatch(m.p, m.len, cs);
            else if constexpr (V == Variant::GenFold) bl::itch::Schema::dispatch(m.p, m.len, cs);
            else bl::itch::Schema::dispatch_table(m.p, m.len, cs);
        }
        return cs.sum;
    }
}

using PassFn = std::uint64_t (*)(const std::vector<Msg>&);

struct Row {
    const char* name;
    PassFn fn;
    std::vector<double> ns;
    std::uint64_t sum = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/sample.NASDAQ_ITCH50";
    const std::size_t max_msgs = argc > 2 ? std::stoull(argv[2]) : 0;
    const int passes = argc > 3 ? std::stoi(argv[3]) : 15;

    const auto f = bl::feed::load(path, max_msgs);
    std::vector<Msg> msgs;
    msgs.reserve(f.frames.size());
    std::size_t types[256] = {};
    for (const auto& fr : f.frames) {
        msgs.push_back({&f.bytes[fr.off], fr.len});
        ++types[f.bytes[fr.off]];
    }
    std::size_t book = 0;
    for (char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P'}) book += types[static_cast<unsigned char>(t)];
    std::printf("feed %s: %zu messages, %.1f%% decoded (book-moving), %d passes\n\n", path.c_str(),
                msgs.size(), 100.0 * double(book) / double(msgs.size()), passes);

    // Interleaved: every round runs each variant once, so drift (frequency,
    // thermal, a noisy neighbour) lands on all variants instead of whichever
    // happened to run last. Round 0 is a discarded warm-up.
    std::vector<Row> rows{{"framing", &run_pass<Variant::Framing>, {}, 0},
                          {"handwritten", &run_pass<Variant::Handwritten>, {}, 0},
                          {"gen-fold", &run_pass<Variant::GenFold>, {}, 0},
                          {"gen-table", &run_pass<Variant::GenTable>, {}, 0}};
    for (int r = 0; r <= passes; ++r) {
        for (auto& row : rows) {
            const auto t0 = std::chrono::steady_clock::now();
            row.sum = row.fn(msgs);
            const auto t1 = std::chrono::steady_clock::now();
            if (r > 0)
                row.ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() /
                                 double(msgs.size()));
        }
    }
    for (auto& row : rows) {
        std::sort(row.ns.begin(), row.ns.end());
        std::printf("%-12s best %6.2f  median %6.2f  ns/msg\n", row.name, row.ns.front(),
                    row.ns[row.ns.size() / 2]);
    }
    const bool agree = rows[1].sum == rows[2].sum && rows[2].sum == rows[3].sum;
    std::printf("\nchecksums %s (%016llx)\n", agree ? "agree" : "DISAGREE",
                (unsigned long long)rows[1].sum);
    return agree ? 0 : 1;
}
