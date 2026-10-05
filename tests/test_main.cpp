// Unit tests. No framework: CHECK counts and reports, main returns non-zero on
// any failure. Pass a feed path as argv[1] to run the decoder-equivalence test
// on it (CI runs the synthetic sample; locally it also runs the real day).

#include <cstdio>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include "bl/feed.hpp"
#include "bl/handler.hpp"
#include "bl/itch.hpp"
#include "bl/itch_handwritten.hpp"
#include "bl/net.hpp"

namespace {

int g_checks = 0, g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

using namespace bl;
using Decoded = std::variant<itch::AddOrder, itch::AddOrderMPID, itch::OrderExecuted,
                             itch::OrderExecutedPrice, itch::OrderCancel, itch::OrderDelete,
                             itch::OrderReplace, itch::Trade>;

struct Recorder {
    std::vector<Decoded> out;
    template <class T>
    void operator()(const T& m) { out.emplace_back(m); }
};

// --- a tiny big-endian message writer ------------------------------------
struct W {
    std::vector<unsigned char> b;
    W& u8(std::uint64_t v) { b.push_back(static_cast<unsigned char>(v)); return *this; }
    W& be(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
        return *this;
    }
    W& raw(const char* s, int n) {
        for (int i = 0; i < n; ++i) b.push_back(static_cast<unsigned char>(s[i]));
        return *this;
    }
};

void test_length_table() {
    for (int t = 0; t < 256; ++t)
        CHECK(int(itch::Schema::lengths[static_cast<std::size_t>(t)]) ==
              itch::handwritten::wire_len(static_cast<unsigned char>(t)));
}

void test_known_values() {
    // Add Order: locate 7, tracking 0, ts 0x0102030405 06, ref 0x1122334455667788,
    // side 'S', shares 300, stock "MSFT    ", price 1234500.
    W w;
    w.u8('A').be(7, 2).be(0, 2).be(0x010203040506ull, 6).be(0x1122334455667788ull, 8).u8('S')
        .be(300, 4).raw("MSFT    ", 8).be(1234500, 4);
    CHECK(w.b.size() == 36);
    std::uint64_t sym;
    std::memcpy(&sym, "MSFT    ", 8);
    const itch::AddOrder want{7, 0x010203040506ull, 0x1122334455667788ull, 'S', 300, sym, 1234500};

    Recorder r1, r2, r3;
    CHECK(itch::Schema::dispatch(w.b.data(), w.b.size(), r1) == schema::Result::Ok);
    CHECK(itch::Schema::dispatch_table(w.b.data(), w.b.size(), r2) == schema::Result::Ok);
    CHECK(itch::handwritten::dispatch(w.b.data(), w.b.size(), r3) == schema::Result::Ok);
    CHECK(r1.out.size() == 1 && std::get<itch::AddOrder>(r1.out[0]) == want);
    CHECK(r2.out.size() == 1 && std::get<itch::AddOrder>(r2.out[0]) == want);
    CHECK(r3.out.size() == 1 && std::get<itch::AddOrder>(r3.out[0]) == want);

    // Replace: two 8-byte refs back to back, then shares and price.
    W u;
    u.u8('U').be(9, 2).be(0, 2).be(42, 6).be(100, 8).be(200, 8).be(500, 4).be(99, 4);
    CHECK(u.b.size() == 35);
    Recorder r4;
    itch::Schema::dispatch(u.b.data(), u.b.size(), r4);
    CHECK(r4.out.size() == 1 && std::get<itch::OrderReplace>(r4.out[0]) ==
                                    (itch::OrderReplace{9, 42, 100, 200, 500, 99}));

    // Wrong length and unknown type are reported, and the visitor is not called.
    Recorder r5;
    CHECK(itch::Schema::dispatch(w.b.data(), 35, r5) == schema::Result::BadLength);
    unsigned char z[4] = {'Z', 0, 0, 0};
    CHECK(itch::Schema::dispatch(z, 4, r5) == schema::Result::Unknown);
    CHECK(r5.out.empty());

    // A skipped type (system event) frames fine and decodes nothing.
    unsigned char s[12] = {'S'};
    CHECK(itch::Schema::dispatch(s, 12, r5) == schema::Result::Ok);
    CHECK(r5.out.empty());
}

// Generated (both dispatch strategies) and hand-written must agree on every
// message of a real feed.
void test_feed_equivalence(const std::string& path) {
    feed::File f;
    try {
        f = feed::load(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "skip feed equivalence: %s\n", e.what());
        return;
    }
    Recorder a, b, c;
    std::size_t unknown = 0, badlen = 0, mismatch = 0;
    for (const auto& fr : f.frames) {
        const unsigned char* m = &f.bytes[fr.off];
        const auto ra = itch::Schema::dispatch(m, fr.len, a);
        const auto rb = itch::Schema::dispatch_table(m, fr.len, b);
        const auto rc = itch::handwritten::dispatch(m, fr.len, c);
        mismatch += !(ra == rb && rb == rc);
        unknown += ra == schema::Result::Unknown;
        badlen += ra == schema::Result::BadLength;
    }
    CHECK(mismatch == 0);
    CHECK(unknown == 0);
    CHECK(badlen == 0);
    CHECK(a.out.size() == b.out.size() && b.out.size() == c.out.size());
    std::size_t diff = 0;
    for (std::size_t i = 0; i < a.out.size() && i < c.out.size(); ++i)
        diff += !(a.out[i] == b.out[i] && a.out[i] == c.out[i]);
    CHECK(diff == 0);
    std::printf("feed equivalence: %s: %zu messages, %zu decoded, %zu mismatches\n", path.c_str(),
                f.frames.size(), a.out.size(), diff);
}

std::vector<unsigned char> make_frame(const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> f(net::kEthLen + net::kIpMinLen + net::kUdpLen + payload.size());
    const unsigned char dmac[6] = {0x02, 0, 0, 0, 0, 0x02}, smac[6] = {0x02, 0, 0, 0, 0, 0x01};
    std::memcpy(&f[0], dmac, 6);
    std::memcpy(&f[6], smac, 6);
    wire::put_be16(&f[12], 0x0800);
    unsigned char* ip = &f[14];
    ip[0] = 0x45;
    wire::put_be16(ip + 2, static_cast<std::uint16_t>(28 + payload.size()));
    ip[8] = 64;
    ip[9] = 17;
    const unsigned char src[4] = {10, 0, 0, 1}, dst[4] = {10, 0, 0, 2};
    std::memcpy(ip + 12, src, 4);
    std::memcpy(ip + 16, dst, 4);
    wire::put_be16(ip + 10, net::ipv4_checksum(ip, 20));
    unsigned char* udp = ip + 20;
    wire::put_be16(udp, 9001);
    wire::put_be16(udp + 2, 9000);
    wire::put_be16(udp + 4, static_cast<std::uint16_t>(8 + payload.size()));
    std::memcpy(udp + 8, payload.data(), payload.size());
    return f;
}

void test_net() {
    std::vector<unsigned char> pl(64, 0xAB);
    auto f = make_frame(pl);
    net::UdpView v;
    CHECK(net::parse_udp(f.data(), f.size(), v));
    CHECK(v.dst_port == 9000 && v.payload_len == 64 && v.payload == 42);
    CHECK(net::ipv4_checksum(&f[14], 20) == 0);  // a valid header sums to zero

    // Ethernet pads short frames; trailing bytes beyond the IP length are fine.
    auto padded = f;
    padded.resize(f.size() + 6, 0);
    CHECK(net::parse_udp(padded.data(), padded.size(), v));

    wire::Echo e{wire::kEchoMagic, 3, 0, 77, 1, 2, 3};
    std::vector<unsigned char> g = f;
    g.resize(2048);
    const std::size_t n = net::make_reply_inplace(g.data(), v, &e, sizeof e);
    CHECK(n == 42 + sizeof e);
    net::UdpView r;
    CHECK(net::parse_udp(g.data(), n, r));
    CHECK(r.dst_port == 9001 && r.payload_len == sizeof e);
    CHECK(wire::get_be16(&g[34]) == 9000);                     // source port swapped
    CHECK(g[14 + 12] == 10 && g[14 + 15] == 2);                // src ip = old dst
    CHECK(g[14 + 16] == 10 && g[14 + 19] == 1);                // dst ip = old src
    CHECK(g[5] == 0x01 && g[11] == 0x02);                      // MACs swapped
    CHECK(net::ipv4_checksum(&g[14], 20) == 0);
    wire::Echo back;
    std::memcpy(&back, &g[42], sizeof back);
    CHECK(back.seq == 77 && back.path == 3);

    // Rejections.
    CHECK(!net::parse_udp(f.data(), 41, v));                   // truncated
    auto bad = f;
    bad[14 + 9] = 6;                                            // TCP
    CHECK(!net::parse_udp(bad.data(), bad.size(), v));
    bad = f;
    wire::put_be16(&bad[14 + 6], 0x2000);                       // more-fragments
    CHECK(!net::parse_udp(bad.data(), bad.size(), v));
    bad = f;
    bad[14] = 0x44;                                             // IHL 16 bytes
    CHECK(!net::parse_udp(bad.data(), bad.size(), v));
    bad = f;
    wire::put_be16(&bad[14 + 2], 2000);                         // IP length beyond frame
    CHECK(!net::parse_udp(bad.data(), bad.size(), v));
    bad = f;
    wire::put_be16(&bad[38], 500);                              // UDP length beyond IP
    CHECK(!net::parse_udp(bad.data(), bad.size(), v));
    bad = f;
    wire::put_be16(&bad[12], 0x86DD);                           // IPv6
    CHECK(!net::parse_udp(bad.data(), bad.size(), v));
}

std::vector<unsigned char> packet(std::uint64_t seq, std::uint16_t flags,
                                  const std::vector<std::vector<unsigned char>>& msgs) {
    std::vector<unsigned char> p(sizeof(wire::FeedHeader));
    wire::FeedHeader h{wire::kFeedMagic, flags, static_cast<std::uint16_t>(msgs.size()), seq, 11, 22};
    std::memcpy(p.data(), &h, sizeof h);
    for (const auto& m : msgs) {
        p.push_back(static_cast<unsigned char>(m.size() >> 8));
        p.push_back(static_cast<unsigned char>(m.size()));
        p.insert(p.end(), m.begin(), m.end());
    }
    return p;
}

void test_handler() {
    W d;
    d.u8('D').be(1, 2).be(0, 2).be(5, 6).be(123, 8);
    FeedHandler h(wire::Path::Recvmsg);
    wire::Echo e{};

    auto p0 = packet(10, wire::kWantEcho, {d.b, d.b});
    CHECK(h.on_payload(p0.data(), p0.size(), 1000, e));
    CHECK(e.seq == 10 && e.sched_ns == 11 && e.send_ns == 22 && e.magic == wire::kEchoMagic);
    auto p1 = packet(11, 0, {d.b});
    CHECK(!h.on_payload(p1.data(), p1.size(), 1001, e));
    auto p2 = packet(15, 0, {d.b});  // 12,13,14 missing
    h.on_payload(p2.data(), p2.size(), 1002, e);
    auto p3 = packet(13, 0, {d.b});  // arrives late
    h.on_payload(p3.data(), p3.size(), 1003, e);
    CHECK(h.stats().packets == 4);
    CHECK(h.stats().gaps == 3);
    CHECK(h.stats().late == 1);
    CHECK(h.stats().messages == 5);
    CHECK(h.decoded() == 5);
    CHECK(h.stats().first_seq == 10 && h.stats().last_seq == 15);

    // Message block overruns the datagram.
    auto bad = packet(16, 0, {d.b});
    bad.resize(bad.size() - 3);
    h.on_payload(bad.data(), bad.size(), 1004, e);
    CHECK(h.stats().malformed == 1);
    // Too short for a header / wrong magic.
    h.on_payload(bad.data(), 10, 1005, e);
    auto wrong = p1;
    wrong[0] ^= 0xFF;
    h.on_payload(wrong.data(), wrong.size(), 1006, e);
    CHECK(h.stats().malformed == 3);

    CHECK(!h.done());
    auto end = packet(99, wire::kEnd, {});
    h.on_payload(end.data(), end.size(), 1007, e);
    CHECK(h.done() && h.stats().sender_sent == 99);
}

// The sender predicts the receiver's checksum from the packets it sends; that
// prediction must equal what the handler computes, in any arrival order.
void test_predicted_checksum(const std::string& path) {
    feed::File f;
    try {
        f = feed::load(path, 200000);
    } catch (...) {
        return;
    }
    for (std::size_t mpp : {1u, 3u, 8u}) {
        auto pool = feed::pack(f, mpp, 1u << 20);
        std::uint64_t predicted = 0;
        for (auto c : pool.checksum) predicted += c;
        FeedHandler h(wire::Path::Recvmmsg);
        wire::Echo e;
        for (std::size_t i = pool.size(); i-- > 0;) {  // reverse order on purpose
            h.on_payload(&pool.buf[pool.off[i]], pool.len[i], 0, e);
        }
        CHECK(h.checksum() == predicted);
        CHECK(h.stats().malformed == 0 && h.stats().unknown == 0 && h.stats().bad_length == 0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string feed_path = argc > 1 ? argv[1] : "data/sample.NASDAQ_ITCH50";
    test_length_table();
    test_known_values();
    test_net();
    test_handler();
    test_feed_equivalence(feed_path);
    test_predicted_checksum(feed_path);
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
