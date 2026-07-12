// ref_streaming.cpp — the recompute references: what a stateless C++ kernel
// library does. Each call recomputes the whole window, O(W). Fully tuned
// (__restrict + reassoc pragma). Variance/vol use the numerically correct
// TWO-PASS centered form (compute mean, then sum of squared deviations) —
// the same algorithm numpy/pandas use — NOT the unstable Sum(x^2) - mean^2
// moment form, so the recompute baseline is itself correct at all scales.

#include <cmath>
#include <cstdint>

extern "C" {

double rc_mean(const double* __restrict px, int w) {
#pragma clang fp reassociate(on) contract(fast)
    double s = 0;
    for (int i = 0; i < w; i++) s += px[i];
    return s / w;
}

double rc_var(const double* __restrict px, int w) {
    double s = 0;
    for (int i = 0; i < w; i++) s += px[i];
    double mu = s / w;
    double m2 = 0;
    for (int i = 0; i < w; i++) { double d = px[i] - mu; m2 += d * d; }
    double v = m2 / w;
    return v > 0 ? v : 0;
}

double rc_vol(const double* __restrict px, int w) {
    return std::sqrt(rc_var(px, w));
}

double rc_vwap(const double* __restrict px, const double* __restrict sz, int w) {
#pragma clang fp reassociate(on) contract(fast)
    double n = 0, q = 0;
    for (int i = 0; i < w; i++) { n += px[i] * sz[i]; q += sz[i]; }
    return n / q;
}

double rc_pressure(const double* __restrict bid, const double* __restrict ask, int w) {
#pragma clang fp reassociate(on) contract(fast)
    double b = 0, a = 0;
    for (int i = 0; i < w; i++) { b += bid[i]; a += ask[i]; }
    return b / (a + b);
}

// Fused recompute: what a stateless kernel library does per tick — recompute
// vwap + pressure + vol (two-pass, correct) over the whole window, then
// decide. Tuned C++.
int64_t cpp_tick_recompute(const double* __restrict px, const double* __restrict sz,
    const double* __restrict bid, const double* __restrict ask, int w) {
    double notional = 0, qty = 0, bidm = 0, askm = 0, s = 0;
    for (int i = 0; i < w; i++) { notional += px[i]*sz[i]; qty += sz[i]; bidm += bid[i]; askm += ask[i]; s += px[i]; }
    double mu = s / w;
    double m2 = 0;
    for (int i = 0; i < w; i++) { double d = px[i] - mu; m2 += d * d; }
    double vwap = notional/qty, pressure = bidm/(askm+bidm);
    double vol = std::sqrt(m2/w > 0 ? m2/w : 0); (void)vol;
    bool buy = pressure > 0.6 && vwap > mu, sell = pressure < 0.4 && vwap < mu;
    return buy ? 100 : (sell ? -100 : 0);
}

// Hand-written INCREMENTAL C++ — the SAME Welford-style O(1) algorithm as
// hotlang's tick_incr. Included to be honest: hotlang does not beat this
// (same algorithm, same roofline) — it ties it. The speedup is incremental-
// vs-recompute (an algorithm win); hotlang's contribution is making the
// incremental form the natural default (a kernel library ships recompute).
// state layout: [notional, qty, bid_mass, ask_mass, price_mean, price_M2]
int64_t cpp_tick_incr(double* __restrict st,
    double epx,double esz,double ebid,double eask,
    double lpx,double lsz,double lbid,double lask,double w){
    double notional=st[0]+epx*esz-lpx*lsz, qty=st[1]+esz-lsz;
    double bidm=st[2]+ebid-lbid, askm=st[3]+eask-lask;
    double old_mean=st[4];
    double new_mean=old_mean+(epx-lpx)/w;
    double m2=st[5]+(epx-lpx)*(epx-new_mean+lpx-old_mean);
    st[0]=notional;st[1]=qty;st[2]=bidm;st[3]=askm;st[4]=new_mean;st[5]=m2;
    double vwap=notional/qty, pressure=bidm/(askm+bidm);
    double vol=std::sqrt(m2/w>0?m2/w:0); (void)vol;
    bool buy=pressure>0.6&&vwap>new_mean, sell=pressure<0.4&&vwap<new_mean;
    return buy?100:(sell?-100:0);
}

}  // extern "C"
