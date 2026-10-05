#pragma once

// The feed handler: the part that is identical across all four receive paths.
// A path's only job is to get UDP payload bytes into on_payload() and, when it
// returns true, get the Echo back onto the wire. Whatever differs in the
// latency numbers is therefore the I/O path, not the decode.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "bl/itch.hpp"
#include "bl/wire.hpp"

namespace bl {

struct RxStats {
    std::uint64_t packets = 0;     // data packets accepted
    std::uint64_t messages = 0;    // ITCH messages framed
    std::uint64_t unknown = 0;     // type byte not in the schema
    std::uint64_t bad_length = 0;  // declared length != canonical length
    std::uint64_t malformed = 0;   // payload too short / block overruns payload / bad magic
    std::uint64_t gaps = 0;        // seq numbers skipped (forward jumps)
    std::uint64_t late = 0;        // seq arrived below the high-water mark (reorder or dup)
    std::uint64_t echoes = 0;      // echo replies handed to the path
    std::uint64_t first_seq = 0;
    std::uint64_t last_seq = 0;
    std::uint64_t sender_sent = 0;  // from the END packet; 0 if no END arrived
    std::int64_t first_ns = 0;      // receiver clock: first / last data packet
    std::int64_t last_ns = 0;
    std::uint64_t empty_polls = 0;  // busy-poll iterations that found nothing
    std::uint64_t batches = 0;      // non-empty receive calls / bursts
};

class FeedHandler {
public:
    explicit FeedHandler(wire::Path path) : path_(path) {}

    // Process one UDP payload. Returns true if an echo must be sent, in which
    // case `echo` is filled in. `now` is the receiver clock, read once per
    // receive batch by the caller.
    bool on_payload(const unsigned char* p, std::size_t len, std::int64_t now, wire::Echo& echo) {
        if (len < sizeof(wire::FeedHeader)) [[unlikely]] {
            ++st_.malformed;
            return false;
        }
        wire::FeedHeader h;
        std::memcpy(&h, p, sizeof h);
        if (h.magic != wire::kFeedMagic) [[unlikely]] {
            ++st_.malformed;
            return false;
        }
        if (h.flags & wire::kEnd) [[unlikely]] {
            st_.sender_sent = h.seq;
            end_ = true;
            return false;
        }

        if (st_.packets == 0) {
            st_.first_seq = h.seq;
            st_.first_ns = now;
            next_ = h.seq;
        }
        if (h.seq >= next_) {
            st_.gaps += h.seq - next_;
            next_ = h.seq + 1;
            st_.last_seq = h.seq;
        } else {
            ++st_.late;
        }
        ++st_.packets;
        st_.last_ns = now;

        const unsigned char* q = p + sizeof h;
        const unsigned char* const end = p + len;
        for (std::uint16_t i = 0; i < h.msg_count; ++i) {
            if (end - q < 2) [[unlikely]] {
                ++st_.malformed;
                break;
            }
            const std::size_t mlen = wire::get_be16(q);
            q += 2;
            if (mlen == 0 || static_cast<std::size_t>(end - q) < mlen) [[unlikely]] {
                ++st_.malformed;
                break;
            }
            const auto r = itch::Schema::dispatch(q, mlen, cs_);
            st_.unknown += r == schema::Result::Unknown;
            st_.bad_length += r == schema::Result::BadLength;
            ++st_.messages;
            q += mlen;
        }

        if (!(h.flags & wire::kWantEcho)) return false;
        echo.magic = wire::kEchoMagic;
        echo.path = static_cast<std::uint16_t>(path_);
        echo.reserved = 0;
        echo.seq = h.seq;
        echo.sched_ns = h.sched_ns;
        echo.send_ns = h.send_ns;
        echo.checksum = cs_.sum;
        ++st_.echoes;
        return true;
    }

    bool done() const { return end_; }
    const RxStats& stats() const { return st_; }
    RxStats& stats() { return st_; }
    std::uint64_t checksum() const { return cs_.sum; }
    std::uint64_t decoded() const { return cs_.decoded; }

    std::string json(const std::string& extra = {}) const {
        const double secs = st_.last_ns > st_.first_ns ? double(st_.last_ns - st_.first_ns) / 1e9 : 0;
        const std::uint64_t lost =
            st_.sender_sent > st_.packets ? st_.sender_sent - st_.packets : 0;
        char buf[1536];
        std::snprintf(
            buf, sizeof buf,
            "{\"path\":\"%s\",\"packets\":%llu,\"messages\":%llu,\"decoded\":%llu,"
            "\"checksum\":\"%016llx\",\"unknown\":%llu,\"bad_length\":%llu,\"malformed\":%llu,"
            "\"gaps\":%llu,\"late\":%llu,\"echoes\":%llu,\"first_seq\":%llu,\"last_seq\":%llu,"
            "\"sender_sent\":%llu,\"lost\":%llu,\"rx_seconds\":%.6f,\"rx_pps\":%.1f,"
            "\"batches\":%llu,\"empty_polls\":%llu%s%s}",
            wire::path_name(path_), ull(st_.packets), ull(st_.messages), ull(cs_.decoded),
            ull(cs_.sum), ull(st_.unknown), ull(st_.bad_length), ull(st_.malformed), ull(st_.gaps),
            ull(st_.late), ull(st_.echoes), ull(st_.first_seq), ull(st_.last_seq),
            ull(st_.sender_sent), ull(lost), secs, secs > 0 ? double(st_.packets - 1) / secs : 0.0,
            ull(st_.batches), ull(st_.empty_polls), extra.empty() ? "" : ",", extra.c_str());
        return buf;
    }

private:
    static unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }

    wire::Path path_;
    RxStats st_;
    itch::Checksum cs_;
    std::uint64_t next_ = 0;
    bool end_ = false;
};

}  // namespace bl
