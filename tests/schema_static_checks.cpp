// Compile-time rejection tests for bl::schema. CMake compiles this file once per
// CASE value: CASE 0 must compile; every other case must FAIL to compile with
// the static_assert named next to it. That is how "the compiler refuses an
// inconsistent spec table" is tested rather than asserted.

#include <cstdint>

#include "bl/schema.hpp"

using namespace bl::schema;

struct M {
    std::uint16_t a;
    std::uint32_t b;
    std::uint64_t c;
};
struct Other {
    std::uint32_t x;
};

#ifndef CASE
#define CASE 0
#endif

#if CASE == 0  // well-formed
using S = Schema<Message<'M', 15, M, Field<&M::a, 1>, Field<&M::b, 3>, Field<&M::c, 7>>,
                 Message<'N', 4>>;
static_assert(S::lengths['M'] == 15 && S::lengths['N'] == 4 && S::lengths['O'] == 0);
#elif CASE == 1  // "field runs past the end of the message"
using S = Schema<Message<'M', 14, M, Field<&M::a, 1>, Field<&M::b, 3>, Field<&M::c, 7>>>;
#elif CASE == 2  // "two fields overlap"
using S = Schema<Message<'M', 15, M, Field<&M::a, 1>, Field<&M::b, 2>, Field<&M::c, 7>>>;
#elif CASE == 3  // "two messages share a type byte"
using S = Schema<Message<'M', 15, M, Field<&M::a, 1>>, Message<'M', 4>>;
#elif CASE == 4  // "field is a member of a different struct"
using S = Schema<Message<'M', 15, M, Field<&Other::x, 1>>>;
#elif CASE == 5  // "wire field is wider than its destination"
using S = Schema<Message<'M', 15, M, Field<&M::a, 1, 4>>>;
#elif CASE == 6  // "big-endian fields must be 1, 2, 4, 6 or 8 bytes"
using S = Schema<Message<'M', 15, M, Field<&M::c, 1, 3>>>;
#elif CASE == 7  // "offset 0 is the type byte"
using S = Schema<Message<'M', 15, M, Field<&M::a, 0>>>;
#endif

// Force instantiation of the static_asserts in Message/Schema.
[[maybe_unused]] constexpr auto kLen = S::lengths;

int main() { return 0; }
