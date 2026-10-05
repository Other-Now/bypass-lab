// Path 3: AF_XDP.
//
// The XDP program (src/bpf/xdp_udp_redirect.bpf.c) runs in the driver's RX
// path, before an skb is allocated, and redirects feed packets into a ring
// shared with this process. No socket buffer, no protocol stack, no copy in
// zero-copy mode: the NIC DMAs into UMEM, a region this process allocated, and
// we read the frame where it landed.
//
// Frame lifecycle (all four rings are single-producer/single-consumer):
//
//   free stack --> FILL ring --> NIC/kernel --> RX ring --> [handler]
//        ^                                                 |       |
//        |                         no echo: back to free --+       | echo:
//        +---- COMPLETION ring <-- kernel/NIC <-- TX ring <--------+ rewritten in place
//
// The echo is the received frame turned around in place (net.hpp), so a
// request/reply round trip touches exactly one UMEM frame and copies nothing.
//
// With --umem-huge the UMEM is backed by 2 MB huge pages: 16 MB of frames is
// then 8 TLB entries instead of 4096. That is the huge-page step of the tuning
// ladder for this path.

#include "rx_paths.hpp"

#ifdef BL_HAVE_XDP

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xdp/xsk.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bl/clock.hpp"
#include "bl/net.hpp"

#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif

namespace bl::rx {

namespace {
constexpr std::uint32_t kFrames = 4096;
constexpr std::uint32_t kFrameSize = XSK_UMEM__DEFAULT_FRAME_SIZE;  // 4096
constexpr std::uint32_t kRing = XSK_RING_CONS__DEFAULT_NUM_DESCS;   // 2048
constexpr std::uint32_t kBatch = 64;
}  // namespace

int run_afxdp(const Config& c, FeedHandler& h, std::string& extra) {
    if (c.iface.empty()) {
        std::fprintf(stderr, "afxdp: --iface is required\n");
        return 2;
    }
    const unsigned ifindex = if_nametoindex(c.iface.c_str());
    if (!ifindex) {
        std::fprintf(stderr, "afxdp: no such interface %s\n", c.iface.c_str());
        return 2;
    }

    // --- UMEM -------------------------------------------------------------
    const std::size_t umem_bytes = std::size_t(kFrames) * kFrameSize;
    int mflags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;
    if (c.umem_huge) mflags |= MAP_HUGETLB;
    void* area = mmap(nullptr, umem_bytes, PROT_READ | PROT_WRITE, mflags, -1, 0);
    if (area == MAP_FAILED) {
        std::fprintf(stderr, "afxdp: mmap UMEM (%s pages) failed: %s\n",
                     c.umem_huge ? "huge" : "4K", std::strerror(errno));
        return 1;
    }
    xsk_ring_prod fq{};
    xsk_ring_cons cq{};
    xsk_umem* umem = nullptr;
    xsk_umem_config ucfg{};
    ucfg.fill_size = kRing;
    ucfg.comp_size = kRing;
    ucfg.frame_size = kFrameSize;
    ucfg.frame_headroom = 0;
    ucfg.flags = 0;
    if (int e = xsk_umem__create(&umem, area, umem_bytes, &fq, &cq, &ucfg); e) {
        std::fprintf(stderr, "afxdp: xsk_umem__create: %s\n", std::strerror(-e));
        return 1;
    }

    // --- XDP program ------------------------------------------------------
    bpf_object* obj = bpf_object__open_file(c.bpf_obj.c_str(), nullptr);
    if (!obj || bpf_object__load(obj)) {
        std::fprintf(stderr, "afxdp: cannot load BPF object %s\n", c.bpf_obj.c_str());
        return 1;
    }
    bpf_program* prog = bpf_object__find_program_by_name(obj, "xdp_udp_redirect");
    const int prog_fd = prog ? bpf_program__fd(prog) : -1;
    const int xsks_fd = bpf_object__find_map_fd_by_name(obj, "xsks_map");
    const int cfg_fd = bpf_object__find_map_fd_by_name(obj, "cfg_map");
    if (prog_fd < 0 || xsks_fd < 0 || cfg_fd < 0) {
        std::fprintf(stderr, "afxdp: BPF object is missing the program or maps\n");
        return 1;
    }
    {
        std::uint32_t k = 0, port = c.port;
        bpf_map_update_elem(cfg_fd, &k, &port, BPF_ANY);
    }
    const std::uint32_t xdp_flags = (c.xdp_mode == "skb") ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE;
    bpf_xdp_detach(static_cast<int>(ifindex), xdp_flags, nullptr);  // stale program from a crashed run
    if (int e = bpf_xdp_attach(static_cast<int>(ifindex), prog_fd, xdp_flags, nullptr); e) {
        std::fprintf(stderr, "afxdp: attach in %s mode failed: %s\n", c.xdp_mode.c_str(),
                     std::strerror(-e));
        return 1;
    }

    // --- socket -----------------------------------------------------------
    xsk_ring_cons rxr{};
    xsk_ring_prod txr{};
    xsk_socket* xsk = nullptr;
    auto make_socket = [&](std::uint16_t copy_flag) {
        xsk_socket_config scfg{};
        scfg.rx_size = kRing;
        scfg.tx_size = kRing;
        scfg.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;
        scfg.xdp_flags = xdp_flags;
        scfg.bind_flags = static_cast<std::uint16_t>(XDP_USE_NEED_WAKEUP | copy_flag);
        return xsk_socket__create(&xsk, c.iface.c_str(), static_cast<std::uint32_t>(c.queue), umem,
                                  &rxr, &txr, &scfg);
    };
    bool zerocopy = false;
    int e = -EINVAL;
    if (c.bind_mode != "copy") {
        e = make_socket(XDP_ZEROCOPY);
        zerocopy = e == 0;
    }
    if (e && c.bind_mode != "zerocopy") e = make_socket(XDP_COPY);
    if (e) {
        std::fprintf(stderr, "afxdp: xsk_socket__create: %s\n", std::strerror(-e));
        bpf_xdp_detach(static_cast<int>(ifindex), xdp_flags, nullptr);
        return 1;
    }
    if (int r = xsk_socket__update_xskmap(xsk, xsks_fd); r) {
        std::fprintf(stderr, "afxdp: update xskmap: %s\n", std::strerror(-r));
        return 1;
    }
    const int xfd = xsk_socket__fd(xsk);
    bool busy_ok = false;
    if (c.xdp_busy_poll) {
        int one = 1, us = 20, budget = static_cast<int>(kBatch);
        busy_ok = setsockopt(xfd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof one) == 0 &&
                  setsockopt(xfd, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof us) == 0 &&
                  setsockopt(xfd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof budget) == 0;
        if (!busy_ok) std::fprintf(stderr, "warning: AF_XDP busy-poll setup failed\n");
    }

    // --- frames -----------------------------------------------------------
    std::vector<std::uint64_t> free_frames;
    free_frames.reserve(kFrames);
    for (std::uint32_t i = 0; i < kFrames; ++i) free_frames.push_back(std::uint64_t(i) * kFrameSize);
    auto refill = [&] {
        if (free_frames.empty()) return;
        const std::uint32_t want = xsk_prod_nb_free(&fq, static_cast<std::uint32_t>(free_frames.size()));
        const std::uint32_t n = std::min<std::uint32_t>(want, static_cast<std::uint32_t>(free_frames.size()));
        if (!n) return;
        std::uint32_t idx = 0;
        if (xsk_ring_prod__reserve(&fq, n, &idx) != n) return;
        for (std::uint32_t i = 0; i < n; ++i) {
            *xsk_ring_prod__fill_addr(&fq, idx + i) = free_frames.back();
            free_frames.pop_back();
        }
        xsk_ring_prod__submit(&fq, n);
    };
    refill();

    std::fprintf(stderr, "READY afxdp iface=%s queue=%d mode=%s %s umem=%s busy_poll=%d\n",
                 c.iface.c_str(), c.queue, c.xdp_mode.c_str(), zerocopy ? "zerocopy" : "copy",
                 c.umem_huge ? "2M" : "4K", busy_ok);

    std::uint64_t tx_full = 0, non_feed = 0, wakeups = 0, spins = 0;
    std::uint32_t tx_outstanding = 0;
    const std::int64_t started = now_ns();
    auto* base = static_cast<unsigned char*>(area);
    wire::Echo echo;

    for (;;) {
        // Completed TX frames go back to the free stack.
        if (tx_outstanding) {
            std::uint32_t idx = 0;
            const std::uint32_t done = xsk_ring_cons__peek(&cq, kBatch, &idx);
            for (std::uint32_t i = 0; i < done; ++i)
                free_frames.push_back(*xsk_ring_cons__comp_addr(&cq, idx + i) & ~std::uint64_t(kFrameSize - 1));
            if (done) {
                xsk_ring_cons__release(&cq, done);
                tx_outstanding -= done;
            }
        }
        refill();

        std::uint32_t ridx = 0;
        const std::uint32_t n = xsk_ring_cons__peek(&rxr, kBatch, &ridx);
        if (!n) {
            ++h.stats().empty_polls;
            if (busy_ok || xsk_ring_prod__needs_wakeup(&fq)) {
                recvfrom(xfd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
                ++wakeups;
            }
            if ((++spins & 0xFFF) == 0 && should_exit(c, h, now_ns(), started)) break;
            continue;
        }
        const std::int64_t now = now_ns();
        ++h.stats().batches;
        std::uint32_t queued = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const xdp_desc* d = xsk_ring_cons__rx_desc(&rxr, ridx + i);
            const std::uint64_t addr = d->addr;
            const std::uint64_t frame = addr & ~std::uint64_t(kFrameSize - 1);
            unsigned char* pkt = base + addr;
            net::UdpView v;
            if (!net::parse_udp(pkt, d->len, v) || v.dst_port != c.port) {
                ++non_feed;
                free_frames.push_back(frame);
                continue;
            }
            if (h.on_payload(pkt + v.payload, v.payload_len, now, echo)) {
                std::uint32_t tidx = 0;
                if (xsk_ring_prod__reserve(&txr, 1, &tidx) == 1) {
                    xdp_desc* t = xsk_ring_prod__tx_desc(&txr, tidx);
                    t->addr = addr;
                    t->len = static_cast<std::uint32_t>(net::make_reply_inplace(pkt, v, &echo, sizeof echo));
                    t->options = 0;
                    ++queued;
                    continue;
                }
                ++tx_full;
            }
            free_frames.push_back(frame);
        }
        xsk_ring_cons__release(&rxr, n);
        if (queued) {
            xsk_ring_prod__submit(&txr, queued);
            tx_outstanding += queued;
            if (busy_ok || xsk_ring_prod__needs_wakeup(&txr)) {
                sendto(xfd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
                ++wakeups;
            }
        }
        if (h.done()) break;
    }

    xsk_socket__delete(xsk);
    xsk_umem__delete(umem);
    bpf_xdp_detach(static_cast<int>(ifindex), xdp_flags, nullptr);
    bpf_object__close(obj);
    munmap(area, umem_bytes);

    char buf[384];
    std::snprintf(buf, sizeof buf,
                  "\"xdp_mode\":\"%s\",\"zerocopy\":%s,\"umem_pages\":\"%s\",\"xdp_busy_poll\":%s,"
                  "\"tx_full\":%llu,\"non_feed\":%llu,\"wakeups\":%llu",
                  c.xdp_mode.c_str(), zerocopy ? "true" : "false", c.umem_huge ? "2M" : "4K",
                  busy_ok ? "true" : "false", (unsigned long long)tx_full,
                  (unsigned long long)non_feed, (unsigned long long)wakeups);
    extra = buf;
    return 0;
}

}  // namespace bl::rx

#else

#include <cstdio>

namespace bl::rx {
int run_afxdp(const Config&, FeedHandler&, std::string&) {
    std::fprintf(stderr, "afxdp: not built (needs libxdp + libbpf + clang; see README)\n");
    return 3;
}
}  // namespace bl::rx

#endif
