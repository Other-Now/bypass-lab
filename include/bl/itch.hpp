#pragma once

// NASDAQ TotalView-ITCH 5.0, declared as a bl::schema::Schema.
//
// All 23 message types are framed and length-checked; the eight that move an
// order book are decoded into structs. Offsets are from the ITCH 5.0 spec with
// offset 0 = the message type byte (NASDAQ BinaryFILE / MoldUDP64 framing, after
// the 2-byte length). They are the same offsets tick2trade's hand-written
// accessors use, which were validated over a full trading day.

#include <cstdint>

#include "bl/schema.hpp"

namespace bl::itch {

struct AddOrder {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    char side;
    std::uint32_t shares;
    std::uint64_t stock;  // 8 ASCII bytes, raw
    std::uint32_t price;  // 4 implied decimals
    bool operator==(const AddOrder&) const = default;
};
struct AddOrderMPID {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    char side;
    std::uint32_t shares;
    std::uint64_t stock;
    std::uint32_t price;
    std::uint32_t mpid;  // 4 ASCII bytes, raw
    bool operator==(const AddOrderMPID&) const = default;
};
struct OrderExecuted {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
    bool operator==(const OrderExecuted&) const = default;
};
struct OrderExecutedPrice {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
    char printable;
    std::uint32_t price;
    bool operator==(const OrderExecutedPrice&) const = default;
};
struct OrderCancel {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    std::uint32_t shares;
    bool operator==(const OrderCancel&) const = default;
};
struct OrderDelete {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    bool operator==(const OrderDelete&) const = default;
};
struct OrderReplace {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t old_ref;
    std::uint64_t new_ref;
    std::uint32_t shares;
    std::uint32_t price;
    bool operator==(const OrderReplace&) const = default;
};
struct Trade {
    std::uint16_t locate;
    std::uint64_t ts;
    std::uint64_t ref;
    char side;
    std::uint32_t shares;
    std::uint64_t stock;
    std::uint32_t price;
    std::uint64_t match;
    bool operator==(const Trade&) const = default;
};

namespace s = bl::schema;
using s::Enc;
using s::Field;
using s::Message;

// The schema. Read this as the spec table, one row per field.
using Schema = s::Schema<
    Message<'S', 12>,  // system event
    Message<'R', 39>,  // stock directory
    Message<'H', 25>,  // stock trading action
    Message<'Y', 20>,  // Reg SHO restriction
    Message<'L', 26>,  // market participant position
    Message<'V', 35>,  // MWCB decline level
    Message<'W', 12>,  // MWCB status
    Message<'K', 28>,  // IPO quoting period
    Message<'J', 35>,  // LULD auction collar
    Message<'h', 21>,  // operational halt
    Message<'A', 36, AddOrder,
            Field<&AddOrder::locate, 1>,
            Field<&AddOrder::ts, 5, 6>,
            Field<&AddOrder::ref, 11>,
            Field<&AddOrder::side, 19>,
            Field<&AddOrder::shares, 20>,
            Field<&AddOrder::stock, 24, 8, Enc::Raw>,
            Field<&AddOrder::price, 32>>,
    Message<'F', 40, AddOrderMPID,
            Field<&AddOrderMPID::locate, 1>,
            Field<&AddOrderMPID::ts, 5, 6>,
            Field<&AddOrderMPID::ref, 11>,
            Field<&AddOrderMPID::side, 19>,
            Field<&AddOrderMPID::shares, 20>,
            Field<&AddOrderMPID::stock, 24, 8, Enc::Raw>,
            Field<&AddOrderMPID::price, 32>,
            Field<&AddOrderMPID::mpid, 36, 4, Enc::Raw>>,
    Message<'E', 31, OrderExecuted,
            Field<&OrderExecuted::locate, 1>,
            Field<&OrderExecuted::ts, 5, 6>,
            Field<&OrderExecuted::ref, 11>,
            Field<&OrderExecuted::shares, 19>,
            Field<&OrderExecuted::match, 23>>,
    Message<'C', 36, OrderExecutedPrice,
            Field<&OrderExecutedPrice::locate, 1>,
            Field<&OrderExecutedPrice::ts, 5, 6>,
            Field<&OrderExecutedPrice::ref, 11>,
            Field<&OrderExecutedPrice::shares, 19>,
            Field<&OrderExecutedPrice::match, 23>,
            Field<&OrderExecutedPrice::printable, 31>,
            Field<&OrderExecutedPrice::price, 32>>,
    Message<'X', 23, OrderCancel,
            Field<&OrderCancel::locate, 1>,
            Field<&OrderCancel::ts, 5, 6>,
            Field<&OrderCancel::ref, 11>,
            Field<&OrderCancel::shares, 19>>,
    Message<'D', 19, OrderDelete,
            Field<&OrderDelete::locate, 1>,
            Field<&OrderDelete::ts, 5, 6>,
            Field<&OrderDelete::ref, 11>>,
    Message<'U', 35, OrderReplace,
            Field<&OrderReplace::locate, 1>,
            Field<&OrderReplace::ts, 5, 6>,
            Field<&OrderReplace::old_ref, 11>,
            Field<&OrderReplace::new_ref, 19>,
            Field<&OrderReplace::shares, 27>,
            Field<&OrderReplace::price, 31>>,
    Message<'P', 44, Trade,
            Field<&Trade::locate, 1>,
            Field<&Trade::ts, 5, 6>,
            Field<&Trade::ref, 11>,
            Field<&Trade::side, 19>,
            Field<&Trade::shares, 20>,
            Field<&Trade::stock, 24, 8, Enc::Raw>,
            Field<&Trade::price, 32>,
            Field<&Trade::match, 36>>,
    Message<'Q', 40>,  // cross trade
    Message<'B', 19>,  // broken trade
    Message<'I', 50>,  // NOII
    Message<'N', 20>,  // retail price improvement indicator
    Message<'O', 48>   // DLCR price discovery
    >;

static_assert(Schema::lengths['A'] == 36 && Schema::lengths['I'] == 50);
static_assert(Schema::lengths['Z'] == 0);
static_assert(Schema::max_length == 50);

// Order-independent checksum over every decoded field. Sum of per-message
// hashes, so packet reordering does not change it but a lost, duplicated or
// corrupted message does: two receive paths that saw the same feed must agree.
struct Checksum {
    std::uint64_t sum = 0;
    std::uint64_t decoded = 0;

    static constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
    static constexpr std::uint64_t fin(std::uint64_t z) {  // splitmix64 finaliser
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    void add(std::uint64_t h) {
        sum += fin(h);
        ++decoded;
    }

    void operator()(const AddOrder& m) {
        add(mix(mix(mix(mix(mix(mix(mix('A', m.locate), m.ts), m.ref), std::uint64_t(m.side)),
                        m.shares), m.stock), m.price));
    }
    void operator()(const AddOrderMPID& m) {
        add(mix(mix(mix(mix(mix(mix(mix(mix('F', m.locate), m.ts), m.ref),
                                    std::uint64_t(m.side)), m.shares), m.stock), m.price),
                m.mpid));
    }
    void operator()(const OrderExecuted& m) {
        add(mix(mix(mix(mix(mix('E', m.locate), m.ts), m.ref), m.shares), m.match));
    }
    void operator()(const OrderExecutedPrice& m) {
        add(mix(mix(mix(mix(mix(mix(mix('C', m.locate), m.ts), m.ref), m.shares), m.match),
                    std::uint64_t(m.printable)), m.price));
    }
    void operator()(const OrderCancel& m) {
        add(mix(mix(mix(mix('X', m.locate), m.ts), m.ref), m.shares));
    }
    void operator()(const OrderDelete& m) { add(mix(mix(mix('D', m.locate), m.ts), m.ref)); }
    void operator()(const OrderReplace& m) {
        add(mix(mix(mix(mix(mix(mix('U', m.locate), m.ts), m.old_ref), m.new_ref), m.shares),
                m.price));
    }
    void operator()(const Trade& m) {
        add(mix(mix(mix(mix(mix(mix(mix(mix('P', m.locate), m.ts), m.ref), std::uint64_t(m.side)),
                                m.shares), m.stock), m.price), m.match));
    }
};

}  // namespace bl::itch
