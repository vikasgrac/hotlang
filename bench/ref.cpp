// ref.cpp — idiomatic C++ implementations of the same kernels.
// Compiled clang++ -O3 (same LLVM backend as hotlang). No __restrict, no
// -ffast-math: this is the C++ you'd actually write and ship by default.

#include <cstdint>

extern "C" {

double cpp_dot(const double* xs, const double* ys) {
    double acc = 0.0;
    for (int i = 0; i < 256; i++) acc += xs[i] * ys[i];
    return acc;
}

double cpp_book_pressure(const double* bid_sz, const double* ask_sz) {
    double b = 0.0, a = 0.0;
    for (int i = 0; i < 64; i++) {
        b += bid_sz[i];
        a += ask_sz[i];
    }
    return b / (a + b);
}

double cpp_vwap(const double* px, const double* sz) {
    double notional = 0.0, qty = 0.0;
    for (int i = 0; i < 64; i++) {
        notional += px[i] * sz[i];
        qty += sz[i];
    }
    return notional / qty;
}

double cpp_scale_ladder(const double* prices, double k, double* out) {
    for (int i = 0; i < 256; i++) out[i] = prices[i] * k;
    return k;
}

double cpp_matvec(const double* m, const double* v, double* out) {
    for (int i = 0; i < 32; i++) {
        double acc = 0.0;
        for (int j = 0; j < 32; j++) acc += m[i * 32 + j] * v[j];
        out[i] = acc;
    }
    return out[31];
}

int64_t cpp_decide(double pressure, double vwap_px, double mid, int64_t max_size) {
    double edge = vwap_px - mid;
    bool buy = pressure > 0.6 && edge > 0.0;
    bool sell = pressure < 0.4 && edge < 0.0;
    return buy ? max_size : (sell ? -max_size : 0);
}

}  // extern "C"
