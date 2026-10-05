#pragma once

// Compile-time message decoder generation.
//
// A wire protocol is declared once, as types:
//
//   using Add = Message<'A', 36, AddOrder,
//                       Field<&AddOrder::ref,    11>,
//                       Field<&AddOrder::shares, 20>, ...>;
//   using Itch = Schema<Add, Delete, ...>;
//
// and everything the hot path needs is derived from that declaration at compile
// time:
//
//   * the per-field load (width + byte order picked from the declaration, so a
//     48-bit big-endian timestamp becomes one 16-bit and one 32-bit bswap load);
//   * a 256-entry length table indexed by the type byte, so framing validation
//     is one load and one compare, with no switch;
//   * the dispatch, generated two ways (a fold-expression compare chain and a
//     function-pointer table) so they can be measured against a hand-written
//     switch in bench/decoder_bench.cpp.
//
// The declaration is also *checked* at compile time: a field that runs past the
// end of its message, two fields that overlap, a field bound to the wrong
// struct, a width that does not fit its destination, or two messages sharing a
// type byte are all static_assert failures, not runtime bugs. tests/
// schema_static_checks.cpp shows each one. That is the real argument for doing
// this with templates: the offsets in the ITCH spec are typed in once and the
// compiler refuses an inconsistent table.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__GNUC__) || defined(__clang__)
#define BL_INLINE [[gnu::always_inline]] inline
#else
#define BL_INLINE inline
#endif

namespace bl::schema {

// How a field's bytes map to its destination.
enum class Enc : std::uint8_t {
    BE,   // big-endian unsigned integer of `Width` bytes (1, 2, 4, 6 or 8)
    Raw,  // bytes copied verbatim (e.g. an 8-byte space-padded symbol packed into a u64)
};

namespace detail {

template <class> struct member_traits;
template <class C, class T> struct member_traits<T C::*> {
    using owner = C;
    using value = T;
};

template <std::size_t W>
inline constexpr bool be_width_ok = W == 1 || W == 2 || W == 4 || W == 6 || W == 8;

template <std::size_t W>
BL_INLINE std::uint64_t load_be(const unsigned char* p) {
    if constexpr (W == 1) {
        return p[0];
    } else if constexpr (W == 2) {
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        return __builtin_bswap16(v);
    } else if constexpr (W == 4) {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return __builtin_bswap32(v);
    } else if constexpr (W == 6) {
        return (load_be<2>(p) << 32) | load_be<4>(p + 2);
    } else {
        static_assert(W == 8);
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        return __builtin_bswap64(v);
    }
}

template <class... Fs>
consteval bool fields_disjoint() {
    constexpr std::size_t n = sizeof...(Fs);
    if constexpr (n < 2) {
        return true;
    } else {
        const std::array<std::size_t, n> lo{Fs::offset...};
        const std::array<std::size_t, n> hi{(Fs::offset + Fs::width)...};
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (lo[i] < hi[j] && lo[j] < hi[i]) return false;
        return true;
    }
}

template <class... Ms>
consteval bool types_unique() {
    constexpr std::size_t n = sizeof...(Ms);
    if constexpr (n < 2) {
        return true;
    } else {
        const std::array<unsigned char, n> t{Ms::type...};
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (t[i] == t[j]) return false;
        return true;
    }
}

}  // namespace detail

// One field: which struct member it lands in, where it sits in the message
// (offset 0 is the type byte), how wide it is on the wire, and its encoding.
// Width defaults to the size of the destination member.
template <auto Member, std::size_t Offset,
          std::size_t Width =
              sizeof(typename detail::member_traits<decltype(Member)>::value),
          Enc E = Enc::BE>
struct Field {
    using owner = typename detail::member_traits<decltype(Member)>::owner;
    using value_type = typename detail::member_traits<decltype(Member)>::value;
    static constexpr std::size_t offset = Offset;
    static constexpr std::size_t width = Width;
    static constexpr Enc enc = E;

    static_assert(std::is_integral_v<value_type>, "fields decode into integers");
    static_assert(Width <= sizeof(value_type), "wire field is wider than its destination");
    static_assert(E == Enc::Raw || detail::be_width_ok<Width>,
                  "big-endian fields must be 1, 2, 4, 6 or 8 bytes");
    static_assert(E == Enc::BE || Width == sizeof(value_type),
                  "raw fields must exactly fill their destination");

    BL_INLINE static void load(owner& o, const unsigned char* msg) {
        if constexpr (E == Enc::Raw)
            std::memcpy(&(o.*Member), msg + Offset, Width);
        else
            o.*Member = static_cast<value_type>(detail::load_be<Width>(msg + Offset));
    }
};

// Body type for messages that are framed and length-checked but not decoded
// (system events, directory, MWCB ...). The visitor is never called for them.
struct Skip {};

template <char Type, std::size_t Length, class Body = Skip, class... Fields>
struct Message {
    static constexpr unsigned char type = static_cast<unsigned char>(Type);
    static constexpr std::size_t length = Length;
    using body = Body;
    static constexpr bool decoded = !std::is_same_v<Body, Skip>;

    static_assert(Length >= 1 && Length <= 255, "length must fit the u8 length table");
    static_assert(decoded || sizeof...(Fields) == 0, "a Skip message cannot declare fields");
    static_assert(((Fields::offset >= 1) && ...), "offset 0 is the type byte");
    static_assert(((Fields::offset + Fields::width <= Length) && ...),
                  "field runs past the end of the message");
    static_assert((std::is_same_v<typename Fields::owner, Body> && ...),
                  "field is a member of a different struct than this message's body");
    static_assert(detail::fields_disjoint<Fields...>(), "two fields overlap");

    BL_INLINE static Body decode(const unsigned char* m) {
        Body b{};
        (Fields::load(b, m), ...);
        return b;
    }
};

enum class Result : std::uint8_t { Ok, Unknown, BadLength };

template <class... Ms>
struct Schema {
    static_assert(sizeof...(Ms) > 0);
    static_assert(detail::types_unique<Ms...>(), "two messages share a type byte");

    // Canonical length per type byte; 0 = not in the schema.
    static constexpr std::array<std::uint8_t, 256> lengths = [] {
        std::array<std::uint8_t, 256> t{};
        ((t[Ms::type] = static_cast<std::uint8_t>(Ms::length)), ...);
        return t;
    }();

    static constexpr std::size_t max_length = [] {
        std::size_t m = 0;
        ((m = Ms::length > m ? Ms::length : m), ...);
        return m;
    }();

    template <class M, class V>
    BL_INLINE static void handle(const unsigned char* m, V& v) {
        if constexpr (M::decoded) v(M::decode(m));
    }

    // Framing check shared by both dispatch strategies: one table load, one
    // compare. Unknown and wrong-length messages take the same cold branch.
    BL_INLINE static Result check(const unsigned char* m, std::size_t len) {
        const std::uint8_t want = lengths[m[0]];
        if (want == len) [[likely]] return Result::Ok;
        return want == 0 ? Result::Unknown : Result::BadLength;
    }

    // Dispatch A: the fold expression expands to `t=='A' ? .. : t=='F' ? ..`;
    // every handler is visible to the optimiser, so it inlines them and is free
    // to lower the chain to a jump table.
    template <class V>
    BL_INLINE static Result dispatch(const unsigned char* m, std::size_t len, V& v) {
        const Result r = check(m, len);
        if (r != Result::Ok) [[unlikely]] return r;
        const unsigned char t = m[0];
        (void)((t == Ms::type ? (handle<Ms>(m, v), true) : false) || ...);
        return Result::Ok;
    }

    // Dispatch B: a 256-entry function-pointer table built at compile time.
    // Zero conditional branches on the type byte, one indirect call -- which
    // also means the visitor can no longer be inlined into the loop.
    template <class V>
    using Fn = void (*)(const unsigned char*, V&);

    template <class V>
    static void noop(const unsigned char*, V&) {}

    template <class M, class V>
    static void thunk(const unsigned char* m, V& v) {
        handle<M>(m, v);
    }

    template <class V>
    static constexpr std::array<Fn<V>, 256> table = [] {
        std::array<Fn<V>, 256> t{};
        t.fill(&noop<V>);
        ((t[Ms::type] = &thunk<Ms, V>), ...);
        return t;
    }();

    template <class V>
    BL_INLINE static Result dispatch_table(const unsigned char* m, std::size_t len, V& v) {
        const Result r = check(m, len);
        if (r != Result::Ok) [[unlikely]] return r;
        table<V>[m[0]](m, v);
        return Result::Ok;
    }
};

}  // namespace bl::schema
