# hotlang

> A language for hot paths. If it compiles, it cannot allocate, cannot loop
> forever, cannot recurse, and cannot index out of bounds. No GC. No JIT
> warmup. No surprises. **It compiles to the code tuned C++ produces — by
> construction, with proofs. You cannot forget the annotations, because
> there are none to forget.**

hotlang is a small, from-scratch language for latency-critical inner loops —
market-data handlers, order books, real-time signal math. It compiles to
native code via LLVM and exports plain C ABI symbols, so the compiled code
can be called from C, C++, Rust, or Java (via the Panama FFM API).

The design bet: instead of *checking* a general-purpose language for latency
sins after the fact, make the sins **inexpressible**, then verify the rest.

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
$ ./compiler/target/release/hotc check examples/signals.hot
$ ./compiler/target/release/hotc emit  examples/signals.hot   # read the LLVM IR
$ ./compiler/target/release/hotc build examples/signals.hot -o hotout
$ clang -O2 bench/bench.c hotout/signals.o -o hotout/bench && ./hotout/bench
```

Requires Rust and clang (any clang can compile LLVM IR text — no LLVM dev
install, no bindings).

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

## The language (v0.2)

- Types: `i64`, `f64`, `bool`, fixed arrays `[f64; 256]` (parameters only).
  No implicit conversions; explicit ones are builtins — `f64(qty) * px` is
  how you write notional, and `i64(x)` truncates toward zero, saturates at
  the i64 range, and maps NaN to 0 (total, like everything else).
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

hotlang is a research project by [Vikas Goenka](https://www.linkedin.com/in/vikasgoenka)
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
