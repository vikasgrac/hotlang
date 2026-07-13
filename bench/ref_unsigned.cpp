#include <cstdint>
// The same loop `out[i] = price[i] % 8` in several C++ spellings, to show what
// the *default type* costs — NOT a claim about how any venue encodes prices.
// Non-negative data in a signed 32-bit `int` is a common general-purpose C++
// default (Google's Style Guide and the C++ Core Guidelines both lean signed).
// Every spelling below except the naive signed `% 8` ties hotlang's `u32`.
// All __restrict, built at -O3 -march=native.

// (1) naive signed %8 — the slow default: round-toward-zero correction, 2-wide.
extern "C" int32_t cpp_signed_mod(const int32_t* __restrict p, int32_t* __restrict o){
    for (int i=0;i<4096;i++) o[i] = p[i] % 8;
    return o[0];
}
// (2) signed with the pow-2 mask idiom a hot-path dev writes — ties (same `and`).
extern "C" int32_t cpp_signed_mask(const int32_t* __restrict p, int32_t* __restrict o){
    for (int i=0;i<4096;i++) o[i] = p[i] & 7;
    return o[0];
}
// (3) the one-word type fix `uint32_t` — ties (single `and`, 4-wide).
extern "C" uint32_t cpp_unsigned(const uint32_t* __restrict p, uint32_t* __restrict o){
    for (int i=0;i<4096;i++) o[i] = p[i] % 8;
    return o[0];
}
// (4) signed + __builtin_assume(x>=0): unchecked hint, UB if ever negative — ties.
extern "C" int32_t cpp_assume(const int32_t* __restrict p, int32_t* __restrict o){
    for (int i=0;i<4096;i++){ int32_t v=p[i]; __builtin_assume(v>=0); o[i]=v%8; }
    return o[0];
}
// (5)/(6) 64-bit: signed i64 %8 pays a *smaller* penalty than i32 vs its unsigned peer.
extern "C" int64_t cpp_i64_mod(const int64_t* __restrict p, int64_t* __restrict o){
    for (int i=0;i<4096;i++) o[i] = p[i] % 8;
    return o[0];
}
extern "C" uint64_t cpp_u64_mod(const uint64_t* __restrict p, uint64_t* __restrict o){
    for (int i=0;i<4096;i++) o[i] = p[i] % 8;
    return o[0];
}
