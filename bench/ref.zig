// ref.zig — idiomatic Zig implementations of the same kernels.
// Compiled `zig build-obj -O ReleaseFast` (Zig bundles its own LLVM; version
// disclosed in the bench environment block). Zig's default float mode is
// strict IEEE like C++/Rust — reductions stay scalar — and plain pointer
// params carry no aliasing promise. This is the Zig you'd write by default.
//
// NOTE: Zig compiles for the HOST cpu by default; the bench script passes
// -mcpu=baseline / -mcpu=native explicitly so this column is flag-matched
// with the clang/rustc contenders.

export fn zig_dot(xs: [*]const f64, ys: [*]const f64) f64 {
    var acc: f64 = 0.0;
    for (0..256) |i| acc += xs[i] * ys[i];
    return acc;
}

export fn zig_book_pressure(bid_sz: [*]const f64, ask_sz: [*]const f64) f64 {
    var b: f64 = 0.0;
    var a: f64 = 0.0;
    for (0..64) |i| {
        b += bid_sz[i];
        a += ask_sz[i];
    }
    return b / (a + b);
}

export fn zig_vwap(px: [*]const f64, sz: [*]const f64) f64 {
    var notional: f64 = 0.0;
    var qty: f64 = 0.0;
    for (0..64) |i| {
        notional += px[i] * sz[i];
        qty += sz[i];
    }
    return notional / qty;
}

export fn zig_scale_ladder(prices: [*]const f64, k: f64, out: [*]f64) f64 {
    for (0..256) |i| out[i] = prices[i] * k;
    return k;
}

export fn zig_matvec(m: [*]const f64, v: [*]const f64, out: [*]f64) f64 {
    for (0..32) |i| {
        var acc: f64 = 0.0;
        for (0..32) |j| acc += m[i * 32 + j] * v[j];
        out[i] = acc;
    }
    return out[31];
}

export fn zig_decide(pressure: f64, vwap_px: f64, mid: f64, max_size: i64) i64 {
    const edge = vwap_px - mid;
    const buy = pressure > 0.6 and edge > 0.0;
    const sell = pressure < 0.4 and edge < 0.0;
    return if (buy) max_size else if (sell) -max_size else 0;
}
