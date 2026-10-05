#pragma once

// Load a NASDAQ BinaryFILE ITCH capture ([u16 BE length][message] ...) and
// pack it into UDP payloads for replay.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "bl/itch.hpp"
#include "bl/wire.hpp"

namespace bl::feed {

struct Frame {
    std::uint32_t off;  // offset of the message body (type byte)
    std::uint16_t len;
};

struct File {
    std::vector<unsigned char> bytes;
    std::vector<Frame> frames;
};

// Reads up to max_msgs messages (0 = all). Stops cleanly at a zero-length frame
// or a truncated tail.
inline File load(const std::string& path, std::size_t max_msgs = 0) {
    File f;
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) throw std::runtime_error("cannot open feed: " + path);
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    f.bytes.resize(static_cast<std::size_t>(size));
    if (size > 0 && std::fread(f.bytes.data(), 1, f.bytes.size(), fp) != f.bytes.size()) {
        std::fclose(fp);
        throw std::runtime_error("short read: " + path);
    }
    std::fclose(fp);

    std::size_t pos = 0;
    while (pos + 2 <= f.bytes.size()) {
        const std::uint16_t len = wire::get_be16(&f.bytes[pos]);
        if (len == 0 || pos + 2 + len > f.bytes.size()) break;
        f.frames.push_back(Frame{static_cast<std::uint32_t>(pos + 2), len});
        pos += 2 + len;
        if (max_msgs && f.frames.size() >= max_msgs) break;
    }
    return f;
}

// A pool of ready-to-send payloads. The sender stamps seq/flags/timestamps into
// the header at send time; everything else is prepared up front so the send
// loop does no packing work. `checksum` is what a receiver's Checksum adds when
// it decodes the payload, so the sender can predict the receiver's final
// checksum exactly.
struct Packets {
    std::vector<unsigned char> buf;
    std::vector<std::uint32_t> off;
    std::vector<std::uint16_t> len;
    std::vector<std::uint64_t> checksum;
    std::vector<std::uint16_t> msgs;
    std::size_t size() const { return off.size(); }
};

inline Packets pack(const File& f, std::size_t msgs_per_packet, std::size_t max_packets) {
    if (msgs_per_packet == 0 || msgs_per_packet > 64)
        throw std::runtime_error("msgs_per_packet must be 1..64");
    Packets p;
    const std::size_t n_pk = std::min(max_packets, (f.frames.size() + msgs_per_packet - 1) / msgs_per_packet);
    p.buf.reserve(n_pk * (sizeof(wire::FeedHeader) + msgs_per_packet * 40));
    std::size_t fi = 0;
    for (std::size_t k = 0; k < n_pk && fi < f.frames.size(); ++k) {
        const std::size_t start = p.buf.size();
        p.buf.resize(start + sizeof(wire::FeedHeader));
        itch::Checksum cs;
        std::uint16_t count = 0;
        for (std::size_t j = 0; j < msgs_per_packet && fi < f.frames.size(); ++j, ++fi) {
            const Frame& fr = f.frames[fi];
            const std::size_t at = p.buf.size();
            p.buf.resize(at + 2 + fr.len);
            wire::put_be16(&p.buf[at], fr.len);
            std::memcpy(&p.buf[at + 2], &f.bytes[fr.off], fr.len);
            itch::Schema::dispatch(&f.bytes[fr.off], fr.len, cs);
            ++count;
        }
        wire::FeedHeader h{wire::kFeedMagic, 0, count, 0, 0, 0};
        std::memcpy(&p.buf[start], &h, sizeof h);
        p.off.push_back(static_cast<std::uint32_t>(start));
        p.len.push_back(static_cast<std::uint16_t>(p.buf.size() - start));
        p.checksum.push_back(cs.sum);
        p.msgs.push_back(count);
    }
    return p;
}

}  // namespace bl::feed
