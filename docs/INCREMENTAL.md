# The Incremental-Streaming Thesis

> hotlang's real performance claim is not "faster codegen than C++." On an
> isolated kernel, the same LLVM backend hits the same silicon roofline —
> hotlang and tuned C++ tie, and we say so. The claim is: **hotlang is the
> language of incremental streaming computation, so the O(1) algorithm that
> a stateless C++ kernel library can't express is the natural, safe default
> — and that beats recompute-style C++ by an order of magnitude on the
> windowed reductions that dominate HFT signals.**

## Two ceilings

There are two different "physics" limits, and conflating them is the mistake:

1. **The hardware roofline for a given algorithm** — peak FMA throughput, L1
   load bandwidth. Genuinely unbeatable on the same backend.
2. **The effective ceiling of the algorithm you chose.** A stateless kernel
   library (`vwap()`, `rolling_std()`, ta-lib, pandas `.rolling()`)
   *recomputes the whole window every tick* — O(W) — because a pure function
   has no memory of the previous call. That runs at the memory roofline,
   which sits **10–200x above the problem's actual information limit.**

Each tick carries only O(1) new information: one sample enters the window,
one leaves. The *incremental* algorithm updates running state in O(1). The
gap between ceiling #2 (recompute) and the O(1) floor is pure waste — and
it's exactly where a streaming language wins.

## The headroom map (Apple M4, tuned C++ recompute vs incremental)

Measured with `bench/bench_streaming.c` over a 2²⁰-tick stream, W=256:

| rolling statistic | recompute (tuned C++) | incremental | speedup | max rel. error |
|-------------------|-----------------------|-------------|---------|----------------|
| variance / vol    | ~26 ns                | ~3.6 ns     | **7x**  | 3.7e-08        |
| mean              | ~28 ns                | ~0.9 ns     | **31x** | 0 (exact)      |
| vwap              | ~26 ns                | ~3.4 ns     | **7x**  | 6.0e-14        |
| book pressure     | ~25 ns                | ~3.2 ns     | **8x**  | 4.7e-15        |
| **fused tick** (vwap+pressure+vol+decision, one call) | **~64 ns** | **~3.9 ns** | **~17x** | decisions identical |

(These per-call numbers include the FFI boundary cost. The pure inlined
algorithmic ratio is larger — 137x for mean, 211x for variance — but ~18x is
the honest deployable figure for a full handler called once per tick. Wins
scale with window size: larger W → larger recompute cost → larger speedup.)

**Where there is NO headroom, and we say so:** `dot` with positional/fixed
weights (when the window slides, every price pairs with a different weight —
truly O(N)), and `scale` (pure streaming store). These are at the roofline;
hotlang ties tuned C++ there.

### The win scales with window size — and has a crossover

Incremental is O(1): its cost is flat (~3.5 ns, dominated by the fixed
decision math — divisions and the sqrt — not the window). Recompute is
O(W). So the speedup is linear in W, and below a small window recompute
actually wins (fused tick handler, Apple M4):

| window W | incremental | recompute | speedup |
|----------|-------------|-----------|---------|
| 8        | 5.5 ns      | 5.2 ns    | **0.9x — recompute wins** |
| 16       | 4.0 ns      | 6.1 ns    | 1.5x    |
| 32       | 3.5 ns      | 10.2 ns   | 2.9x    |
| 64       | 3.6 ns      | 18.8 ns   | 5.2x    |
| 128      | 3.5 ns      | 35.7 ns   | 10.2x   |
| 256      | 3.7 ns      | 65.5 ns   | 17.7x   |
| 512      | 3.7 ns      | 124.7 ns  | 33.6x   |

Honest reading: for **tiny windows (W ≤ ~8–16)** — a handful of recent ticks
— just recompute; the incremental fixed cost isn't worth it. The incremental
win is for **real rolling windows** (moving averages over dozens–hundreds of
ticks, volatility over thousands), where it grows without bound. Pick the
algorithm to fit the window; the language expresses both.

## The honest three-way (this is the whole story)

For the fused tick handler:

| contender | ns/tick | vs recompute-C++ |
|-----------|---------|------------------|
| **hotlang incremental** | ~3.6 | **~18x** |
| hand-written incremental C++ | ~4.5 | ~18x (ties hotlang, ±noise) |
| recompute C++ (kernel-library style, tuned) | ~65 | 1x |

Read it precisely:

- hotlang does **not** beat hand-written incremental C++. Same algorithm,
  same roofline → a tie (hotlang occasionally edges it ~1.2x from the
  `noalias` state buffer, but that's within measurement noise; we claim a
  tie).
- The **18x is incremental-vs-recompute** — an *algorithm* win, available in
  any language.
- hotlang's contribution is that **the incremental algorithm is the natural,
  safe default**: the running-state buffer is `noalias` and bounds-proven by
  construction, and the [`ring` builtin](DESIGN-builtins.md) automates the
  window (which element leaves) with a masked index that is in-bounds by
  construction. A C++ dev *can* hand-roll a stateful ring buffer — and
  maintain the aliasing and bounds invariants by hand, forever. Kernel
  libraries don't; they ship the stateless recompute form. hotlang makes the
  fast form the one you reach for.

This is the same shape as the [fusion result](../README.md): hotlang beats
the code people *actually write* (library recompute), ties the optimal
hand-tuned version, and the real product is *safety + optimal-by-default*.

## Numerical honesty

The standard objection to incremental algorithms is drift: running sums
accumulate floating-point error over millions of updates. We measure it. The
worst relative error over a 2²⁰-tick stream:

- mean: **0** (exact — add then subtract the same magnitudes)
- vwap, pressure: **1e-14 to 1e-15** (near machine precision)
- variance, vol: **~3–7e-08** — the `Σx² − μ²` form has mild cancellation;
  this is still 7–8 correct significant figures, far below tick precision.

For streams longer than ~10⁸ updates, or variance ≪ mean², bound the drift
with a **periodic full refresh** (recompute from the window every K ticks):
O(1) amortized with an O(W)/K correction, still ~18x for large K, and the
error is reset each refresh. A Welford/Kahan compensated update is the
alternative — also O(1), a few more flops. hotlang expresses all of these;
the example uses the simple form and the benchmark proves its error is
acceptable at 2²⁰ ticks.

## What this means for the language

The incremental thesis is *why* the streaming data structures are the core
of hotlang, not conveniences:

- **`ring` builtin** — the window, with O(1) push and masked in-bounds
  indexing, so "what leaves the window" is free and safe.
- **stateful handlers** — running state in a caller-owned buffer that
  persists across ticks (expressible today as a `mut` array; see
  `examples/streaming.hot`).
- **`const` config** — window sizes and thresholds baked in, so the
  incremental update and its bounds are compile-time facts.

Reproduce: `hotc build examples/streaming.hot -o hotout` then build and run
`bench/bench_streaming.c` (commands in its header).
