# A full tick → trade, in four languages

> **Status: honest draft, not a "win" story.** An adversarial audit corrected
> several overclaims an earlier draft made (see the bottom). What survives is
> modest and true: hotlang *matches* hand-tuned C++/Zig from safe, portable
> source, and beats *strict-IEEE* builds — a win that a single `-ffast-math`
> flag erases. It does **not** beat a tuned expert.

A complete market-making reaction path — book snapshot in, order out — written
in **hotlang, C++, Rust, and Zig**, driven by one shared harness. Files:
[`examples/flow.hot`](../examples/flow.hot),
[`bench/flow_ref.{cpp,rs,zig}`](../bench/),
[`bench/flow_bench.c`](../bench/flow_bench.c),
[`bench/flow.sh`](../bench/flow.sh). It measures the **vectorizable arithmetic
core** of a tick→trade (fused book-pressure + vwap + alpha reductions over 32
levels → branchless decision → sized order) — *not* end-to-end latency, which in
a real system is dominated by non-vectorizable feed decode, book maintenance,
risk, and OMS.

## The numbers, stated honestly

`ns/tick`, best-of median-of-5. **AMD Ryzen 9700X (Zen 5, AVX-512), clang 18:**

| language                         | ns/tick | vs hotlang |
|----------------------------------|---------|------------|
| **hotlang**                      | 7.0     | 1.00×      |
| C++ tuned (`__restrict`+reassoc) | 6.5     | **0.92× — beats hotlang** |
| Zig tuned (`@setFloatMode`)      | 6.5     | **0.93× — beats hotlang** |
| Rust, multi-accumulator (stable) | 8.4     | 1.20×      |
| C++ default (strict-IEEE)        | 18.9    | 2.68×      |
| Rust, single-accumulator         | 18.8    | 2.67×      |
| Zig default                      | 22.7    | 3.22×      |

On Apple M4 the shape holds: hotlang 21.5 ns, ~1.6× over strict-IEEE defaults,
**~10% behind tuned C++ (19.5 ns).**

Read it straight:

1. **hotlang does NOT beat a tuned expert. It trails by ~8–10%** (7.7% Zen5,
   10.3% M4). Tuned C++ and tuned Zig are both faster.
2. **The "3× over default" is a strict-IEEE artifact, not a real win.** Recompile
   the *untouched* default C++ with one `-ffast-math` flag and it drops to
   **6.4 ns** (Zen5) — tying/beating hotlang, zero source changes. Desks build
   hot code with fast-math or `__restrict`; against that, hotlang ties, it
   doesn't win. The "3.22×" is specifically vs *Zig's* default codegen, not C++.
3. **Even "beats Rust" is only vs single-accumulator Rust.** Idiomatic stable
   Rust with a manual accumulator split is 1.20× behind — hotlang gets that
   vectorized shape from the compiler for free; Rust must hand-write it.

## Why the ~8–10% gap — and it is NOT a safety tax

An earlier draft claimed the gap was hotlang's "total-division safety guards."
**That was false, and we struck it.** hotlang's totality guards wrap *integer*
div/rem only; this flow is 100% `f64`, so its divisions are plain `fdiv` —
byte-identical to tuned C++ (hotlang even emits *fewer* total instructions). The
real cause is **branchless-vs-branchy codegen**: hotlang's `if` compiles to a
branchless decision that computes every division and select on every tick, while
tuned C++ branches and skips work on the no-trade path. On this benchmark's
*predictable* period-3 synthetic stream the branch predictor is perfect, so
branchy C++ wins by a hair. On unpredictable real data hotlang's constant-time
path could close or flip that — but we haven't measured that, so we don't claim
it.

(Also corrected: `flow.hot` said an empty book is "total → 0". It isn't — `0/0`
is `NaN`; the order is 0 only because `NaN` fails the `>0.6`/`<0.4` comparisons
and routes to hold. That's a structural NaN-safety property of *this* function,
not a language guarantee for arbitrary float division.)

## What all four *do* share, honestly

- **Same trade every tick** — `qty` agrees to <1e-12 (max 5.6e-13); `side` and
  `price` are identical, but that's near-tautological (price is a raw-input
  passthrough, and every decision sits ~1e12× the reassociation error from its
  threshold). A book straddling `pressure = 0.6` within ~1e-16 *would* legitimately
  emit different trades between strict-IEEE and reassociated builds — we don't
  test that boundary.
- **hotlang's compile-time guarantees** — allocation-free, bounded, in-bounds,
  proven. That is real and it is the actual product. It is not a speed claim.

## The honest bottom line

On a compute-bound arithmetic kernel, hotlang **ties the best hand-tuned C++/Zig
from safe, portable source, and never beats it** — same LLVM, same silicon.
That's worth something (no safe language matches tuned C++ automatically), but it
is not the "beats tuned C++" story, and we won't pretend it is. hotlang's
*outright* wins are elsewhere, where it does less work (incremental streaming,
~30×) or leaves the CPU (Verilog/FPGA) — not here.
