// ref_tuned.zig — the Zig a performance-focused dev ships on a hot kernel:
// `noalias` on pointer params and `@setFloatMode(.optimized)` per function.
// Same algorithms as ref.zig.
//
// Honesty note on the comparison: Zig's `.optimized` float mode maps to
// LLVM's full fast-math flag set — INCLUDING `nnan`/`ninf`, which assume
// NaN/Inf never occur (UB if they do). That is a *stronger* (less safe)
// assumption than hotlang's `reassoc nsz contract` or tuned C++'s
// NaN-preserving pragma; Zig currently has no scoped NaN-preserving
// reassociation mode. So this column gets slightly MORE optimizer freedom
// than hotlang; on straight reduction kernels parity is the honest
// expectation — when its vectorizer is on (zig 0.14.x; 0.16 disables LLVM
// loop vectorization as an upstream-regression workaround) and when full
// fast-math's extra freedom doesn't backfire (measured: it does on some
// AVX-512 shapes; see bench/RESULTS-x86.md).

export fn zigt_dot(noalias xs: [*]const f64, noalias ys: [*]const f64) f64 {
    @setFloatMode(.optimized);
    var acc: f64 = 0.0;
    for (0..256) |i| acc += xs[i] * ys[i];
    return acc;
}

export fn zigt_book_pressure(noalias bid_sz: [*]const f64, noalias ask_sz: [*]const f64) f64 {
    @setFloatMode(.optimized);
    var b: f64 = 0.0;
    var a: f64 = 0.0;
    for (0..64) |i| {
        b += bid_sz[i];
        a += ask_sz[i];
    }
    return b / (a + b);
}

export fn zigt_vwap(noalias px: [*]const f64, noalias sz: [*]const f64) f64 {
    @setFloatMode(.optimized);
    var notional: f64 = 0.0;
    var qty: f64 = 0.0;
    for (0..64) |i| {
        notional += px[i] * sz[i];
        qty += sz[i];
    }
    return notional / qty;
}

export fn zigt_scale_ladder(noalias prices: [*]const f64, k: f64, noalias out: [*]f64) f64 {
    @setFloatMode(.optimized);
    for (0..256) |i| out[i] = prices[i] * k;
    return k;
}

export fn zigt_matvec(noalias m: [*]const f64, noalias v: [*]const f64, noalias out: [*]f64) f64 {
    @setFloatMode(.optimized);
    for (0..32) |i| {
        var acc: f64 = 0.0;
        for (0..32) |j| acc += m[i * 32 + j] * v[j];
        out[i] = acc;
    }
    return out[31];
}

export fn zigt_decide(pressure: f64, vwap_px: f64, mid: f64, max_size: i64) i64 {
    const edge = vwap_px - mid;
    const buy = pressure > 0.6 and edge > 0.0;
    const sell = pressure < 0.4 and edge < 0.0;
    return if (buy) max_size else if (sell) -max_size else 0;
}
