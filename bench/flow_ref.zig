// Zig contenders for the tick->trade flow. flow_zig = default; flow_zig_tuned =
// noalias + @setFloatMode(.optimized) (full fast-math, more freedom than hotlang).
// The float mode must sit in the SAME function scope as the reduction, so the
// tuned version spells the body out rather than calling a (differently-scoped)
// helper. Same logic field-for-field as examples/flow.hot. NOTE: zig 0.16
// disables LLVM loop vectorization upstream — use zig 0.14.x for a fair column.

export fn flow_zig(bp:[*]const f64,bq:[*]const f64,ap:[*]const f64,aq:[*]const f64,
                   aw:[*]const f64,ft:[*]const f64,th:f64,base:f64,maxq:f64,out:[*]f64) f64 {
    var bsum:f64=0; var asum:f64=0; var bnot:f64=0; var anot:f64=0; var alpha:f64=0;
    for (0..32) |i| {
        bsum += bq[i]; asum += aq[i];
        bnot += bp[i]*bq[i]; anot += ap[i]*aq[i];
        alpha += aw[i]*ft[i];
    }
    const depth=bsum+asum; const pressure=bsum/depth; const vwap=(bnot+anot)/depth;
    const tb=bp[0]; const ta=ap[0]; const mid=(tb+ta)/2.0;
    const micro=(tb*aq[0]+ta*bq[0])/(bq[0]+aq[0]);
    const edge=alpha+(micro-mid)+(vwap-mid);
    const buy = pressure > 0.6 and edge > th;
    const sell = pressure < 0.4 and edge < -th;
    const side:f64 = if (buy) 1.0 else (if (sell) 2.0 else 0.0);
    const px:f64 = if (buy) ta else (if (sell) tb else 0.0);
    const qty:f64 = if (buy) @min(base+edge,maxq) else (if (sell) @min(base-edge,maxq) else 0.0);
    out[0]=side; out[1]=px; out[2]=qty; return side;
}

export fn flow_zig_tuned(noalias bp:[*]const f64,noalias bq:[*]const f64,noalias ap:[*]const f64,
                         noalias aq:[*]const f64,noalias aw:[*]const f64,noalias ft:[*]const f64,
                         th:f64,base:f64,maxq:f64,noalias out:[*]f64) f64 {
    @setFloatMode(.optimized);
    var bsum:f64=0; var asum:f64=0; var bnot:f64=0; var anot:f64=0; var alpha:f64=0;
    for (0..32) |i| {
        bsum += bq[i]; asum += aq[i];
        bnot += bp[i]*bq[i]; anot += ap[i]*aq[i];
        alpha += aw[i]*ft[i];
    }
    const depth=bsum+asum; const pressure=bsum/depth; const vwap=(bnot+anot)/depth;
    const tb=bp[0]; const ta=ap[0]; const mid=(tb+ta)/2.0;
    const micro=(tb*aq[0]+ta*bq[0])/(bq[0]+aq[0]);
    const edge=alpha+(micro-mid)+(vwap-mid);
    const buy = pressure > 0.6 and edge > th;
    const sell = pressure < 0.4 and edge < -th;
    const side:f64 = if (buy) 1.0 else (if (sell) 2.0 else 0.0);
    const px:f64 = if (buy) ta else (if (sell) tb else 0.0);
    const qty:f64 = if (buy) @min(base+edge,maxq) else (if (sell) @min(base-edge,maxq) else 0.0);
    out[0]=side; out[1]=px; out[2]=qty; return side;
}
