# A Typical HFT Flow in hotlang

This walks a realistic market-making tick-to-trade path and shows, stage by
stage, **what hotlang expresses today** (v0.2, verified, compiles now) and
**what the planned built-ins add** (v0.3, see
[DESIGN-builtins.md](DESIGN-builtins.md)). Code marked ✅ compiles at this
commit; code marked 🔶 is proposed design.

The shape of the path:

```
   market data ──▶ [1] normalize ──▶ [2] update book state ──▶ [3] signals
                                                                    │
   exchange  ◀──── [5] emit order ◀──── [4] strategy decision ◀─────┘
```

hotlang owns stages 1–4 (the compute). The host (Java/C++/Rust) owns the
network, the exchange session, and the buffers — and calls into hotlang per
tick across the C ABI.

---

## Stage 1 — Normalize the incoming tick ✅

Prices arrive as integers (ticks) and sizes as integers; strategy math is in
doubles. The first line of real HFT code is a conversion — which v0.2
expresses with the `f64()` builtin.

```
// compiles today
fn mid_price(bid_ticks: i64, ask_ticks: i64, tick_size: f64) -> f64 {
    let mid_ticks = f64(bid_ticks + ask_ticks) * 0.5;
    return mid_ticks * tick_size;
}

fn notional(qty: i64, px: f64) -> f64 {
    return f64(qty) * px;
}

// microprice: size-weighted mid — where the book is leaning
fn microprice(bid: f64, ask: f64, bid_sz: f64, ask_sz: f64) -> f64 {
    return (bid * ask_sz + ask * bid_sz) / (bid_sz + ask_sz);
}
```

`bid_sz + ask_sz == 0`? Total division returns `0` rather than trapping —
no crash on an empty book.

## Stage 2 — Maintain book / rolling state

**Today (✅), windowed state is a `mut` buffer written shift-style**, because
the bounds prover can't yet see through a ring cursor:

```
// compiles today — O(N) shift, honest but not ideal
fn push_and_vol(rets: mut [f64; 64], newest: f64) -> f64 {
    for i in 0..63 {
        rets[i] = rets[i + 1];       // shift window
    }
    rets[63] = newest;
    let mut s = 0.0;
    let mut s2 = 0.0;
    for i in 0..64 {
        s = s + rets[i];
        s2 = s2 + rets[i] * rets[i];
    }
    let mu = s / 64.0;
    return sqrt(max(s2 / 64.0 - mu * mu, 0.0));   // realized vol
}
```

**Planned (🔶), the `ring` builtin makes this O(1) amortized and the index
in-bounds by construction:**

```
// v0.3 design — DOES NOT COMPILE YET
fn push_and_vol(newest: f64, hist: mut ring[f64; 64]) -> f64 {
    push hist, newest;               // O(1), overwrites oldest
    let mut s = 0.0;
    let mut s2 = 0.0;
    for k in 0..64 {
        s = s + hist[k];
        s2 = s2 + hist[k] * hist[k];
    }
    let mu = s / 64.0;
    return sqrt(max(s2 / 64.0 - mu * mu, 0.0));
}
```

## Stage 3 — Compute signals ✅

The heart of the strategy — and exactly the reduction-shaped math hotlang
compiles into vectorized, branchless native code today. These are from
`examples/book.hot` and `examples/stats.hot`, all verified:

```
// compiles today — linear signal (weights · features), auto-vectorized
fn dot(xs: [f64; 256], ys: [f64; 256]) -> f64 {
    let mut acc = 0.0;
    for i in 0..256 { acc = acc + xs[i] * ys[i]; }
    return acc;
}

// book pressure: bid mass / total mass; >0.5 is bid-heavy
fn book_pressure(bid_sz: [f64; 64], ask_sz: [f64; 64]) -> f64 {
    let mut b = 0.0;
    let mut a = 0.0;
    for i in 0..64 { b = b + bid_sz[i]; a = a + ask_sz[i]; }
    return b / (a + b);
}

// z-score of the latest tick vs window stats (guarded against sigma=0)
fn zscore(x: f64, mu: f64, sigma: f64) -> f64 {
    return (x - mu) / max(sigma, 1e-12);
}
```

And options market-making math — Black-Scholes, written *in* hotlang rather
than linked from a library, verified against textbook values:

```
// compiles today (examples/stats.hot) — normal CDF + BS delta (the hedge ratio)
fn bs_delta(spot: f64, strike: f64, vol: f64, t: f64, rate: f64) -> f64 {
    return norm_cdf(bs_d1(spot, strike, vol, t, rate));
}
```

## Stage 4 — Strategy decision ✅ (with a caveat)

The decision is branchless — good for deterministic latency:

```
// compiles today — fully branchless, compiles to csel chains
fn decide(pressure: f64, vwap_px: f64, mid: f64, max_size: i64) -> i64 {
    let edge = vwap_px - mid;
    let buy  = pressure > 0.6 && edge > 0.0;
    let sell = pressure < 0.4 && edge < 0.0;
    return if buy { max_size } else { if sell { 0 - max_size } else { 0 } };
}
```

**Honest caveat**: `if` evaluates *both* arms. When "do nothing" is the
common case and the action arm is cheap (as here), branchless is a win. When
the action arm is expensive ("compute a 256-wide signal only if a trigger
fires"), hotlang pays that cost every tick — a C++ guard clause would skip
it. This is the deterministic-worst-case / worse-average-case trade; a
`when` guarded-work construct is the top roadmap item.

**Planned (🔶): configurable, pre-compiled thresholds.** The `0.6`, `0.4`,
and sizes become named `const`s, baked into the binary as immediates and
overridable per build:

```
// v0.3 design — DOES NOT COMPILE YET
const BULL_PRESSURE: f64 = 0.6;
const BEAR_PRESSURE: f64 = 0.4;
const MAX_SIZE:      i64 = 100;

fn decide(pressure: f64, edge: f64) -> i64 {
    let buy  = pressure > BULL_PRESSURE && edge > 0.0;
    let sell = pressure < BEAR_PRESSURE && edge < 0.0;
    return if buy { MAX_SIZE } else { if sell { 0 - MAX_SIZE } else { 0 } };
}
```

```
hotc build strat.hot --set BULL_PRESSURE=0.7 --set MAX_SIZE=250 -o build/aggressive
hotc build strat.hot --set BULL_PRESSURE=0.55                    -o build/passive
```

One source file → many specialized binaries, each with its parameters
constant-folded into the instruction stream.

## Stage 5 — Emit the order (host side)

hotlang returns the decision; the host owns the exchange session, order IDs,
and risk gates. **Planned (🔶)**, the order is a hotlang `struct` with a
compiler-generated C header so the host and kernel never disagree on layout:

```
// v0.3 design — DOES NOT COMPILE YET
struct Order { id: i64, px: f64, qty: i64, side: i64 }

fn quote(t: Tick, max_qty: i64) -> Order {
    let mid = (t.bid + t.ask) * 0.5;
    return Order { id: t.ts, px: mid, qty: min(t.bid_sz, max_qty), side: 1 };
}
```

---

## Putting it together — the host loop

Today, a C/C++/Rust host allocates the buffers once at startup and calls
hotlang per tick — zero allocation on both sides of the boundary:

```c
// host (C), buffers allocated ONCE at startup
double bid_sz[64], ask_sz[64], weights[256], features[256];

for (;;) {                                   // per-tick hot loop
    recv_tick(&bid_sz, &ask_sz, ...);        // host: network
    double p   = book_pressure(bid_sz, ask_sz);   // hotlang
    double sig = dot(weights, features);          // hotlang
    int64_t q  = decide(p, vwap, mid, 100);       // hotlang
    if (q) send_order(q);                    // host: exchange session
}
```

From a **Java** host the same shape holds via the Panama FFM API: allocate
`MemorySegment`s once, hand the same segments to every downcall (~tens of ns
per crossing), and keep the JVM's GC entirely off the hot path — the
discipline your Java trading system enforced by hand, now enforced by the
compiler on the hotlang side of the call.

---

## What's real vs. planned, at a glance

| Stage | Today (v0.2 ✅) | Planned (v0.3 🔶) |
|-------|----------------|-------------------|
| 1 Normalize | `f64()`/`i64()` conversions, microprice | — |
| 2 State | shift-style `mut` buffer (O(N)) | `ring` builtin (O(1), in-bounds by construction) |
| 3 Signals | dot / pressure / vol / z-score / Black-Scholes | — |
| 4 Decide | branchless `decide`, literal thresholds | `const` config, `--set` overrides, `when` guarded work |
| 5 Order | scalar return | `struct` + generated C/Java header |

Everything in the ✅ column compiles at this commit and is exercised by
`examples/` and `tests/run.sh`. Everything in the 🔶 column is
[designed and open for critique](DESIGN-builtins.md), not yet built.
