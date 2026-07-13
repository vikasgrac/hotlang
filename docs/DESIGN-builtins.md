# Built-in Data Structures & Configuration

> **Status: `ring` is IMPLEMENTED (see `examples/ring.hot`); `struct` and
> `const` are still DESIGN.** The ring section below reflects the shipped
> feature; the struct/const sections propose the remaining v0.3 set and do
> not compile yet. Everything in [SPEC.md](SPEC.md) is the stable surface.

The v0.2 language proves guarantees over scalar and fixed-array math. To
express a real HFT hot path — ingest ticks, maintain a book, run a
configured strategy — three constructs are needed. Each is designed to
preserve the existing guarantees (zero allocation, bounded execution,
proven bounds, totality) by construction.

The unifying idea: **the data structures are language primitives, so the
compiler can prove their access patterns safe** — exactly the thing a
hand-rolled C ring buffer or a `std::` container cannot offer.

---

## 1. `ring` — a built-in circular buffer for incoming ticks  ✅ IMPLEMENTED

The most common HFT structure: a fixed-capacity circular buffer of the last
*N* items (ticks, prices, sizes). The v0.2 bounds prover could not see
through a `% N` cursor, so users could not build one safely. As a primitive,
the compiler owns the index arithmetic and proves it in-bounds by
construction. Working example: `examples/ring.hot`.

### Syntax (compiles today)

```
// A ring is declared with a power-of-two capacity and an element type.
// Storage is a host-owned, zero-initialized buffer laid out as
// { i64 head; elem data[N] } (like array params, plus the head cursor).
fn on_tick(px: f64, hist: mut ring[f64; 1024]) -> f64 {
    push hist, px;              // O(1); overwrites the oldest when full
    return hist[0];             // hist[0] = newest, hist[1] = previous, ...
}
```

### Semantics

- **Power-of-two capacity** so the wrap is a mask (`idx & (N-1)`), never a
  division — the fast, branchless form, and the reason capacity is
  restricted.
- `push r, v` is a statement (mutation), O(1), overwrites the oldest slot
  when full. Only legal on a `mut ring` parameter.
- `r[k]` reads the k-th most recent element; `k` must be provably in
  `[0, N)`. Because the compiler generates the masked index, **every ring
  access is in-bounds by construction** — no interval-analysis gap.
- A function that `push`es is a *stateful handler*: host-callable, but (like
  any mutating function) it loses `memory(none)` and is not eligible for
  eager-`if` speculation of its call. Rings are single-writer; concurrent
  host access is governed by the Host Contract (§4 below).
- Zero allocation preserved: the ring's storage is a caller-owned buffer,
  same model as array parameters.

### What it unlocks

For **sum-decomposable** statistics (mean, variance/vol via Welford, vwap,
book pressure), the ring gives the leaving element so the running aggregate
updates in **O(1) per tick** — the whole incremental-streaming thesis, now
ergonomic and safe (`examples/ring.hot`, `examples/streaming.hot`).

Honest scope note: the ring gives O(1) *only* for stats where knowing the
entering and leaving element suffices. **Rolling max/min and median are NOT
sum-decomposable** — when the leaving element was the max, you must rescan.
A ring-based `rolling_max` is still an O(W) scan per tick:

```
// O(W) per tick — the ring does NOT make max incremental. For true O(1)
// rolling max you need a monotonic deque (a planned separate primitive);
// this simple form just avoids the manual shift/cursor bookkeeping.
fn rolling_max(px: f64, win: mut ring[f64; 64]) -> f64 {
    push win, px;
    let mut m = win[0];
    for k in 1..64 { m = max(m, win[k]); }
    return m;
}
```

---

## 2. `struct` — flat tick and order records

Market data and orders are records. hotlang structs are **flat, fixed
layout, scalar-field-only** — the flyweight/pre-allocated-buffer pattern as
a type. No pointers, no nesting that could hide allocation or aliasing.

### Proposed syntax

```
struct Tick {
    ts:    i64,     // exchange timestamp (nanos)
    bid:   f64,
    ask:   f64,
    bid_sz: i64,
    ask_sz: i64,
}

struct Order {
    id:    i64,
    px:    f64,
    qty:   i64,
    side:  i64,     // +1 buy, -1 sell, 0 flat
}

fn quote(t: Tick, max_qty: i64) -> Order {
    let mid = (t.bid + t.ask) * 0.5;
    let sz  = min(t.bid_sz, t.ask_sz);
    return Order { id: t.ts, px: mid, qty: min(sz, max_qty), side: 1 };
}
```

### Semantics

- **Fixed layout**: fields laid out in declaration order, natural
  alignment, no padding surprises. The compiler emits a matching C header
  (§3) so host and kernel can never disagree on layout — your old
  hand-synced struct bug, eliminated.
- Passed and returned **by value** (in registers when small, else by hidden
  pointer per the C ABI) — no heap, no ownership questions.
- Field access `t.bid` is a static offset load. No allocation, totally
  analyzable.
- Structs compose with arrays (`[Tick; 256]`) and rings (`ring[Tick; 1024]`)
  as the natural "order book level array" / "tick history" containers.

## 3. Generated interop header

`hotc build strat.hot --emit-header` produces `strat.h`:

```c
typedef struct { int64_t ts; double bid; double ask;
                 int64_t bid_sz; int64_t ask_sz; } hot_Tick;
typedef struct { int64_t id; double px; int64_t qty; int64_t side; } hot_Order;

hot_Order quote(hot_Tick t, int64_t max_qty);
```

The host (C/C++/Rust, or Java via Panama with a generated layout) includes
this; the layout is authoritative and single-sourced from the `.hot` file.

---

## 4. `const` — pre-compiled, configurable strategy parameters

"Configurable but pre-compiled": strategy parameters (thresholds, sizes,
alphas) are declared as module constants and **baked into the binary as
immediate operands** — no config lookup on the hot path, no indirection.
Different configurations produce different specialized binaries.

### Proposed syntax

```
const ENTRY_Z:   f64 = 2.5;
const MAX_QTY:   i64 = 100;
const ALPHA:     f64 = 0.05;

fn signal(z: f64, ewma_prev: f64, px: f64) -> i64 {
    let ewma = ALPHA * px + (1.0 - ALPHA) * ewma_prev;   // ALPHA is an immediate
    return if z > ENTRY_Z { MAX_QTY } else { 0 };        // ENTRY_Z folded in
}
```

### Configuration override at build time

```
hotc build strat.hot --set ENTRY_Z=3.0 --set MAX_QTY=250 -o build/aggressive
hotc build strat.hot --set ENTRY_Z=2.0 -o build/passive
```

### Semantics

- A `const` is a compile-time value of scalar type. It is substituted at
  every use, so it appears in the machine code as an immediate — the
  strategy is *specialized*, not *parameterized at runtime*.
- `--set NAME=VALUE` overrides a declared const at build time (type-checked
  against its declaration). This is the "configurable, pre-compiled" story:
  one source file, N specialized binaries, each with its parameters fused
  in and constant-folded (e.g. `1.0 - ALPHA` becomes a literal).
- Because consts are compile-time, they may appear in array/ring capacities
  and loop bounds — a window size becomes a single configurable knob that
  still yields statically bounded loops.
- Interval analysis sees const values, so more indices become provable and
  more division guards elide — configuration makes the *proofs* tighter,
  not just the code faster.

---

## 5. How it stays honest

Every construct above is designed to keep the v0.2 guarantees:

| Construct | Zero-alloc | Bounded | Bounds-proven | Total |
|-----------|-----------|---------|---------------|-------|
| `ring`    | caller-owned buffer | fixed capacity | masked index, in-bounds by construction | wrap is total |
| `struct`  | flat, by-value, no heap | — | static field offsets | — |
| `const`   | — | enables const loop bounds | tightens interval analysis | — |

None of them introduces a heap, an unbounded loop, or an unprovable access.
That constraint is the whole point: if a proposed feature can't preserve the
guarantees, it doesn't go in the language — it goes to the host.

## 6. Open design questions (feedback wanted)

1. **Ring concurrency**: single-writer/single-reader is the SPSC queue every
   HFT system uses. Should hotlang model the memory ordering (acquire/release
   on the cursor) or delegate entirely to the Host Contract?
2. **Struct arrays vs SoA**: an `[Tick; N]` is array-of-structs; hot loops
   often want struct-of-arrays for vectorization. Auto-SoA transform, or an
   explicit `soa` modifier?
3. **`const` and floats**: build-time float overrides invite "0.1 isn't
   representable" surprises. Reject non-representable literals, or accept and
   document rounding?
4. **Guarded work** (from the review panels): the eager-`if` makes gated
   computation pay full cost. A `when cond { ... }` skippable-work block that
   preserves worst-case bounds is the fourth planned construct — see the
   roadmap in the README.

---

*This is a request for comments. If you run HFT systems and the shapes here
are wrong, [open an issue](https://github.com/vikasgrac/hotlang/issues) —
the point of publishing the design before building it is to be corrected
cheaply.*
