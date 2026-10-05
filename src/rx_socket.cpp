// Paths 1 and 2: the kernel UDP stack.
//
//   recvmsg  -- what most code does: a blocking socket, one syscall per packet,
//               one sendto per echo. The thread sleeps between packets, so every
//               packet pays an interrupt -> softirq -> wakeup -> context switch.
//   recvmmsg -- the best the socket API can do: non-blocking, up to 64 packets
//               per syscall, the thread never sleeps (it spins on the socket),
//               echoes go out in one sendmmsg per batch, and SO_BUSY_POLL lets
//               the spinning thread poll the NIC queue itself instead of
//               waiting for the interrupt (only effective on a real NIC with a
//               NAPI context; a no-op on loopback/veth).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "bl/clock.hpp"
#include "rx_paths.hpp"

#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif

namespace bl::rx {
namespace {

int open_bound(std::uint16_t port, bool nonblocking) {
    const int fd = socket(AF_INET, SOCK_DGRAM | (nonblocking ? SOCK_NONBLOCK : 0), 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    int bytes = 16 << 20;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &bytes, sizeof bytes) != 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        std::perror("bind");
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int run_recvmsg(const Config& c, FeedHandler& h, std::string& extra) {
    const int fd = open_bound(c.port, false);
    if (fd < 0) return 1;
    timeval tv{0, 100'000};  // wake every 100 ms to check the exit rule
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    alignas(64) unsigned char buf[2048];
    sockaddr_in from{};
    iovec iov{buf, sizeof buf};
    msghdr mh{};
    wire::Echo echo;

    std::fprintf(stderr, "READY recvmsg port=%u\n", c.port);
    const std::int64_t started = now_ns();
    for (;;) {
        mh.msg_name = &from;
        mh.msg_namelen = sizeof from;
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        const ssize_t n = recvmsg(fd, &mh, 0);
        const std::int64_t now = now_ns();
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                std::perror("recvmsg");
                break;
            }
            if (should_exit(c, h, now, started)) break;
            continue;
        }
        ++h.stats().batches;
        if (h.on_payload(buf, static_cast<std::size_t>(n), now, echo))
            sendto(fd, &echo, sizeof echo, 0, reinterpret_cast<sockaddr*>(&from), mh.msg_namelen);
        if (h.done()) break;
    }
    close(fd);
    extra = "\"blocking\":true";
    return 0;
}

int run_recvmmsg(const Config& c, FeedHandler& h, std::string& extra) {
    const int fd = open_bound(c.port, true);
    if (fd < 0) return 1;

    // Socket busy polling needs CAP_NET_ADMIN to raise above net.core.busy_read.
    bool busy_ok = false;
    if (c.busy_poll_us > 0) {
        int us = c.busy_poll_us, one = 1, budget = 64;
        busy_ok = setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof us) == 0;
        if (busy_ok) {
            setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof one);
            setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof budget);
        } else {
            std::fprintf(stderr, "warning: SO_BUSY_POLL refused (%s); spinning without it\n",
                         std::strerror(errno));
        }
    }

    constexpr unsigned kB = 64;
    alignas(64) static unsigned char bufs[kB][2048];
    static sockaddr_in from[kB];
    iovec iov[kB];
    mmsghdr mm[kB];
    wire::Echo echoes[kB];
    iovec eiov[kB];
    mmsghdr emm[kB];
    std::memset(mm, 0, sizeof mm);
    std::memset(emm, 0, sizeof emm);

    std::fprintf(stderr, "READY recvmmsg port=%u busy_poll=%d\n", c.port, busy_ok ? c.busy_poll_us : 0);
    const std::int64_t started = now_ns();
    std::uint64_t spins = 0;
    for (;;) {
        for (unsigned i = 0; i < kB; ++i) {
            iov[i] = {bufs[i], sizeof bufs[i]};
            mm[i].msg_hdr.msg_name = &from[i];
            mm[i].msg_hdr.msg_namelen = sizeof from[i];
            mm[i].msg_hdr.msg_iov = &iov[i];
            mm[i].msg_hdr.msg_iovlen = 1;
        }
        const int n = recvmmsg(fd, mm, kB, MSG_DONTWAIT, nullptr);
        if (n <= 0) {
            ++h.stats().empty_polls;
            if ((++spins & 0xFFF) == 0 && should_exit(c, h, now_ns(), started)) break;
            continue;
        }
        const std::int64_t now = now_ns();
        ++h.stats().batches;
        unsigned ne = 0;
        for (int i = 0; i < n; ++i) {
            if (h.on_payload(bufs[i], mm[i].msg_len, now, echoes[ne])) {
                eiov[ne] = {&echoes[ne], sizeof(wire::Echo)};
                emm[ne].msg_hdr.msg_name = &from[i];
                emm[ne].msg_hdr.msg_namelen = mm[i].msg_hdr.msg_namelen;
                emm[ne].msg_hdr.msg_iov = &eiov[ne];
                emm[ne].msg_hdr.msg_iovlen = 1;
                ++ne;
            }
        }
        if (ne) sendmmsg(fd, emm, ne, 0);
        if (h.done()) break;
    }
    close(fd);
    char buf[96];
    std::snprintf(buf, sizeof buf, "\"busy_poll_us\":%d", busy_ok ? c.busy_poll_us : 0);
    extra = buf;
    return 0;
}

}  // namespace bl::rx
