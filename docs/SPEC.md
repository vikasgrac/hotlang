# hotlang Language Specification

Version 0.2 · normative for the compiler at this commit.

This document describes the language that **compiles today**. Planned
features (ring buffers, structs, compile-time configuration) live in
[DESIGN-builtins.md](DESIGN-builtins.md) and are explicitly *not* part of
this spec until implemented.

hotlang is a small, statically-typed language for latency-critical hot
paths. It compiles to native code through LLVM and exports C ABI symbols.
The design goal is that a program which type-checks carries hard
guarantees — no allocation, bounded execution, in-bounds access, total
arithmetic — proven at compile time rather than asserted by convention.

---

## 1. Lexical structure

- **Comments**: `//` to end of line.
- **Identifiers**: `[A-Za-z_][A-Za-z0-9_]*`.
- **Integer literals**: decimal, `_` allowed as a separator (`100_000`).
  Range is that of `i64`; out-of-range is a compile error.
- **Float literals**: decimal with `.` and/or an exponent
  (`1.5`, `0.05`, `1e-12`, `2.5E9`, `1_000.0`). A leading digit is
  required (`.5` is not a float).
- **Boolean literals**: `true`, `false`.
- **Keywords**: `fn let mut return if else for in true false`.
- **Operators/punctuation**: `+ - * / % == != < <= > >= && || ! =
  -> .. ( ) { } [ ] , ; :`.

Whitespace is insignificant except as a token separator.

## 2. Types

| Type          | Meaning                                              |
|---------------|------------------------------------------------------|
| `i16`         | 16-bit signed integer, two's complement (the squeeze)|
| `i32`         | 32-bit signed integer — holds any real price in ticks|
| `i64`         | 64-bit signed integer                                |
| `f64`         | IEEE-754 double                                      |
| `bool`        | `true` / `false`                                     |
| `[T; N]`      | fixed-size array of `N` `T` (`T` ∈ i16/i32/i64/f64), **parameter only** |
| `ring[T; N]`  | circular buffer, `N` a power of two, **parameter only, mutable** |

`N` is a positive integer literal — a compile-time constant. There are no
implicit conversions between types; explicit conversions are the builtins
`f64`, `i16`, `i32`, `i64` (§9).

**Narrow integers** (`i16`/`i32`) are the bit-squeeze lever: half or a
quarter the width of `f64`, so more values pack into each SIMD register and
cache line. Integer literals infer the narrow type of the other operand in a
binary op, if they provably fit (`price / 2` where `price: i32` compiles;
`x + 40000` where `x: i16` is a compile error — the literal doesn't fit).

**`ring`** is the tick-stream primitive: `push r, v` appends in O(1)
(masked wrap), `r[k]` reads the k-th most recent element in-bounds by
construction. See [DESIGN-builtins.md](DESIGN-builtins.md) and
`examples/ring.hot`. Arrays are not first-class
values: they may appear only as parameters, be indexed, and (if declared
`mut`) be stored into. They cannot be bound with `let`, returned, or passed
to another hotlang function (see §11).

## 3. Programs and functions

A program is a sequence of function definitions. There is no other
top-level construct (no global `let`, no imports).

```
fn name(param, param, ...) -> ret_type {
    stmt
    ...
    return expr ;
}
```

- Every function returns a value; the return type is mandatory and cannot
  be an array.
- A parameter is `name : type`, or `name : mut [elem; N]` for a writable
  output buffer. `mut` is legal only on array parameters.
- Function names must be unique, must not shadow a reserved C runtime
  symbol (`main`, `malloc`, …) and must not shadow a builtin (§8).

### Function body

The body is a sequence of statements ending in exactly one `return`. There
is no early return and no `return` inside a loop — a function has a single
exit. Value selection is done with the `if` expression (§7), not control
flow.

## 4. Statements

| Statement                    | Meaning                                        |
|------------------------------|------------------------------------------------|
| `let x = expr;`              | immutable binding; no shadowing                |
| `let mut x = expr;`          | mutable scalar binding (accumulators)          |
| `x = expr;`                  | assignment; `x` must be `let mut`              |
| `arr[idx] = expr;`           | store into a `mut` array parameter             |
| `for i in lo..hi { ... }`    | bounded loop; `lo`,`hi` integer literals, lo<hi|
| `return expr;`               | function exit (final statement only)           |

The loop variable `i` is an immutable `i64` in scope only inside the loop
body, with statically known range `[lo, hi-1]`. Loop bounds are literals,
so every loop's trip count is a compile-time constant.

## 5. Expressions and operators

Precedence, loosest to tightest:

1. `||`
2. `&&`
3. comparisons `== != < <= > >=` (non-associative)
4. `+ -`
5. `* / %`
6. unary `- !`
7. primary: literal, variable, `arr[idx]`, `call(...)`, `( expr )`,
   `if … else …`

`&&` and `||` are **not short-circuiting** — both operands always evaluate
(they lower to single bitwise instructions on `i1`). This is observable and
sound because every hotlang expression is total and side-effect-free.

## 6. Arithmetic semantics

hotlang arithmetic is **total**: every operation is defined on every input.

### Integer (`i64`)

- `+ - *` wrap on overflow (two's complement, like Java). This is a
  deliberate choice, not undefined behavior — the result is always the
  mathematical result modulo 2⁶⁴.
- `/` and `%` are **total**, with no trap and no UB:
  - `a / 0 == 0`
  - `a % 0 == a` (so the identity `a == (a/b)*b + a%b` holds for all `b`)
  - `i64::MIN / -1 == i64::MIN`, `i64::MIN % -1 == 0` (wrapping)
  - Otherwise the usual truncated-toward-zero division.
- The guards implementing this are branchless `select`s. When interval
  analysis proves the divisor cannot be `0` (and the `MIN/-1` pair is
  impossible), the guards are **omitted** and a raw `sdiv`/`srem` is
  emitted — proving safety statically is the zero-overhead fast path.

### Floating point (`f64`)

- `+ - * / %` carry the fast-math flags `reassoc nsz contract`. This means
  the compiler may **reassociate** additions (enabling vectorized
  reductions) and fuse multiply-adds. It does **not** assume the absence of
  NaN or Inf (`nnan`/`ninf` are not set) — NaN and Inf propagate normally.
- Consequence: floating-point results may differ from strict left-to-right
  IEEE evaluation in the low-order bits. hotlang defines FP accumulation as
  reassociable; if you require bit-exact IEEE ordering, hotlang is the
  wrong tool. This is the same tradeoff every hand-vectorized kernel makes.

## 7. The `if` expression

`if cond { a } else { b }` is an **expression**, not a statement. Both arms
are required, must have the same type, and `cond` must be `bool`.

**Both arms are always evaluated**, then the result is chosen with an LLVM
`select` (which lowers to `csel`/`cmov`) — there is no branch. This is why
`if` is "branchless": no branch predictor is involved. The cost model is
therefore *max(cost(a), cost(b))*, not the taken-arm cost. For gated heavy
work ("only compute the signal when the trigger fires") this is a poor fit;
see the roadmap for a planned guarded-work construct.

## 8. Comparison semantics (including NaN)

Integer comparisons are the usual signed comparisons. Float comparisons
follow IEEE-754 and match C/Java:

| Operator | float lowering | `NaN` vs `NaN` | `NaN` vs `3.0` |
|----------|----------------|----------------|----------------|
| `==`     | `fcmp oeq`     | `false`        | `false`        |
| `!=`     | `fcmp une`     | **`true`**     | **`true`**     |
| `<`      | `fcmp olt`     | `false`        | `false`        |
| `<=`     | `fcmp ole`     | `false`        | `false`        |
| `>`      | `fcmp ogt`     | `false`        | `false`        |
| `>=`     | `fcmp oge`     | `false`        | `false`        |

`x != x` is therefore a valid NaN test (true iff `x` is NaN), and `==`/`!=`
are proper complements — identical to C, C++, Rust, and Java.

## 9. Builtins

Math builtins are lowered to LLVM intrinsics (single machine instructions
where the target allows). Their names are reserved.

| Builtin        | Signature                          | Notes                          |
|----------------|-------------------------------------|--------------------------------|
| `sqrt`         | `(f64) -> f64`                      | `fsqrt`                        |
| `abs`          | `(f64) -> f64`, `(i64) -> i64`      | `fabs` / `llvm.abs` (total: `abs(MIN)==MIN`) |
| `min`,`max`    | `(f64,f64)`, `(i64,i64)`            | `fminnm`/`fmaxnm` / `smin`/`smax`; float min/max absorb NaN |
| `fma`          | `(f64,f64,f64) -> f64`              | fused multiply-add             |
| `floor`,`ceil` | `(f64) -> f64`                      |                                |
| `exp`,`log`    | `(f64) -> f64`                      | libm-grade intrinsic, alloc-free, errno-free |
| `pow`          | `(f64,f64) -> f64`                  |                                |
| `f64`          | `(i64) -> f64`                      | exact integer→double           |
| `i64`          | `(f64) -> i64`                      | truncates toward zero, **saturates** at the `i64` range, `NaN → 0` |

Builtin names being reserved also prevents a hotlang module from
accidentally interposing a libm symbol (e.g. `sqrt`) in the host process.

## 10. Static guarantees

For every function that compiles, the verifier establishes:

1. **No allocation.** The language has no allocating construct; there is no
   heap. Arrays are caller-owned buffers passed by pointer.
2. **Bounded execution.** Loops have literal bounds; the call graph must be
   a DAG (recursion is a compile error naming the cycle). Every program
   terminates in a statically bounded number of steps.
3. **In-bounds access.** Every index expression must have a statically
   provable range within its array (interval analysis over loop variables,
   literals, immutable lets, and `+`/`-`/`*` over them, plus `if` unions).
   An unprovable or out-of-range index is a compile error showing the
   range. No runtime bounds checks are emitted.
4. **Totality.** See §6 — no operation traps or is undefined.

These are enforced in the type system / verifier, not by a post-hoc
bytecode pass, and are reflected in the emitted IR as function attributes
(`nounwind willreturn norecurse`, tight `memory(...)`, `noalias` array
params).

## 11. Current limitations

Honest boundaries of v0.2 (each is a roadmap item, not a hidden failure):

- Arrays cannot be passed between hotlang functions, only from the host.
- No structs, no strings, no I/O.
- Loop bounds must be literals (no `0..n` for a runtime `n`).
- Index ranges do not refine through `%`, `min`/`max`, or parameter values
  — so data-dependent indexing (ring-buffer cursors) is not yet
  expressible. Windowed state is written shift-style into a `mut` buffer.
- No debug info is emitted (profilers see bare addresses).
- Single module per compilation.

## 12. Host ABI contract

hotlang's guarantees end at the C ABI. Per call, the host must provide:

1. **Exact lengths** — a `[f64; 256]` parameter points to exactly 256
   `f64`. Bounds were proven against that length.
2. **8-byte alignment** of every array buffer.
3. **No aliasing into `mut` buffers** — a `mut` output array must not
   overlap any other array argument of the same call. Read-only arrays may
   alias each other. (This is what makes every parameter `noalias`;
   violating it is the same class of UB as violating C `restrict`.)

Scalars are passed and returned by value in the natural C types
(`i64`→`int64_t`, `f64`→`double`, `bool`→`_Bool`/`int`). Array parameters
are pointers (`const double*`, or `double*` when `mut`).

---

*This spec is versioned with the compiler. When behavior and spec disagree,
that is a bug in one of them — please file an issue.*
