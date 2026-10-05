// bl_sender: replay an ITCH capture as UDP at a fixed packet rate, and measure
// round-trip latency from the *scheduled* send time.
//
// Open loop: packet i is due at t0 + i/rate whether or not the previous one has
// gone out. If the sender (or the receiver, via backpressure) falls behind, the
// late packets are stamped with their original due time, so the backlog shows
// up in the latency instead of silently lowering the offered rate -- the
// coordinated-omission correction from tick2trade, applied to a network path.
//
// Threads:
//   tx    -- spins on the schedule, sendmmsg()s whatever is due (batching only
//            happens when it is already behind)
//   echo  -- busy-polls recvmmsg() on the same socket for the receiver's echoes
//            and records (seq, sched, send, rx) per echo
//
// Both ends of every latency sample are read on this host's clock, so no
// cross-host clock synchronisation is involved.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "bl/args.hpp"
#include "bl/clock.hpp"
#include "bl/feed.hpp"
#include "bl/wire.hpp"

namespace {

struct Sample {
    std::uint64_t seq;
    std::int64_t sched_ns;
    std::int64_t send_ns;
    std::int64_t rx_ns;
};

struct TxResult {
    std::uint64_t sent = 0;
    std::uint64_t errors = 0;
    std::uint64_t checksum = 0;  // sum of per-packet checksums actually sent
    std::int64_t first_send = 0;
    std::int64_t last_send = 0;
    std::int64_t lag_max = 0;
    std::uint64_t late_1us = 0, late_10us = 0, late_100us = 0;
};

void set_buf(int fd, int opt, int force_opt, int bytes) {
    if (setsockopt(fd, SOL_SOCKET, force_opt, &bytes, sizeof bytes) != 0)
        setsockopt(fd, SOL_SOCKET, opt, &bytes, sizeof bytes);
}

void usage() {
    std::fprintf(stderr,
                 "usage: bl_sender --dst IP --rate PPS [--seconds S | --count N]\n"
                 "                 [--feed FILE] [--max-msgs N] [--msgs-per-pkt K]\n"
                 "                 [--echo-every E] [--port P] [--src-port P]\n"
                 "                 [--tx-core C] [--echo-core C] [--batch B]\n"
                 "                 [--tx-threads T] [--grace-ms MS] [--warmup-ms MS]\n"
                 "                 [--out PREFIX] [--label TEXT]\n");
}

}  // namespace

int main(int argc, char** argv) try {
    const bl::Args a(argc, argv);
    if (!a.has("dst") || !a.has("rate")) {
        usage();
        return 2;
    }
    const std::string dst = a.str("dst");
    const double rate = a.f64("rate", 0);
    const auto port = static_cast<std::uint16_t>(a.i64("port", bl::wire::kDefaultPort));
    const auto src_port = static_cast<std::uint16_t>(a.i64("src-port", bl::wire::kDefaultSrcPort));
    const std::string feed_path = a.str("feed", "data/sample.NASDAQ_ITCH50");
    const auto mpp = static_cast<std::size_t>(a.i64("msgs-per-pkt", 1));
    const auto echo_every = static_cast<std::uint64_t>(a.i64("echo-every", 1));
    const int tx_core = static_cast<int>(a.i64("tx-core", -1));
    const int echo_core = static_cast<int>(a.i64("echo-core", -1));
    const auto batch = static_cast<unsigned>(std::max<std::int64_t>(1, a.i64("batch", 32)));
    const int tx_threads = static_cast<int>(std::max<std::int64_t>(1, a.i64("tx-threads", 1)));
    const std::int64_t grace_ns = a.i64("grace-ms", 300) * 1'000'000;
    const std::int64_t warmup_ns = a.i64("warmup-ms", 500) * 1'000'000;
    const std::string out = a.str("out");
    std::uint64_t count = static_cast<std::uint64_t>(a.i64("count", 0));
    if (count == 0) count = static_cast<std::uint64_t>(rate * a.f64("seconds", 5));
    if (rate <= 0 || count == 0) throw std::runtime_error("rate and count must be positive");
    if (tx_threads > 1 && echo_every != 0)
        throw std::runtime_error("--tx-threads > 1 is only for throughput runs (--echo-every 0)");

    const auto file = bl::feed::load(feed_path, static_cast<std::size_t>(a.i64("max-msgs", 0)));
    auto pool = bl::feed::pack(file, mpp, std::min<std::uint64_t>(count, 1u << 20));
    if (pool.size() == 0) throw std::runtime_error("feed produced no packets");
    std::fprintf(stderr, "feed: %zu messages -> pool of %zu packets (%zu msgs/pkt)\n",
                 file.frames.size(), pool.size(), mpp);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (inet_pton(AF_INET, dst.c_str(), &to.sin_addr) != 1) throw std::runtime_error("bad --dst");

    // One socket per tx thread, all bound to src_port (SO_REUSEPORT) so every
    // packet shares one 5-tuple and lands on one receive queue. Socket 0 also
    // receives the echoes.
    std::vector<int> fds;
    for (int t = 0; t < tx_threads; ++t) {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) throw std::runtime_error("socket");
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
        set_buf(fd, SO_SNDBUF, SO_SNDBUFFORCE, 8 << 20);
        set_buf(fd, SO_RCVBUF, SO_RCVBUFFORCE, 8 << 20);
        int busy = 50;
        setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy, sizeof busy);  // needs CAP_NET_ADMIN
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_port = htons(src_port);
        if (bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0)
            throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
        if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0)
            throw std::runtime_error(std::string("connect: ") + std::strerror(errno));
        fds.push_back(fd);
    }

    const double period = 1e9 / rate;
    const std::uint64_t echoes_expected = echo_every ? (count + echo_every - 1) / echo_every : 0;
    std::atomic<bool> tx_done{false};
    std::atomic<std::int64_t> tx_end_ns{0};
    std::vector<Sample> samples;
    samples.reserve(echoes_expected + 64);
    std::uint64_t echo_bad = 0;

    // Leave time for the echo thread to start spinning before the first packet.
    const std::int64_t t0 = bl::now_ns() + 20'000'000;

    std::thread echo_thr;
    if (echo_every) {
        echo_thr = std::thread([&] {
            bl::pin_this_thread(echo_core);
            constexpr unsigned kB = 64;
            unsigned char bufs[kB][64];
            iovec iov[kB];
            mmsghdr mm[kB];
            for (unsigned i = 0; i < kB; ++i) {
                iov[i] = {bufs[i], sizeof bufs[i]};
                std::memset(&mm[i], 0, sizeof mm[i]);
                mm[i].msg_hdr.msg_iov = &iov[i];
                mm[i].msg_hdr.msg_iovlen = 1;
            }
            for (;;) {
                const int n = recvmmsg(fds[0], mm, kB, MSG_DONTWAIT, nullptr);
                if (n > 0) {
                    const std::int64_t rx = bl::now_ns();
                    for (int i = 0; i < n; ++i) {
                        bl::wire::Echo e;
                        if (mm[i].msg_len < sizeof e) {
                            ++echo_bad;
                            continue;
                        }
                        std::memcpy(&e, bufs[i], sizeof e);
                        if (e.magic != bl::wire::kEchoMagic) {
                            ++echo_bad;
                            continue;
                        }
                        if (samples.size() < samples.capacity())
                            samples.push_back({e.seq, e.sched_ns, e.send_ns, rx});
                    }
                    if (samples.size() >= echoes_expected && tx_done.load(std::memory_order_acquire))
                        break;
                    continue;
                }
                if (tx_done.load(std::memory_order_acquire)) {
                    if (samples.size() >= echoes_expected) break;
                    if (bl::now_ns() > tx_end_ns.load() + grace_ns) break;
                }
                bl::cpu_relax();
            }
        });
    }

    std::vector<TxResult> res(static_cast<std::size_t>(tx_threads));
    std::vector<std::thread> tx;
    for (int t = 0; t < tx_threads; ++t) {
        tx.emplace_back([&, t] {
            bl::pin_this_thread(tx_core < 0 ? -1 : tx_core + t);
            TxResult& r = res[static_cast<std::size_t>(t)];
            std::vector<iovec> iov(batch);
            std::vector<mmsghdr> mm(batch);
            std::vector<std::uint64_t> slot(batch);
            for (auto& m : mm) std::memset(&m, 0, sizeof m);
            const std::uint64_t step = static_cast<std::uint64_t>(tx_threads);
            std::uint64_t i = static_cast<std::uint64_t>(t);
            while (i < count) {
                std::int64_t now = bl::now_ns();
                const std::int64_t due = t0 + static_cast<std::int64_t>(double(i) * period);
                if (now < due) continue;  // spin to the schedule
                const std::int64_t lag = now - due;
                r.lag_max = std::max(r.lag_max, lag);
                r.late_1us += lag > 1'000;
                r.late_10us += lag > 10'000;
                r.late_100us += lag > 100'000;

                unsigned k = 0;
                for (std::uint64_t j = i; k < batch && j < count; j += step) {
                    const std::int64_t dj = t0 + static_cast<std::int64_t>(double(j) * period);
                    if (dj > now) break;
                    const std::size_t s = j % pool.size();
                    unsigned char* p = &pool.buf[pool.off[s]];
                    bl::wire::FeedHeader h;
                    std::memcpy(&h, p, sizeof h);
                    h.seq = j;
                    h.flags = (echo_every && j % echo_every == 0) ? bl::wire::kWantEcho : 0;
                    h.sched_ns = dj;
                    h.send_ns = now;
                    std::memcpy(p, &h, sizeof h);
                    iov[k] = {p, pool.len[s]};
                    mm[k].msg_hdr.msg_iov = &iov[k];
                    mm[k].msg_hdr.msg_iovlen = 1;
                    slot[k] = s;
                    ++k;
                }
                const int sent = sendmmsg(fds[static_cast<std::size_t>(t)], mm.data(), k, 0);
                if (r.first_send == 0) r.first_send = now;
                if (sent < 0) {
                    // ENOBUFS / ECONNREFUSED (ICMP from a receiver not yet up):
                    // count the batch as not sent and move on -- the schedule
                    // does not wait.
                    r.errors += k;
                    i += step * k;
                    continue;
                }
                for (int q = 0; q < sent; ++q) r.checksum += pool.checksum[slot[static_cast<unsigned>(q)]];
                r.sent += static_cast<std::uint64_t>(sent);
                if (static_cast<unsigned>(sent) < k) r.errors += k - static_cast<unsigned>(sent);
                i += step * k;
                r.last_send = now;
            }
        });
    }
    for (auto& t : tx) t.join();
    tx_end_ns.store(bl::now_ns());
    tx_done.store(true, std::memory_order_release);

    TxResult tot;
    tot.first_send = res[0].first_send;
    for (const auto& r : res) {
        tot.sent += r.sent;
        tot.errors += r.errors;
        tot.checksum += r.checksum;
        tot.first_send = std::min(tot.first_send, r.first_send);
        tot.last_send = std::max(tot.last_send, r.last_send);
        tot.lag_max = std::max(tot.lag_max, r.lag_max);
        tot.late_1us += r.late_1us;
        tot.late_10us += r.late_10us;
        tot.late_100us += r.late_100us;
    }

    if (echo_thr.joinable()) echo_thr.join();

    // END marker, repeated because UDP may drop any single one.
    for (int k = 0; k < 5; ++k) {
        bl::wire::FeedHeader h{bl::wire::kFeedMagic, bl::wire::kEnd, 0, tot.sent, 0, 0};
        send(fds[0], &h, sizeof h, 0);
        usleep(2000);
    }

    const double tx_secs = tot.last_send > tot.first_send ? double(tot.last_send - tot.first_send) / 1e9 : 0;
    char json[1536];
    std::snprintf(
        json, sizeof json,
        "{\"label\":\"%s\",\"rate\":%.1f,\"count\":%llu,\"sent\":%llu,\"tx_errors\":%llu,"
        "\"achieved_pps\":%.1f,\"msgs_per_pkt\":%zu,\"echo_every\":%llu,\"echoes_expected\":%llu,"
        "\"echoes_received\":%zu,\"echo_bad\":%llu,\"expected_checksum\":\"%016llx\","
        "\"lag_max_ns\":%lld,\"late_1us\":%llu,\"late_10us\":%llu,\"late_100us\":%llu,"
        "\"tx_threads\":%d,\"t0_ns\":%lld,\"warmup_ns\":%lld}",
        a.str("label").c_str(), rate, (unsigned long long)count, (unsigned long long)tot.sent,
        (unsigned long long)tot.errors, tx_secs > 0 ? double(tot.sent - 1) / tx_secs : 0.0, mpp,
        (unsigned long long)echo_every, (unsigned long long)echoes_expected, samples.size(),
        (unsigned long long)echo_bad, (unsigned long long)tot.checksum, (long long)tot.lag_max,
        (unsigned long long)tot.late_1us, (unsigned long long)tot.late_10us,
        (unsigned long long)tot.late_100us, tx_threads, (long long)t0, (long long)warmup_ns);
    std::printf("%s\n", json);

    if (!out.empty()) {
        if (std::FILE* f = std::fopen((out + ".tx.json").c_str(), "w")) {
            std::fprintf(f, "%s\n", json);
            std::fclose(f);
        }
        if (echo_every) {
            if (std::FILE* f = std::fopen((out + ".samples.bin").c_str(), "wb")) {
                const char magic[8] = {'B', 'L', 'S', 'A', 'M', 'P', '0', '1'};
                const std::uint64_t n = samples.size();
                std::fwrite(magic, 1, 8, f);
                std::fwrite(&n, sizeof n, 1, f);
                std::fwrite(samples.data(), sizeof(Sample), samples.size(), f);
                std::fclose(f);
            }
        }
    }
    for (const int fd : fds) close(fd);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "bl_sender: %s\n", e.what());
    return 1;
}
