// ref_fused.cpp — the C++ reference for the FUSED pipeline comparison.
// Two contenders:
//   cpp_tick_5call : the pipeline as five separate tuned kernels (what a
//                    desk calling a kernel library actually ships). Each
//                    call round-trips the 256-wide `out` buffer.
//   cpp_tick_fused : the same pipeline hand-fused into one function, fully
//                    tuned (__restrict + reassoc pragma). The strongest
//                    possible C++ — what hotlang's fused `tick` ties.
// Both are correctness-matched against hotlang's tick().

#include <cstdint>

extern "C" {

// --- five tuned kernels (same as ref_tuned.cpp, local copies) ---
static inline double k_scale(const double* __restrict p, double k, double* __restrict o) {
#pragma clang fp reassociate(on) contract(fast)
    for (int i = 0; i < 256; i++) o[i] = p[i] * k;
    return k;
}
static inline double k_vwap(const double* __restrict px, const double* __restrict s) {
#pragma clang fp reassociate(on) contract(fast)
    double n = 0, q = 0;
    for (int i = 0; i < 64; i++) { n += px[i] * s[i]; q += s[i]; }
    return n / q;
}
static inline double k_pressure(const double* __restrict b, const double* __restrict a) {
#pragma clang fp reassociate(on) contract(fast)
    double bs = 0, as = 0;
    for (int i = 0; i < 64; i++) { bs += b[i]; as += a[i]; }
    return bs / (as + bs);
}
static inline double k_dot(const double* __restrict x, const double* __restrict y) {
#pragma clang fp reassociate(on) contract(fast)
    double acc = 0;
    for (int i = 0; i < 256; i++) acc += x[i] * y[i];
    return acc;
}
static inline int64_t k_decide(double pr, double v, double mid, int64_t mx) {
    double edge = v - mid;
    bool buy = pr > 0.6 && edge > 0.0, sell = pr < 0.4 && edge < 0.0;
    return buy ? mx : (sell ? -mx : 0);
}

// out buffer for the 5-call variant (caller-owned in the real world; static
// here to match the hotlang harness which reuses one buffer).
static double OUT5[256];

int64_t cpp_tick_5call(const double* prices, const double* weights,
                       const double* sz, const double* bid,
                       const double* ask, double fx) {
    k_scale(prices, fx, OUT5);
    double v = k_vwap(OUT5, sz);
    double p = k_pressure(bid, ask);
    double sig = k_dot(OUT5, weights);
    return k_decide(p, v, 100.02, sig > 0.0 ? 100 : 50);
}

int64_t cpp_tick_fused(const double* __restrict prices, const double* __restrict weights,
                       const double* __restrict sz, const double* __restrict bid,
                       const double* __restrict ask, double fx) {
#pragma clang fp reassociate(on) contract(fast)
    double notional = 0, qty = 0, bsum = 0, asum = 0, sig = 0;
    for (int i = 0; i < 64; i++) {
        double s = prices[i] * fx;
        notional += s * sz[i]; qty += sz[i];
        bsum += bid[i]; asum += ask[i];
        sig += s * weights[i];
    }
    for (int i = 64; i < 256; i++) sig += prices[i] * fx * weights[i];
    double vwap = notional / qty, pressure = bsum / (asum + bsum);
    int64_t max_size = sig > 0.0 ? 100 : 50;
    double edge = vwap - 100.02;
    bool buy = pressure > 0.6 && edge > 0.0, sell = pressure < 0.4 && edge < 0.0;
    return buy ? max_size : (sell ? -max_size : 0);
}

}  // extern "C"
