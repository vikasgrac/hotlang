// Zig tuned: noalias + @setFloatMode(.optimized) (full fast-math, more freedom
// than hotlang). CAVEAT: zig 0.16.x disables LLVM loop vectorization (upstream
// regression) so this stays scalar there — use zig 0.14.x for a fair number.
export fn basket_zig(noalias w:[*]const f64,noalias px:[*]const f64,
                     noalias traded:[*]const f64,th:f64,noalias sig:[*]f64) f64 {
    @setFloatMode(.optimized);
    var hits:f64=0;
    for (0..32) |b| {
        var fair:f64=0;
        for (0..32) |k| fair += w[b*32+k]*px[k];
        const e=traded[b]-fair; sig[b]=e;
        hits += if (e > th or e < -th) @as(f64,1.0) else @as(f64,0.0);
    }
    return hits;
}
