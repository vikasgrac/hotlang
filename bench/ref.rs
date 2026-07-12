// ref.rs — idiomatic Rust implementations of the same kernels.
// Compiled rustc -C opt-level=3 (rustc bundles its own LLVM; version in the
// bench.sh environment block). Safe iterator style inside; C ABI at the
// boundary. Note: because the extern params are raw pointers (C ABI), LLVM
// gets no noalias from them — reference params would carry it, raw pointers
// don't. Rust's FP is strict IEEE like C++ (and stable Rust has no scoped
// reassociation opt-in at all) — reductions stay scalar either way.

use std::slice::from_raw_parts;
use std::slice::from_raw_parts_mut;

#[no_mangle]
pub extern "C" fn rs_dot(xs: *const f64, ys: *const f64) -> f64 {
    let (xs, ys) = unsafe { (from_raw_parts(xs, 256), from_raw_parts(ys, 256)) };
    xs.iter().zip(ys).map(|(a, b)| a * b).sum()
}

#[no_mangle]
pub extern "C" fn rs_book_pressure(bid_sz: *const f64, ask_sz: *const f64) -> f64 {
    let (bid, ask) = unsafe { (from_raw_parts(bid_sz, 64), from_raw_parts(ask_sz, 64)) };
    let b: f64 = bid.iter().sum();
    let a: f64 = ask.iter().sum();
    b / (a + b)
}

#[no_mangle]
pub extern "C" fn rs_vwap(px: *const f64, sz: *const f64) -> f64 {
    let (px, sz) = unsafe { (from_raw_parts(px, 64), from_raw_parts(sz, 64)) };
    let notional: f64 = px.iter().zip(sz).map(|(p, s)| p * s).sum();
    let qty: f64 = sz.iter().sum();
    notional / qty
}

#[no_mangle]
pub extern "C" fn rs_scale_ladder(prices: *const f64, k: f64, out: *mut f64) -> f64 {
    let (prices, out) = unsafe { (from_raw_parts(prices, 256), from_raw_parts_mut(out, 256)) };
    for (o, p) in out.iter_mut().zip(prices) {
        *o = p * k;
    }
    k
}

#[no_mangle]
pub extern "C" fn rs_matvec(m: *const f64, v: *const f64, out: *mut f64) -> f64 {
    let (m, v, out) =
        unsafe { (from_raw_parts(m, 1024), from_raw_parts(v, 32), from_raw_parts_mut(out, 32)) };
    for i in 0..32 {
        out[i] = m[i * 32..i * 32 + 32].iter().zip(v).map(|(a, b)| a * b).sum();
    }
    out[31]
}

#[no_mangle]
pub extern "C" fn rs_decide(pressure: f64, vwap_px: f64, mid: f64, max_size: i64) -> i64 {
    let edge = vwap_px - mid;
    let buy = pressure > 0.6 && edge > 0.0;
    let sell = pressure < 0.4 && edge < 0.0;
    if buy {
        max_size
    } else if sell {
        -max_size
    } else {
        0
    }
}
