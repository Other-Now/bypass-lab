#pragma once

// One clock for everything: CLOCK_MONOTONIC via the vDSO (~20 ns on x86 with a
// TSC clocksource, no syscall). Latencies here are microseconds, so a ~20 ns
// read is noise; the payoff is that every number is in plain nanoseconds with
// no TSC calibration step to get wrong. tick2trade needed rdtsc because it was
// timing 37 ns of work; this project is timing a network round trip.

#include <cstdint>
#include <cstdio>
#include <ctime>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace bl {

inline std::int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// Pin the calling thread. core < 0 means "leave it to the scheduler", which is
// the unpinned baseline of the tuning ladder.
inline bool pin_this_thread(int core) {
#if defined(__linux__)
    if (core < 0) return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr, "warning: could not pin to core %d\n", core);
        return false;
    }
    return true;
#else
    (void)core;
    return false;
#endif
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

}  // namespace bl
