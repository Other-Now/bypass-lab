// bl_rx: the feed handler, on one of four receive paths.
//
//   bl_rx --path recvmsg  [--port P] [--core C]
//   bl_rx --path recvmmsg [--port P] [--core C] [--busy-poll-us US]
//   bl_rx --path afxdp    --iface IF [--queue Q] [--xdp-mode native|skb]
//                         [--bind auto|zerocopy|copy] [--umem-huge] [--xdp-busy-poll]
//                         [--bpf-obj FILE] [--core C]
//   bl_rx --path dpdk     [--dpdk-port N] -- <EAL args, e.g. -l 3 -a 0000:00:06.0>
//
// Common: [--idle-exit-ms MS] [--out FILE.json]
//
// Prints "READY ..." on stderr once it can receive, and one JSON line with its
// counters on stdout when the sender's END marker arrives (or it goes idle).

#include <csignal>
#include <cstdio>
#include <string>

#include "bl/args.hpp"
#include "bl/clock.hpp"
#include "rx_paths.hpp"

namespace bl::rx {
volatile std::sig_atomic_t g_stop = 0;
}

int main(int argc, char** argv) try {
    const bl::Args a(argc, argv);
    const std::string path = a.str("path", "recvmsg");

    bl::rx::Config c;
    c.port = static_cast<std::uint16_t>(a.i64("port", bl::wire::kDefaultPort));
    c.core = static_cast<int>(a.i64("core", -1));
    c.iface = a.str("iface");
    c.idle_exit_ns = a.i64("idle-exit-ms", 3000) * 1'000'000;
    c.busy_poll_us = static_cast<int>(a.i64("busy-poll-us", 50));
    c.xdp_mode = a.str("xdp-mode", "native");
    c.bind_mode = a.str("bind", "auto");
    c.umem_huge = a.has("umem-huge");
    c.xdp_busy_poll = a.has("xdp-busy-poll");
    c.queue = static_cast<int>(a.i64("queue", 0));
    c.bpf_obj = a.str("bpf-obj", BL_DEFAULT_BPF_OBJ);
    c.eal_args = a.rest();
    c.dpdk_port = static_cast<int>(a.i64("dpdk-port", -1));

    std::signal(SIGINT, [](int) { bl::rx::g_stop = 1; });
    std::signal(SIGTERM, [](int) { bl::rx::g_stop = 1; });

    // DPDK pins its own main lcore from -l; everything else pins here.
    if (path != "dpdk") bl::pin_this_thread(c.core);

    bl::wire::Path p;
    if (path == "recvmsg") p = bl::wire::Path::Recvmsg;
    else if (path == "recvmmsg") p = bl::wire::Path::Recvmmsg;
    else if (path == "afxdp") p = bl::wire::Path::AfXdp;
    else if (path == "dpdk") p = bl::wire::Path::Dpdk;
    else throw std::runtime_error("unknown --path " + path);

    bl::FeedHandler h(p);
    std::string extra;
    int rc = 0;
    switch (p) {
        case bl::wire::Path::Recvmsg: rc = bl::rx::run_recvmsg(c, h, extra); break;
        case bl::wire::Path::Recvmmsg: rc = bl::rx::run_recvmmsg(c, h, extra); break;
        case bl::wire::Path::AfXdp: rc = bl::rx::run_afxdp(c, h, extra); break;
        case bl::wire::Path::Dpdk: rc = bl::rx::run_dpdk(c, h, extra); break;
    }
    if (rc != 0) return rc;

    char core_field[48];
    std::snprintf(core_field, sizeof core_field, "\"core\":%d%s", c.core, extra.empty() ? "" : ",");
    const std::string js = h.json(core_field + extra);
    std::printf("%s\n", js.c_str());
    std::fflush(stdout);
    if (const std::string out = a.str("out"); !out.empty()) {
        if (std::FILE* f = std::fopen(out.c_str(), "w")) {
            std::fprintf(f, "%s\n", js.c_str());
            std::fclose(f);
        }
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "bl_rx: %s\n", e.what());
    return 1;
}
