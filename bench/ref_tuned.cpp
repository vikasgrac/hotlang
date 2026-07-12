// ref_tuned.cpp — the C++ a competent HFT dev actually ships on a hot
// kernel: __restrict on pointer params and a function-local
// `#pragma clang fp reassociate(on) contract(fast)` (clang >= 12, no
// -ffast-math flag, NaN semantics preserved). Same algorithms as ref.cpp.
// This column exists because comparing only against default-semantics C++
// overstates hotlang; parity here is the honest expectation.

#include <cstdint>

extern "C" {

double cppt_dot(const double* __restrict xs, const double* __restrict ys) {
#pragma clang fp reassociate(on) contract(fast)
    double acc = 0.0;
    for (int i = 0; i < 256; i++) acc += xs[i] * ys[i];
    return acc;
}

double cppt_book_pressure(const double* __restrict bid_sz, const double* __restrict ask_sz) {
#pragma clang fp reassociate(on) contract(fast)
    double b = 0.0, a = 0.0;
    for (int i = 0; i < 64; i++) {
        b += bid_sz[i];
        a += ask_sz[i];
    }
    return b / (a + b);
}

double cppt_vwap(const double* __restrict px, const double* __restrict sz) {
#pragma clang fp reassociate(on) contract(fast)
    double notional = 0.0, qty = 0.0;
    for (int i = 0; i < 64; i++) {
        notional += px[i] * sz[i];
        qty += sz[i];
    }
    return notional / qty;
}

double cppt_scale_ladder(const double* __restrict prices, double k, double* __restrict out) {
#pragma clang fp reassociate(on) contract(fast)
    for (int i = 0; i < 256; i++) out[i] = prices[i] * k;
    return k;
}

double cppt_matvec(const double* __restrict m, const double* __restrict v, double* __restrict out) {
#pragma clang fp reassociate(on) contract(fast)
    for (int i = 0; i < 32; i++) {
        double acc = 0.0;
        for (int j = 0; j < 32; j++) acc += m[i * 32 + j] * v[j];
        out[i] = acc;
    }
    return out[31];
}

int64_t cppt_decide(double pressure, double vwap_px, double mid, int64_t max_size) {
    double edge = vwap_px - mid;
    bool buy = pressure > 0.6 && edge > 0.0;
    bool sell = pressure < 0.4 && edge < 0.0;
    return buy ? max_size : (sell ? -max_size : 0);
}

}  // extern "C"
