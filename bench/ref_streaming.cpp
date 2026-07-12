// ref_streaming.cpp — the recompute references: what a stateless C++ kernel
// library does. Each call recomputes the whole window, O(W). Fully tuned
// (__restrict + reassoc pragma) — the strongest C++, so the incremental
// speedup is not an artifact of weak reference code.

#include <cmath>

extern "C" {

double rc_mean(const double* __restrict px, int w) {
#pragma clang fp reassociate(on) contract(fast)
    double s = 0;
    for (int i = 0; i < w; i++) s += px[i];
    return s / w;
}

double rc_var(const double* __restrict px, int w) {
#pragma clang fp reassociate(on) contract(fast)
    double s = 0, s2 = 0;
    for (int i = 0; i < w; i++) { s += px[i]; s2 += px[i] * px[i]; }
    double mu = s / w;
    double v = s2 / w - mu * mu;
    return v > 0 ? v : 0;
}

double rc_vol(const double* __restrict px, int w) {
    double v = rc_var(px, w);
    return std::sqrt(v);
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

}  // extern "C"

// Fused recompute: what a stateless kernel library does per tick — recompute
// vwap + pressure + vol over the whole window, then decide. Tuned C++.
extern "C" int64_t cpp_tick_recompute(const double* __restrict px, const double* __restrict sz,
    const double* __restrict bid, const double* __restrict ask, int w) {
#pragma clang fp reassociate(on) contract(fast)
    double notional=0,qty=0,bidm=0,askm=0,sp=0,sp2=0;
    for(int i=0;i<w;i++){ notional+=px[i]*sz[i]; qty+=sz[i]; bidm+=bid[i]; askm+=ask[i]; sp+=px[i]; sp2+=px[i]*px[i]; }
    double vwap=notional/qty, pressure=bidm/(askm+bidm), mu=sp/w;
    double vv=sp2/w-mu*mu; double vol=vv>0?vv:0; vol=__builtin_sqrt(vol);
    double edge=vwap-mu;
    bool buy=pressure>0.6&&edge>vol, sell=pressure<0.4&&edge<-vol;
    return buy?100:(sell?-100:0);
}

// Hand-written INCREMENTAL C++ — the SAME O(1) algorithm as hotlang's
// tick_incr. Included to be honest: hotlang does not beat this (same
// algorithm, same roofline) — it ties it. The 18x is incremental-vs-
// recompute; hotlang's contribution is making the incremental form the
// safe, natural default (state buffer noalias + bounds-proven; the ring
// builtin automates the window). A kernel library ships the recompute form.
extern "C" int64_t cpp_tick_incr(double* __restrict st,
    double epx,double esz,double ebid,double eask,
    double lpx,double lsz,double lbid,double lask,double w){
    double notional=st[0]+epx*esz-lpx*lsz, qty=st[1]+esz-lsz;
    double bidm=st[2]+ebid-lbid, askm=st[3]+eask-lask;
    double sp=st[4]+epx-lpx, sp2=st[5]+epx*epx-lpx*lpx;
    st[0]=notional;st[1]=qty;st[2]=bidm;st[3]=askm;st[4]=sp;st[5]=sp2;
    double vwap=notional/qty, pressure=bidm/(askm+bidm), mu=sp/w;
    double vv=sp2/w-mu*mu; double vol=__builtin_sqrt(vv>0?vv:0);
    double edge=vwap-mu;
    bool buy=pressure>0.6&&edge>vol, sell=pressure<0.4&&edge<-vol;
    return buy?100:(sell?-100:0);
}
