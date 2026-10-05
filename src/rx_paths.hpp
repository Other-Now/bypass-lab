#pragma once

#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

#include "bl/handler.hpp"

namespace bl::rx {

struct Config {
    std::uint16_t port = wire::kDefaultPort;
    int core = -1;                     // -1: unpinned
    std::string iface;                 // afxdp
    std::int64_t idle_exit_ns = 3'000'000'000;   // after traffic starts
    std::int64_t start_wait_ns = 120'000'000'000;  // before traffic starts
    // recvmmsg
    int busy_poll_us = 50;
    // afxdp
    std::string xdp_mode = "native";   // native | skb
    std::string bind_mode = "auto";    // auto | zerocopy | copy
    bool umem_huge = false;
    bool xdp_busy_poll = false;
    int queue = 0;
    std::string bpf_obj;
    // dpdk
    std::vector<std::string> eal_args;
    int dpdk_port = -1;
};

extern volatile std::sig_atomic_t g_stop;

// Each returns 0 on success and appends path-specific JSON fields to `extra`.
int run_recvmsg(const Config&, FeedHandler&, std::string& extra);
int run_recvmmsg(const Config&, FeedHandler&, std::string& extra);
int run_afxdp(const Config&, FeedHandler&, std::string& extra);
int run_dpdk(const Config&, FeedHandler&, std::string& extra);

// Shared exit rule: stop on END, on Ctrl-C, after idle_exit_ns of silence once
// traffic has started, or after start_wait_ns if it never starts.
inline bool should_exit(const Config& c, const FeedHandler& h, std::int64_t now, std::int64_t started) {
    if (g_stop || h.done()) return true;
    const auto& s = h.stats();
    if (s.packets == 0) return now - started > c.start_wait_ns;
    return now - s.last_ns > c.idle_exit_ns;
}

}  // namespace bl::rx
