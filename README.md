# hotlang

> A small, from-scratch language for HFT hot paths. If it compiles, it cannot
> allocate, cannot loop forever, cannot recurse, and cannot index out of
> bounds — proven, not reviewed. No GC, no JIT warmup, no surprises.

hotlang is for latency-critical inner loops — market-data handlers, order
books, real-time signal math. It compiles to native code via LLVM (callable
from C/C++/Rust/Java via the C ABI) — **and, because its guarantees make a
program synthesizable, a subset compiles to hardware (Verilog/FPGA).**

The design bet: instead of *checking* a general-purpose language for latency
sins after the fact, make the sins **inexpressible**, then verify the rest —
and let the same guarantees unlock things a general-purpose language can't.

By [Vikas Goenka](https://www.linkedin.com/in/vikas-goenka-92048512/) — an
ex-HFT engineer. Questions and feedback welcome (issues, PRs, or LinkedIn).

### What makes it fast (honestly)

On an isolated kernel, hotlang and hand-tuned C++ both lower through LLVM to
the same silicon, so they **tie** — that's physics, and we say so. The wins
are where the language does *less work*, reaches a substrate C++ can't, or
makes the fast, safe choice the one you can't forget:

- **Incremental streaming** — the ring buffer + O(1) rolling-stat updates
  make the streaming algorithm the safe default, beating the stateless
  recompute a C++ kernel library ships by **~30x** on rolling signals
  (mean/vol/vwap). [docs/INCREMENTAL.md](docs/INCREMENTAL.md).
- **Bit-squeeze** — narrow `i32`/`i16` integer prices (a price is an integer
  in ticks) pack 2–4x more per SIMD lane and cache line than `f64`;
  measured **~2.4x** on a reduction. [examples/narrow.hot](examples/narrow.hot).
- **Leave the CPU** — `hotc verilog` turns a loop-free integer function into
  a combinational circuit, evaluated in one clock cycle, verified to match
  the CPU. General C++ can't be synthesized (unbounded loops, allocation,
  recursion); hotlang's guarantees are exactly the synthesis preconditions.
  [compiler/src/verilog.rs](compiler/src/verilog.rs).
- **The fast type is the natural one** — prices, sizes, sequence numbers and
  ring indices are never negative, so hotlang's `u16`/`u32`/`u64` carry a
  proven `[0, 2ⁿ)` range as a *checked type fact*. That lets `x % 8`
  strength-reduce to a vectorized `and` and a constant `x / 8` to a shift,
  automatically. This is **not a faster modulo**: write `uint32_t` in C++ and
  it emits the identical instruction and ties us, as it must. The point is
  defaults — the *naive* signed spelling `int x % 8` pays a round-toward-zero
  correction that also halves its vector width, so on 32-bit ints it runs
  ~4.3x slower here (bitwise-identical output). Either one-word fix erases the
  gap (`uint32_t`, or `x & 7`); hotlang's only claim is that the fast,
  non-negative type is the one you reach for by default, and that its
  compile-checked types rule out the divide-by-zero and signed/unsigned-
  comparison bugs that push C++ toward signed in the first place.
  [examples/unsigned.hot](examples/unsigned.hot).

The "faster than C++" headline this README once carried was **falsified by
adversarial AI review** before it went public (tuned C++ ties on a kernel).
What survived is the honest story above. See the benchmark section below.

## The benchmark — including the comparisons that are hard on us

Six columns because fewer would be misleading: **C++** is `clang++ -O3`
with default semantics (what you get without effort); **C++ tuned** is what
a latency-critical desk actually ships — `__restrict` on pointer params
plus a function-local `#pragma clang fp reassociate(on) contract(fast)`
(no `-ffast-math` flag needed, NaN semantics preserved); **Rust** is safe
idiomatic iterator style; **Zig** is `-O ReleaseFast` default semantics;
**Zig tuned** is `noalias` params + `@setFloatMode(.optimized)` — full
fast-math including `nnan`/`ninf`, which is strictly *more* optimizer
freedom than hotlang grants itself. All contenders lower through LLVM
(clang 18 compiles hotlang's IR and the C++ columns; rustc and zig bundle
their own — versions disclosed by `bench/bench.sh`), with the CPU-target
flag tier matched across all six. All calls go through volatile function
pointers.

**AMD Ryzen 7 9700X (Zen 5, AVX-512), governor `performance`,
`-march=native` tier, ns/call, median of 3, lower is better:**

| kernel            | hotlang | C++   | C++ tuned | Rust  | Zig   | Zig tuned | vs C++    | vs tuned C++ |
|-------------------|---------|-------|-----------|-------|-------|-----------|-----------|--------------|
| dot(256)          | 7.3     | 136.9 | 7.3       | 71.5  | 71.5  | 7.3       | **18.8x** | 1.0x         |
| book pressure(64) | 2.3     | 14.4  | 2.6       | 11.6  | 14.4  | 2.2       | **6.2x**  | **1.13x**    |
| vwap(64)          | 2.3     | 20.4  | 2.5       | 20.2  | 17.7  | 2.3       | **9.0x**  | **1.09x**    |
| scale ladder(256) | 6.3     | 6.0   | 6.1       | 6.1   | 6.3   | 6.2       | 0.96x     | 0.96x        |
| matvec(32×32)     | 23.7    | 193.4 | 166.7     | 185.8 | 191.2 | 191.1     | **8.2x**  | **7.1x**     |
| decide (branchy)  | 1.12    | 1.11  | 1.05      | 1.21  | 0.97  | 0.98      | 0.99x     | 0.94x        |
| **full tick pipeline** | **21.3** | **200.3** | **21.4** | **112.2** | **116.5** | **21.7** | **9.4x** | **1.01x** |

**Apple M4 (macOS, Low Power Mode OFF, full clock, NEON 128-bit),
median of 3, ns/call** — the same shape at a narrower vector width:

| kernel            | hotlang | C++   | C++ tuned | Rust  | Zig   | Zig tuned | vs C++   | vs tuned C++ |
|-------------------|---------|-------|-----------|-------|-------|-----------|----------|--------------|
| dot(256)          | 22.1    | 78.4  | 22.3      | 78.0  | 77.4  | 22.2      | **3.5x** | 1.0x         |
| book pressure(64) | 5.8     | 14.2  | 5.9       | 13.8  | 14.2  | 5.8       | **2.4x** | 1.02x        |
| vwap(64)          | 5.8     | 15.9  | 6.0       | 18.6  | 16.5  | 5.9       | **2.7x** | 1.03x        |
| scale ladder(256) | 18.4    | 18.4  | 18.4      | 18.4  | 18.3  | 18.0      | 1.0x     | 1.0x         |
| matvec(32×32)     | 45.5    | 112.4 | 127.0     | 140.3 | 142.9 | 49.4      | **2.5x** | **2.8x**     |
| decide (branchy)  | 0.88    | 1.09  | 1.09      | 1.40  | 1.05  | 1.05      | **1.2x** | **1.2x**     |
| **full tick pipeline** | **51.7** | **161.5** | **52.0** | **158.4** | **160.0** | **52.5** | **3.1x** | **1.01x** |

Same story, smaller multipliers: the reassociation win scales with vector
width (2 doubles on NEON vs 8 on AVX-512), so Zen 5 shows 9.4x where M4
shows 3.1x on the pipeline. The matvec loop-shaping metadata helps *both*
targets — on M4 it takes matvec from 49.6 ns (metadata stripped) to 45.5,
flipping a tie with tuned Zig into a 1.10x win, confirming it is not an
AVX-512-only trick.

(The portable-binary tier — plain `-O3`, SSE2 — and the full six-ratio
tables live in [bench/RESULTS-x86.md](bench/RESULTS-x86.md), environment
verbatim.)

Read the table honestly and it says four things:

1. **hotlang beats the C++, Rust, and Zig people write by default —
   9.4x/5.3x/5.5x on the full tick pipeline** (normalize ladder → vwap →
   book pressure → linear signal → branchless decision), and up to 18.8x
   on a single reduction. The mechanism: hotlang's semantics make FP
   accumulation reassociable (`reassoc nsz contract` — no `nnan`/`ninf`,
   NaNs still work) and every array parameter `noalias` by construction,
   so LLVM vectorizes reductions at full AVX-512 width. Strict IEEE order
   pins C++/Rust/Zig to a serial dependency chain that wider vectors
   cannot help. Stable Rust has no scoped reassociation opt-in at all —
   its ceiling on these kernels is the strict-IEEE number.
2. **A competent C++ dev can reach parity on straight reductions** with
   two annotations per kernel (`__restrict` + the clang fp pragma), and
   tuned Zig with full fast-math can too. Where everyone emits the same
   optimal loop, everyone ties at 1.0x — as they must. hotlang's claim is
   not that annotations can't exist; it's that in hotlang they are
   *impossible to forget*, on every function, uniformly, with a verifier
   proving the aliasing and totality facts the annotations merely assert.
   In C++ the annotations are unchecked promises — get `restrict` wrong
   and you've bought UB, not speed.
3. **Where hotlang beats even the tuned columns** — matvec, 7-8x against
   *both* tuned C++ and tuned Zig. At AVX-512, LLVM flattens the nested
   reduction and SLP-vectorizes it into a 32-broadcast register-spill
   catastrophe — tuned C++ and tuned Zig hit exactly this today (191ns vs
   their own 139/66ns at the portable tier). hotc emits per-loop
   `!llvm.loop` policy metadata (keep innermost nested reductions rolled,
   interleave 4 accumulators, unroll the vectorized loop) — something it
   can do *because* trip counts and aliasing are compile-time facts of the
   language, and something no pragma a C++/Zig dev actually ships
   expresses. It also edges tuned C++ on pressure (1.13x) and vwap (1.09x).
4. **Where hotlang loses, we publish it**: `scale` (pure streaming stores,
   all six columns within ~5%, no language freedom to exploit) and
   `decide` against Zig's branchy code (0.87x) — on a deliberately
   branch-predictable input pattern that is hotlang's documented worst
   case (a predictable branch costs ~0; a branchless select always pays
   both arms; real market data predicts worse). Tuned Zig also edges the
   pipeline by ~3% at the portable tier, powered by the `nnan`/`ninf`
   assumptions hotlang refuses (NaN-in ⇒ UB-out is not "total").

One design honesty note: `if` arms are always evaluated (that is what
branchless means). When one arm carries heavy work — "compute the full
signal only if the trigger fires" — hotlang pays the full cost every tick,
where a C++ guard clause skips it. Deterministic worst-case, worse average
case: that trade is the point of the language, and you should know you're
making it. A skippable-work construct is on the roadmap.

Reproduce with one command: `bench/bench.sh` (builds the compiler, runs the
correctness suite, builds all six contenders at both flag tiers, prints the
environment block and three runs of each table; report the median). Within
a table run, each cell is timed exactly once and ratios are computed from
the printed cells.

### A defaults footnote: what C++'s signed default costs on `% 8`

This is not a cross-language speed win — it belongs here only as a caution
about default types. `bucketize` is `out[i] = price[i] % 8` over 4096 `u32`
elements (Apple M4, best-of-9 ns/call, bitwise-identical output across every
row; harness `bench/bench_unsigned.c`):

| contender                                             | ns/call | vs hotlang  |
|-------------------------------------------------------|---------|-------------|
| **hotlang `u32`**                                     | **~140**| —           |
| C++ `uint32_t` (the type fix)                         | ~145    | ~1.0x (tie) |
| C++ `int` with `x & 7` (the pow-2 mask idiom)         | ~145    | ~1.0x (tie) |
| C++ `int` + `__builtin_assume(x>=0)` (UB if negative) | ~145    | ~1.0x (tie) |
| C++ `int`, naive `x % 8` (signed default)             | ~625    | **~4.3x**   |

Read it honestly: **hotlang's modulo is not faster than C++'s modulo.**
Given the same type they emit the same instruction and tie — physics. The
gap is entirely the *default type* on one specific spelling. Signed
`int % 8` must round toward zero for negative dividends, so clang emits a
correction sequence (`cmlt/usra/bic/sub`) and — the part worth stating
precisely — vectorizes it only 2-wide (`.2s`) instead of the 4-wide (`.16b`)
single `and` the unsigned form gets. It is *not* unvectorized; half the lane
width plus the per-element correction is the ~4.3x.

The gap needs *two* naive choices, and either one-word fix erases it: switch
the type to `uint32_t`, **or** keep `int` and write `x & 7` (a power-of-two
bucket is a mask) — signed `& 7` vectorizes to the same `and` and ties. So
the ~4.3x is against the naive `signed % 8` spelling specifically, not
against competent C++.

Two honest scopings:

- **This is the 32-bit-`int` number only.** Signed `int64_t % 8` still pays a
  correction but a smaller ~1.8x vs `uint64_t` (the 64-bit fix-up is cheaper
  relative to the work), and it applies only to *integer* power-of-two
  `%`/`÷` — hash buckets, ring slots, histogram bins, seqnum masks. If the
  quantity lives in a `double`, this doesn't apply in either direction:
  `x % 8` becomes `fmod`, a libcall no compiler strength-reduces (~280x
  slower here), and neither side gets the `and`.
- **Who actually ships the slow spelling.** General-purpose C++ leans signed
  by default — Google's C++ Style Guide says not to use unsigned merely to
  assert non-negativity, and the C++ Core Guidelines agree (ES.102 "use
  signed types for arithmetic", ES.106 "don't try to avoid negative values by
  using unsigned"). So the naive idiom is common. But performance-critical
  code — including trading systems, where ITCH prices are already `uint32`
  and ring masks are unsigned — often carries these quantities unsigned, in
  which case it ties us, as it should. A desk that profiled this loop would
  just write `& 7`.

hotlang's actual contribution here is narrow and worth stating precisely: it
makes the fast, non-negative type the natural one, and its type checker
removes two of the footguns that push C++ back to signed — divide/modulo is
total (`a/0==0`, `a%0==a`, no UB) and a signed/unsigned comparison is a
compile error, not a silent bug. It does **not** remove the third: unsigned
subtraction still wraps on underflow, exactly as in C++
(`u32(5) - u32(10) == 4294967291`). We don't check that, and we don't claim
to.

## Guarantees (v0.2)

Every function that compiles is:

- **Allocation-free** — the language has no allocating construct. No heap
  exists. Zero garbage, so there is nothing to collect. Arrays live in
  caller-provided buffers; writes go only to `mut` output parameters.
- **Bounded-execution** — loop bounds are compile-time constants and the
  compiler rejects any cycle in the call graph (the eBPF-verifier move).
  Every program provably terminates in a statically known number of steps.
- **Bounds-proven** — every array access is proven in-bounds at compile
  time by interval analysis over index expressions. No runtime checks,
  no UB: the third option is "the program doesn't compile."
- **Total** — every operation is defined on every input. Integer division
  cannot fault or hit UB: `a / 0 == 0`, `a % 0 == a`, `MIN / -1 == MIN`
  (so `a == (a/b)*b + a%b` holds everywhere). The guards are branchless
  selects — and when interval analysis proves the divisor safe (e.g. a
  constant, or a range excluding zero), they are omitted entirely and you
  get raw `sdiv`. Proving safety statically is the fast path.
- **Branch-honest** — `if` is an expression and compiles to LLVM `select`,
  which becomes `csel`/`cmov`: no branches, no branch-predictor roulette.
- **Optimizer-verified** — every function gets LLVM attributes
  (`nounwind willreturn norecurse`, tight `memory`, `noalias` params)
  that the verifier has already proven, which LLVM exploits ruthlessly.
- **Loop-shaped** — because every trip count is a compile-time fact, the
  compiler also tells LLVM *how to spend* that freedom: innermost loops of
  nests carry `!llvm.loop` policy metadata (rolled reduction → interleaved
  accumulators → unroll the vectorized loop), which is what wins matvec
  7-8x against tuned C++ and tuned Zig at AVX-512 instead of losing 3x to
  a register-spill blowup.

## Show me

```text
// signals.hot
fn spread(bid: i64, ask: i64) -> i64 {
    let raw = ask - bid;
    return if raw < 0 { 0 } else { raw };
}
```

```console
$ hotc build examples/signals.hot -o hotout
```

```asm
_spread:                         ; arm64, clang -O3
    sub  x8, x1, x0
    bic  x0, x8, x8, asr #63     ; branchless max(0, x)
    ret
```

Two instructions. And the recursion that a Java checker would only catch in
review folklore:

```console
$ hotc check examples/rejected_recursion.hot
error: recursion detected: feedback -> echo -> feedback
  --> examples/rejected_recursion.hot:10:39
   |
10 |     return if depth <= 0 { x } else { feedback(depth, x) };
   |                                       ^
note: hot paths must have statically bounded execution time; hotlang rejects all recursion at compile time
```

## Measured (Apple M-series, 100M calls through a volatile function pointer)

> Disclosure: the scalar table below was measured in macOS Low Power Mode
> (~2.08 GHz); absolute latencies are ~2x understated vs full clock. Being
> re-measured; ratios are unaffected.

| function     | ns/call | notes                                    |
|--------------|---------|------------------------------------------|
| `spread`     | ~1.4    | 2 instructions                           |
| `clamp`      | ~1.4    | branchless `csel`, immune to bad inputs  |
| `microprice` | ~1.5    | 4-operand size-weighted mid              |
| `ewma`       | ~4.9    | serial FP dependency chain (honest cost) |

These numbers include the function-call overhead; link-time inlining makes
them effectively free inside a real host loop.

## Quickstart

```console
$ cd compiler && cargo build --release       # zero dependencies
$ cd ..
$ ./compiler/target/release/hotc check   examples/signals.hot
$ ./compiler/target/release/hotc emit    examples/signals.hot   # LLVM IR
$ ./compiler/target/release/hotc build   examples/signals.hot -o hotout
$ ./compiler/target/release/hotc verilog examples/narrow.hot    # -> hardware
$ clang -O2 bench/bench.c hotout/signals.o -o hotout/bench && ./hotout/bench
```

Requires Rust and clang (any clang can compile LLVM IR text — no LLVM dev
install, no bindings). `tests/run.sh` runs the full suite; the hardware test
also runs if `iverilog` is installed.

## Documentation

- **[Language Specification](docs/SPEC.md)** — the normative spec of what
  compiles today: grammar, types, arithmetic/NaN semantics, guarantees,
  builtins, and the host ABI contract.
- **[A Typical HFT Flow](docs/HFT-FLOW.md)** — a market-making tick-to-trade
  path stage by stage, marking what's real (v0.2) vs. planned (v0.3), with
  verified code.
- **[Design RFC: Built-in Data Structures & Configuration](docs/DESIGN-builtins.md)**
  — the v0.3 proposal: a built-in `ring` buffer for ticks, flat `struct`
  records for ticks/orders with generated headers, and `const` +
  build-time `--set` for pre-compiled, configurable strategy parameters.
  Design open for critique; not yet implemented.

## The language

- Types: `i16`/`i32`/`i64` signed, `u16`/`u32`/`u64` unsigned, `f64`,
  `bool`; fixed arrays `[T; N]` and the `ring[T; N]` circular buffer
  (parameters only). Narrow `i16`/`i32` are the bit-squeeze lever (integer
  prices in ticks). The unsigned types carry a proven `[0, 2ⁿ)` range, so
  `%`/`÷` by a power of two strength-reduce (to `and`/shift) and vectorize;
  division is total (`a/0==0`, `a%0==a`) and a signed/unsigned comparison is
  a type error, not a silent conversion — making unsigned the natural type
  for non-negative prices, sizes, and indices (subtraction still wraps on
  underflow, as in any language). No implicit conversions; explicit ones are
  builtins — `f64(qty) * px` for notional, `i32(x)`/`i16(x)`
  saturating/truncating, all total. Integer literals infer the type of the
  other operand when they provably fit (`price / 2`).
- `ring` is the tick-stream primitive: `push r, v` (O(1), masked wrap),
  `r[k]` reads the k-th most recent element in-bounds by construction — the
  language owns the window, so incremental streaming is safe and default.
- `fn name(a: i64, xs: [f64; 64], out: mut [f64; 64]) -> f64 { ... }` —
  every function returns a value; `mut` arrays are the only write targets.
- Immutable `let` bindings; `let mut` for loop accumulators; one final
  `return` (single exit, no `return` inside loops).
- `for i in 0..256 { ... }` — bounds are integer literals, trip counts are
  compile-time facts.
- `arr[i]`, `arr[i + 1]`, `arr[2 * i + j]` — indexes built from loop
  variables, integer literals, immutable `let`s of those, and `+`/`-`/`*`
  arithmetic over them (plus `if`-expression unions). Out-of-range is a
  compile error showing the proven range. Known limit: the prover does not
  yet refine through `%`, `min`/`max`, or parameter values — so
  data-dependent indexing (ring-buffer cursors) is currently inexpressible;
  windowed state is written shift-style into a `mut` buffer instead.
- `if cond { a } else { b }` is an expression (both arms required, same type).
  **Both arms are always evaluated** (that's what makes it branchless) —
  sound because every hotlang expression is total and side-effect-free.
- `&&` and `||` likewise evaluate both operands (no short-circuit): they are
  single bitwise instructions, not hidden branches.
- Functions call other functions in the same module; the whole call graph is
  visible to the verifier.
- Numeric literals take `_` separators: `100_000`.
- Function names may not shadow C runtime symbols (`main`, `malloc`, ...) —
  exported symbols are the source names.

## Math builtins — port the formulas, don't link the library

`sqrt`, `abs`, `min`, `max`, `fma`, `floor`, `ceil`, `exp`, `log`, `pow`,
plus the conversions `f64(i64)` and `i64(f64)` — type-overloaded
(`i64`/`f64` where sensible) and lowered to LLVM intrinsics. `sqrt`/`abs`/`min`/`max`/`fma`/`floor`/`ceil` compile to single
arm64 instructions (`fsqrt`, `fabs`, `fminnm`, `fmadd`, ...); `exp`/`log`/
`pow` lower to allocation-free, errno-free libm-grade intrinsics — the only
calls hotlang ever emits. All total: `abs(MIN) == MIN`, `min`/`max` absorb
NaN. Builtin names are reserved, so a hotlang module can never shadow libm
symbols in the host process.

The philosophy for everything above the primitives: **port the math into
hotlang instead of calling out to a library**. `examples/stats.hot` is a
quant math library written in hotlang — mean/variance/realized vol,
z-scores, normal PDF/CDF (Zelen–Severo polynomial, |err| < 7.5e-8), and
Black-Scholes price/delta verified against textbook values from a C host
(`tests/math_edge.c`). Ported formulas inline into their call sites,
vectorize inside loops, and carry the full verifier guarantees; a library
call boundary can do none of that. Roadmap: `hotport`, a converter that
transpiles pure scalar C/Java math functions into `.hot` automatically, and
an `extern pure` FFI contract for the rare case where linking is
unavoidable.

## Host contract

hotlang guarantees end at the C ABI; the host owes three things per call:

1. **Exact lengths** — a `[f64; 256]` parameter is a pointer to exactly 256
   doubles. The compiler proved every access in-bounds *against that length*.
2. **8-byte alignment** — array buffers must be 8-byte aligned (any
   `malloc`/`new double[n]`/Java `MemorySegment.allocate(n*8, 8)` qualifies).
3. **No aliasing into `mut` buffers** — a `mut` output array must not
   overlap any other array argument of the same call. Read-only arrays may
   alias each other freely. (This is what lets every parameter be `noalias`;
   violating it is the same UB as violating C `restrict`.)

Java hosts: allocate once at startup with `MemorySegment`, hand the same
buffers to every call — zero-allocation on both sides of the boundary.

## Architecture

```
.hot source ──lexer──▶ tokens ──parser──▶ AST ──sema──▶ verified AST
                                                      │
                                    (types, returns,  │
                                     recursion ban)   ▼
                                              LLVM IR (.ll text)
                                                      │
                                            clang -O3 │
                                                      ▼
                                     .s  +  .o  +  .dylib/.so (C ABI)
```

The compiler (`hotc`) is ~2,500 lines of dependency-free Rust: hand-written
lexer, recursive-descent parser, type checker + bounded-execution verifier
(interval analysis for bounds and division-safety proofs), and a textual
LLVM IR emitter (including the per-loop vectorization-policy metadata). clang does the last mile, so there is no LLVM linkage to
fight. `tests/run.sh` runs the pass/fail example matrix and division
edge-case suite.

## Roadmap

- **v0** — scalar functions, branchless conditionals, the verifier. ✅
- **v0.2 (this)** — bounded loops, fixed arrays, compile-time bounds proofs,
  `mut` output buffers; beats C++/Rust on reduction-shaped kernels. ✅
- **v1** — structs, array passing between hotlang functions, worst-case
  stack depth computed at build time; a guarded/skippable-work construct
  (deterministic-tail by default, skip-when-provably-idle opt-in); index
  prover refinement through `%` and `min`/`max` (unlocks ring buffers).
- **v2** — host embedding story: Java Panama bindings + a shared-memory ring
  buffer handoff pattern; Rust/C++ header generation.
- **v3** — statically planned scratch memory: declare bounds, the compiler
  computes the total memory footprint at build time (the SPARK Ada move).

## Why not just Rust/C++?

You can write this discipline in Rust or C++ — with vigilance, code review,
and profiling. hotlang's claim is narrower and stronger: in the subset that
matters for the innermost loop, the discipline is *the type system*. The
compiler is the reviewer that never gets tired.

## Status, license, provenance

hotlang is a research project by [Vikas Goenka](https://www.linkedin.com/in/vikas-goenka-92048512/)
— an ex-HFT engineer's answer to "what if the zero-allocation discipline
lived in the compiler instead of code review?" It is not production
software and ships with no support SLA. Issues and PRs are welcome;
response times follow a side-project cadence.

Licensed under [Apache-2.0](LICENSE). Built with AI assistance (Anthropic
Claude) under human direction — including the adversarial review panels
that falsified this README's original headline claim and forced the honest
benchmark table above. We consider that provenance a feature: every claim
here survived an attack, or was reworded until it did.

## Lineage

Born from building single-digit-microsecond trading systems in Java, where
zero-allocation style and JIT warmup rituals were enforced by folklore and
profiling. hotlang inverts that: the guarantees move into the compiler.
Influences: eBPF's verifier, SPARK Ada's static bounds, Halide/kdb+ as
domain languages, Rust as implementation substrate.
