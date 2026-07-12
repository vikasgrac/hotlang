# hotlang — Project Brief

> **hotlang**: not a new language. A strictness mode for Java hot paths. The name is the ambition; the tool is the discipline.

## Background & Motivation

I (Vikas) built a single-digit-microsecond HFT system in Java. The core discipline that made it possible:

- **Zero allocation on the hot path.** The word `new` was effectively banned. All objects created once at startup, pooled and recycled forever. No garbage → GC never runs → no pauses.
- **JIT warmup ritual.** Java compiles hot spots only after observing them run. So every morning the system ran thousands of fake messages through every critical path before real traffic, forcing the HotSpot JIT to compile everything to machine code upfront.
- Flat pre-allocated buffers instead of deserializing into fresh objects (flyweight pattern).

Today this discipline is enforced by code-review folklore and after-the-fact profiling (JMC allocation profiling). Libraries like LMAX Disruptor, Aeron, and Chronicle *enable* the style but cannot *enforce* it. The gap: **compile-time enforcement**, the Rust-borrow-checker move applied to latency.

## Project Vision (long-term, NOT the MVP)

A "strictness mode" for Java — not a new language. Declare constraints (max message rate, max sessions, latency budget); the toolchain computes memory footprint, generates object pools, enforces zero allocation on hot paths, generates warmup harnesses, and optionally AOT-compiles. Think `javac --deterministic`.

## Staged Plan — each stage independently useful and shippable

### Stage 1 — MVP: The Allocation Checker (BUILD THIS FIRST)

A Java annotation processor + bytecode analyzer.

- Annotate a method `@HotPath`.
- The **build fails** if anything reachable from it allocates.
- Must catch not just explicit `new`, but the *invisible* allocations that break zero-alloc discipline in practice:
  - Autoboxing (int → Integer, etc.)
  - Varargs calls (implicit array creation)
  - String concatenation (`+` creating StringBuilder/String)
  - Lambda captures (capturing lambdas allocate)
  - Hidden iterator creation in for-each loops over collections
  - Stream API usage
- Tagline: *"The compiler error I wish I had 15 years ago."*
- First concrete deliverable: a minimal working annotation processor that flags a direct `new` inside a `@HotPath` method, plus test files showing one failing and one passing case. Grow from there.

### Stage 2 — Pool Generation

`@Pooled(max = 100_000)` on a class generates:
- The object pool implementation
- The acquire/release (recycling) protocol
- Reset logic to return objects to a clean state

Mechanical codegen, well-understood patterns.

### Stage 3 — Warmup Harness Generator

Walk the call graph from all `@HotPath` methods and generate a fake-traffic driver that exercises every branch enough times to force JIT compilation before real traffic arrives. Automates the morning warmup ritual.

### Stage 4 — Worst-Case Memory Computation (research-grade, maybe never)

From declared bounds (max messages, max sessions, etc.), compute total static memory footprint at build time. Inspired by SPARK Ada memory-bound proofs, XLA/TensorRT static buffer planning, embedded RTOS static configuration. Do not promise this; treat as an aspiration.

## Open Decisions (decide before/at project start)

1. **Detection strategy for Stage 1:**
   - Option A: Compile-time bytecode analysis via an ASM-based build plugin (Maven/Gradle). Static, catches issues at build time. Known limitation: false positives/negatives around polymorphic calls (virtual dispatch means the callee isn't always statically known — need a policy: conservative failure? annotation on interfaces? allowlist?).
   - Option B: Java agent verifying zero allocation at runtime during a test harness. Precise, but catches problems later.
   - Current lean: **Option A first**, Option B later as a runtime verifier. Both eventually.
2. **Polymorphism policy** (follows from above): how to treat virtual calls from a `@HotPath` — require `@HotPath` on all overrides? Fail conservatively? Configurable?
3. **JDK intrinsics / core library calls**: some JDK methods allocate internally. Need an allowlist/denylist mechanism or transitive bytecode analysis of JDK classes.
4. ~~Project name~~ — DECIDED: **hotlang**. Positioning note: it is not a language (yet); play the contradiction as the brand ("not a language, a discipline") or as aspiration pointing at Stage 4. Repo/artifact naming: `hotlang`, annotation stays `@HotPath`.

## Technical Starting Points

- Annotation processing: JSR 269 (`javax.annotation.processing`) for the annotation + source-level checks; ASM for bytecode-level allocation detection (annotation processors alone can't see all hidden allocations — bytecode analysis is where the real detection lives).
- Alternative to explore: `javac` compiler plugin API (com.sun.source) for AST-level checks (catches string concat and autoboxing at source level, nicer error messages with source positions).
- Likely architecture: Gradle/Maven plugin that runs post-compile, loads class files, walks call graph from `@HotPath` roots, flags allocation opcodes (`NEW`, `NEWARRAY`, `ANEWARRAY`, `MULTIANEWARRAY`) and known-allocating method calls (boxing valueOf, StringBuilder, iterator(), etc.).
- Prior art to study (not copy): Chronicle libraries, LMAX Disruptor, Aeron, Epsilon GC (JEP 318 — no-op GC, useful for runtime verification: run tests under Epsilon with a tiny heap; any allocation eventually crashes the test), Project Valhalla value types (future-proofing consideration).

## Working Style & Constraints

- **Pace: deliberately slow.** A few hours a week. This is a side project; the consulting practice stays priority #1.
- **Every milestone doubles as a LinkedIn post.** The project is a credibility artifact for two audiences: engineering leadership (VP Eng/CTO prospects) and the AI-consulting brand (judgment about when the expensive path is worth it). Even half-finished, it pays for itself in content.
- Milestone = something runnable + a post. Never let it become an invisible multi-month slog.
- Vikas brings: practitioner judgment (which hidden allocations actually bite in production, what real hot paths look like), final calls on all design decisions, Ubuntu workstation (RTX 5070 Ti, 128GB RAM) for local dev.
- Environment: Ubuntu 24.04, expects JDK + Gradle or Maven setup as step zero.

## Definition of Done for the First Session in Claude Code

1. Project scaffolded (Gradle or Maven, pick one — Gradle suggested for plugin ergonomics).
2. `@HotPath` annotation defined.
3. Minimal checker that detects a direct `new` in a `@HotPath` method via ASM and fails the build with a clear error message including class/method/line.
4. Two test cases: one that fails (allocates), one that passes (allocation-free).
5. README with the project vision (can crib from this doc).

Stretch: detect autoboxing in the same session.
