# hotlang — Philosophy and High-Level Spec

> If it compiles, it cannot allocate, cannot loop forever, cannot recurse,
> and cannot index out of bounds — proven by the compiler, not caught in
> code review.

This document is the high-level, prose companion to
[SPEC.md](SPEC.md). It explains *why* the language is shaped the way it
is, where it sits in a trading system, and exactly how control and data
flow between the host and hotlang. For token-level grammar and semantics,
SPEC.md is authoritative.

---

## 1. Motivation

Every HFT shop writes C++ and then spends enormous review effort proving a
negative: that the hot path doesn't allocate, doesn't take a lock, doesn't
recurse, doesn't blow an array bound, doesn't hit a slow libm corner.
General-purpose languages make the latency sins *expressible*, so teams
build linters, review checklists, and tribal knowledge to catch them after
the fact.

hotlang inverts this. It is a deliberately small language in which those
sins are **inexpressible**. There is no heap, so nothing can allocate.
Loop bounds are compile-time literals and the call graph must be a DAG, so
every program terminates in a statically bounded number of steps. Every
array index is proven in-bounds at compile time — no runtime checks, no
possibility of a wild read. Every arithmetic operation is total: nothing
traps, nothing is undefined.

The design bet: instead of *checking* a general-purpose language for
latency sins, make them unwritable — then let the same guarantees unlock
things a general-purpose language can't (see §7, hardware synthesis).

## 2. Design philosophy

- **Subtractive, not additive.** hotlang is not "C++ plus safety." It is a
  tiny language that only admits the kind of code a hot path should
  contain anyway: bounded loops, fixed-size state, straight-line math,
  branchless decisions.
- **A kernel language, not a system language.** hotlang is not your whole
  system. Your feed handlers, order sessions, config, logging, and
  threading stay in C++/Rust/Java. hotlang owns the *think* step —
  everything between a decoded tick and a trade decision.
- **No runtime.** hotlang compiles to plain functions. There is no
  scheduler, no thread, no warmup, no GC — nothing to pin or babysit. A
  hotlang function drops into an existing single-threaded spin loop
  exactly like a hand-written C function.
- **The fast choice is the default choice.** Incremental O(1) streaming
  updates, unsigned/narrow integer types, branchless `if`-expressions —
  the idiomatic hotlang spelling is the one a tuned-C++ expert would have
  hand-picked. You can't forget to.
- **Honesty as a feature.** On an isolated compute kernel, hotlang and
  hand-tuned C++ lower through the same LLVM to the same silicon and
  **tie** — that's physics, and the docs say so. The wins come from doing
  algorithmically less work, reaching substrates C++ can't, and making
  fast defaults unforgettable — not from magic codegen. (The original
  "beats tuned C++" headline was falsified by adversarial review before
  launch; what survived is this story.)

## 3. Where hotlang sits in a trading system

Of the classic tick-to-trade pipeline, hotlang owns the middle:

| Stage | Where it lives |
|---|---|
| NIC / feed decode (ITCH etc. → plain values) | Host (or FPGA) |
| Full-depth book building (order-ID maps, unbounded state) | Host (or FPGA) |
| **Signals: rolling vol, VWAP, microprice, book pressure** | **hotlang** |
| **Strategy decision logic (branchless, bounded)** | **hotlang** |
| Order adapter: session, encoding, socket | Host |

hotlang has **no I/O** — no sockets, no syscalls, no strings, no protocol
encoding. Data enters through function arguments and leaves through the
return value. This is a feature: it is what keeps every kernel a pure,
deterministic function of `(state, tick)`, and it is a precondition for
hardware synthesis.

## 4. Execution flow: host → hotlang → host

There is **no queue, no handoff, no hotlang thread**. A hotlang function
is a synchronous C-ABI call on the host's hot thread. Per tick:

1. **Host** receives the packet and decodes it into plain scalars
   (price, size, side, …).
2. **Host** calls the hotlang-compiled function directly, passing the
   tick fields by value alongside pointers to pre-allocated state:
   `int64_t qty = on_tick(&px_hist, state, bid, ask, bid_sz, ask_sz, max);`
3. **Inside that one call**, hotlang updates its state (ring-buffer
   windows, running stats, book view), computes the signals, runs the
   decision logic, and returns.
4. **The return value is the decision** (e.g. a signed quantity; `0` =
   do nothing). The host checks it and fires the order adapter.

Added latency between "tick decoded" and "decision returned" is exactly
the function's execution time — tens of nanoseconds in the published
benchmarks, including the FFI boundary — with no scheduling, wakeup, or
inter-thread hop.

### Memory model: allocate once, reuse forever

The language has no heap, so **the host allocates all state once at
startup** and passes pointers on every call:

```c
// startup — the only allocation that will ever exist:
static struct { int64_t head; double data[256]; } px_hist;  // zeroed
static double state[2] = {0, 0};

// hot loop, per tick:
int64_t qty = on_tick(&px_hist, state, bid, ask, bid_sz, ask_sz, 100);
if (qty) send_order(qty);
```

Every traversal of the hot path reuses the same memory; the compiler
proves nothing else can be touched (reflected in the emitted IR as
`noalias` parameters and tight `memory(...)` attributes). Because all
state is externally owned plain memory, the host can snapshot it, replay
it in a backtester, or run two strategy variants against identical state.

The host's side of the contract (SPEC §12): exact buffer lengths matching
the declared types, 8-byte alignment, and no aliasing into `mut` buffers.
hotlang's proofs end at the C ABI; these three obligations are what they
were proven against.

## 5. Fixed capacity, variable occupancy

Every array's length is part of its type — `[f64; 64]` is a 64-level book
side, forever. There is no dynamically sized anything. Consequences:

- **Bounds are proven, not checked.** Loops run over literal ranges, so
  every index is provably in-range at compile time; no runtime checks are
  emitted.
- **The memory contract is exact.** The host knows to the byte what to
  allocate at startup.
- **Capacity is a compile-time decision.** Changing book depth means
  recompiling the kernel — a config-time choice, matching how desks treat
  per-venue depth anyway. Need several depths? Compile the function per
  depth: same source, different constant.
- **Fixed capacity ≠ fixed occupancy.** The number of live levels can
  vary tick to tick (tracked via a count or zero sizes); the storage and
  loop bounds never move.

### The truncated-book problem — and the recommended split

Any fixed-depth (top-N) book — hotlang's, or a hardware book builder's —
has the classic truncation problem: an order resting below the horizon
gets repriced into range, or a sweep scrolls unseen levels into view.
This is a property of top-N views, not of hotlang. Two sane designs:

- **Option A (recommended): build the book in the host, feed hotlang the
  view.** The host keeps the full book — order-ID maps, unbounded level
  counts, all the bookkeeping hotlang deliberately can't express — and
  refreshes a fixed top-N snapshot that the kernel consumes. The full
  book is always correct, so the top-N view always is too.
- **Option B: book state inside hotlang, with snapshot correction.**
  Compile two entry points over the *same* host-allocated state:
  `on_update(...)` for the incremental per-tick path and
  `on_snapshot(...)` that overwrites state from a venue snapshot or the
  host's full book. Both are synchronous calls on one thread over one
  memory region, so a snapshot apply is atomic with respect to ticks by
  construction. The refresh itself is a fixed-trip-count copy — **even
  the recovery path has a compile-time-known worst case.** Prefer
  triggered resync (sequence gap, checksum mismatch, crossed-book /
  negative-size sanity checks — themselves expressible in the kernel)
  with a slow periodic refresh as backstop for silent drift; and give the
  decision function a "book suspect" state flag so the strategy widens or
  flattens until the snapshot path clears it.

## 6. Compilation model: a peer of C++/Rust, not a preprocessor

`hotc build` takes `.hot` source through **LLVM straight to a native
object file** (`.o`) exposing plain C-ABI functions. There is no
transpilation to C++ or Rust source — deliberately: generated C++ would
reintroduce exactly the problem the language exists to kill, because once
it's C++ the surrounding build can edit or "fix" it and the proofs no
longer bind the artifact. By owning codegen down to the object file, what
the compiler proved about the source is true of the machine code you link.

Calling it:

- **C/C++** — declare the `extern` prototype, link the `.o`, call it.
- **Rust** — an `extern "C"` block.
- **Java** — JNI or the FFM (Panama) API; it looks like any native library.

## 7. The second target: hardware

The static guarantees are not just safety — they are **exactly the
preconditions for hardware synthesis**. General C++ cannot be synthesized
(unbounded loops, allocation, recursion); every hotlang program clears
those bars by construction. So the same source has two targets:

- `hotc build` → native object via LLVM (CPU hot path);
- `hotc verilog` → a loop-free integer function becomes a **combinational
  circuit, evaluated in one clock cycle**, verified to produce
  bit-identical results to the CPU version.

The workflow this enables: prototype and backtest the decision logic on
the CPU, then push the latency-critical decision into the NIC/FPGA with
no hand translation to HDL. As on the CPU side, the synthesizable piece
is the *decision kernel* — your infrastructure (book builder, feed
decode) maintains state of unbounded shape on either substrate; hotlang's
proven kernel makes the decision from a bounded view of it.

## 8. What makes it fast (the honest version)

- **Incremental streaming is the native idiom.** The `ring` buffer plus
  O(1) rolling-stat updates make the streaming algorithm the safe
  default — measured **~30x** over the stateless recompute a C++ kernel
  library ships, on rolling mean/vol/VWAP ([INCREMENTAL.md](INCREMENTAL.md)).
  Each tick carries O(1) new information; recomputing an O(W) window per
  tick is pure waste, and stateless kernel-library APIs can't avoid it.
- **The natural types are the fast ones.** Prices, sizes, sequence
  numbers, and ring indices are never negative; hotlang's unsigned types
  carry a proven `[0, 2ⁿ)` range, so `x % 8` strength-reduces to a
  vectorized `and` automatically. C++ emits the identical instruction if
  you write `uint32_t` — the claim is about *defaults*, not a faster
  modulo: the naive signed spelling costs ~4x here, and hotlang makes the
  fast spelling the one you reach for.
- **Bit-squeeze.** Narrow integer prices (`i32`/`i16` — a price is an
  integer in ticks) pack 2–4x more per SIMD lane and cache line than
  `f64`; measured ~2.4x on a reduction.
- **Aggressive-but-safe float semantics by default.** hotlang grants the
  optimizer reassociation/contraction (NaN semantics preserved) that
  default-flags C++ withholds — so it often beats *default* C++ 3–10x on
  real kernels, and **ties** C++ tuned with `__restrict` + local
  fast-math pragmas, as it must (same LLVM, same silicon).
- **Leave the CPU entirely** (§7) — the win no C++ flag can reach.

Where there is no algorithmic headroom (positional-weight dot, pure
streaming stores, branchy scalar decisions), hotlang ties tuned C++ at
the roofline — and the benchmarks publish those rows too.

## 9. Language constraints at a glance

The guarantees (SPEC §10), enforced in the type system / verifier:

1. **No allocation** — no heap exists; arrays are caller-owned buffers.
2. **Bounded execution** — literal loop bounds; call graph is a DAG
   (recursion is a compile error naming the cycle).
3. **In-bounds access** — every index proven by interval analysis; an
   unprovable index is a compile error showing the derived range. No
   runtime bounds checks are emitted.
4. **Totality** — no operation traps or is undefined: division is total
   (empty book gives 0, not a fault), float→int conversion saturates,
   `NaN → 0`.

Notable deliberate restrictions:

- No structs, no strings, no I/O, no pointers, no heap.
- Arrays and rings are **parameters only** — never `let`-bound, returned,
  or (currently) passed between hotlang functions; memory always flows in
  from the host.
- `ring[T; N]` (N a power of two) is the tick-stream primitive: O(1)
  `push`, `r[k]` reads the k-th most recent element, in-bounds by
  construction; the language owns the head cursor.
- Math builtins (`sqrt`, `abs`, `min`/`max`, `fma`, `exp`, `log`, …)
  lower to LLVM intrinsics — alloc-free, errno-free — and their names are
  reserved, which also prevents accidentally interposing a host libm
  symbol.
- No implicit numeric conversions; conversions are explicit builtins,
  and integer literals must provably fit the inferred narrow type.

Current v0.2 limitations (roadmap items, honestly listed in SPEC §11):
runtime-valued loop bounds, cross-function array passing, structs, debug
info, multi-module compilation.

## 10. One-paragraph summary

The wire-to-wire path stays in your existing stack. The think step in the
middle — everything between decoded tick and order decision — is a set of
hotlang functions over state your host allocated once at startup. The
compiler proves those functions cannot allocate, loop forever, recurse,
or read out of bounds, and that every operation is total; the artifact is
a plain C-ABI object file your binary links like any other. The same
guarantees make the decision kernel synthesizable, so the same source can
become a one-cycle circuit on an FPGA. On a pure compute kernel it ties
tuned C++ — by physics — and it wins where the language does less work
(O(1) streaming vs recompute), packs data tighter (narrow unsigned
integers in ticks), or leaves the CPU entirely.
