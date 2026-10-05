// Path 4: DPDK.
//
// The NIC is unbound from the kernel driver and handed to a userspace poll-mode
// driver (on AWS: net_ena over vfio-pci). There are no interrupts, no syscalls
// and no kernel code anywhere on the packet path: rte_eth_rx_burst reads the
// NIC's descriptor ring directly, packet buffers come from a mempool carved out
// of huge pages at startup, and the echo is the received mbuf rewritten in
// place and handed straight to rte_eth_tx_burst.
//
// Everything DPDK-specific (which device, which core, huge pages or
// --no-huge) is EAL arguments, passed after `--` on the bl_rx command line, so
// the tuning scripts can vary them without touching this file.

#include "rx_paths.hpp"

#ifdef BL_HAVE_DPDK

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bl/clock.hpp"
#include "bl/net.hpp"

namespace bl::rx {

namespace {
constexpr unsigned kBurst = 32;
constexpr unsigned kMbufs = 8191;
constexpr unsigned kCache = 256;
}  // namespace

int run_dpdk(const Config& c, FeedHandler& h, std::string& extra) {
    std::vector<std::string> args{"bl_rx"};
    args.insert(args.end(), c.eal_args.begin(), c.eal_args.end());
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    if (rte_eal_init(static_cast<int>(argv.size()), argv.data()) < 0) {
        std::fprintf(stderr, "dpdk: rte_eal_init failed (check EAL args / huge pages / vfio)\n");
        return 1;
    }

    std::uint16_t port = 0;
    if (c.dpdk_port >= 0) {
        port = static_cast<std::uint16_t>(c.dpdk_port);
    } else {
        bool found = false;
        std::uint16_t p = 0;
        RTE_ETH_FOREACH_DEV(p) {
            port = p;
            found = true;
            break;
        }
        if (!found) {
            std::fprintf(stderr, "dpdk: no ethdev available\n");
            return 1;
        }
    }
    const int socket = rte_eth_dev_socket_id(port);
    rte_mempool* pool = rte_pktmbuf_pool_create("bl_mbufs", kMbufs, kCache, 0,
                                                RTE_MBUF_DEFAULT_BUF_SIZE,
                                                socket < 0 ? static_cast<int>(rte_socket_id()) : socket);
    if (!pool) {
        std::fprintf(stderr, "dpdk: mempool create failed\n");
        return 1;
    }

    rte_eth_dev_info info{};
    rte_eth_dev_info_get(port, &info);
    rte_eth_conf conf{};
    if (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
    // One RX and one TX queue: the whole feed is one flow on one core.
    if (rte_eth_dev_configure(port, 1, 1, &conf) < 0) {
        std::fprintf(stderr, "dpdk: dev_configure failed\n");
        return 1;
    }
    std::uint16_t nrx = 1024, ntx = 1024;
    rte_eth_dev_adjust_nb_rx_tx_desc(port, &nrx, &ntx);
    if (rte_eth_rx_queue_setup(port, 0, nrx, static_cast<unsigned>(socket < 0 ? 0 : socket), nullptr, pool) < 0 ||
        rte_eth_tx_queue_setup(port, 0, ntx, static_cast<unsigned>(socket < 0 ? 0 : socket), nullptr) < 0) {
        std::fprintf(stderr, "dpdk: queue setup failed\n");
        return 1;
    }
    if (rte_eth_dev_start(port) < 0) {
        std::fprintf(stderr, "dpdk: dev_start failed\n");
        return 1;
    }
    rte_eth_promiscuous_enable(port);  // not supported by every PMD (ENA); harmless if refused

    rte_eth_link link{};
    for (int i = 0; i < 50; ++i) {
        rte_eth_link_get_nowait(port, &link);
        if (link.link_status) break;
        rte_delay_ms(100);
    }

    std::fprintf(stderr, "READY dpdk port=%u driver=%s lcore=%u link=%s nrx=%u ntx=%u\n", port,
                 info.driver_name, rte_lcore_id(), link.link_status ? "up" : "down", nrx, ntx);

    rte_mbuf* rx[kBurst];
    rte_mbuf* tx[kBurst];
    std::uint64_t non_feed = 0, multiseg = 0, tx_drops = 0, spins = 0;
    const std::int64_t started = now_ns();

    for (;;) {
        const std::uint16_t n = rte_eth_rx_burst(port, 0, rx, kBurst);
        if (!n) {
            ++h.stats().empty_polls;
            if ((++spins & 0xFFF) == 0 && should_exit(c, h, now_ns(), started)) break;
            continue;
        }
        const std::int64_t now = now_ns();
        ++h.stats().batches;
        std::uint16_t nt = 0;
        for (std::uint16_t i = 0; i < n; ++i) {
            rte_mbuf* m = rx[i];
            if (m->nb_segs != 1) {
                ++multiseg;
                rte_pktmbuf_free(m);
                continue;
            }
            auto* pkt = rte_pktmbuf_mtod(m, unsigned char*);
            net::UdpView v;
            if (!net::parse_udp(pkt, rte_pktmbuf_data_len(m), v) || v.dst_port != c.port) {
                ++non_feed;
                rte_pktmbuf_free(m);
                continue;
            }
            wire::Echo echo;
            if (h.on_payload(pkt + v.payload, v.payload_len, now, echo)) {
                const auto len = static_cast<std::uint16_t>(net::make_reply_inplace(pkt, v, &echo, sizeof echo));
                m->data_len = len;
                m->pkt_len = len;
                m->ol_flags = 0;
                tx[nt++] = m;
            } else {
                rte_pktmbuf_free(m);
            }
        }
        if (nt) {
            const std::uint16_t sent = rte_eth_tx_burst(port, 0, tx, nt);
            for (std::uint16_t i = sent; i < nt; ++i) rte_pktmbuf_free(tx[i]);
            tx_drops += nt - sent;
        }
        if (h.done()) break;
    }

    rte_eth_stats es{};
    rte_eth_stats_get(port, &es);
    rte_eth_dev_stop(port);
    rte_eth_dev_close(port);

    std::string eal;
    for (const auto& s : c.eal_args) eal += (eal.empty() ? "" : " ") + s;
    char buf[640];
    std::snprintf(buf, sizeof buf,
                  "\"driver\":\"%s\",\"eal_args\":\"%s\",\"non_feed\":%llu,\"multiseg\":%llu,"
                  "\"tx_drops\":%llu,\"nic_ipackets\":%llu,\"nic_imissed\":%llu,\"nic_ierrors\":%llu,"
                  "\"nic_rx_nombuf\":%llu",
                  info.driver_name, eal.c_str(), (unsigned long long)non_feed,
                  (unsigned long long)multiseg, (unsigned long long)tx_drops,
                  (unsigned long long)es.ipackets, (unsigned long long)es.imissed,
                  (unsigned long long)es.ierrors, (unsigned long long)es.rx_nombuf);
    extra = buf;
    rte_eal_cleanup();
    return 0;
}

}  // namespace bl::rx

#else

#include <cstdio>

namespace bl::rx {
int run_dpdk(const Config&, FeedHandler&, std::string&) {
    std::fprintf(stderr, "dpdk: not built (needs libdpdk via pkg-config; see README)\n");
    return 3;
}
}  // namespace bl::rx

#endif
