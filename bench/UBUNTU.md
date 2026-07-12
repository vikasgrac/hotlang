# Ubuntu / x86-64 Benchmark Handoff

**For the Claude Code instance on Vikas's Ubuntu box** (Ubuntu 24.04, RTX
5070 Ti, 128 GB RAM — the GPU is irrelevant here, this is a CPU benchmark).
The project folder is synced, so this file and the whole repo are already
present.

## Why this run matters

Every hotlang benchmark number published so far was measured on **one Apple
M4 laptop, in macOS Low Power Mode, at ~2.08 GHz**. An adversarial review
panel (simulated MIT/CSAIL reviewers) flagged this as the single biggest
methodology weakness:

1. The absolute latencies are ~2x understated (full-clock M4 does `dot(256)`
   in ~23 ns, not the ~43 ns in the README — confirmed locally).
2. **Trading desks run x86, not Apple Silicon.** The core win comes from
   hotlang defining FP reduction as reassociable, which lets LLVM vectorize.
   Apple NEON is 128-bit (2 doubles/vector). x86 AVX2 is 256-bit (4), and
   AVX-512 (Zen 4/5, recent Intel) is 512-bit (8). So on the right x86 CPU
   the reassociation advantage could be **larger** than on the Mac — or the
   picture could shift. Nobody has measured it. That's this job.

## What to run

One turnkey script does everything (build compiler, run correctness suite,
build all six contenders, run two benchmark tables):

```bash
cd <repo>            # the synced hotlang folder
# One-time, for stable numbers:
sudo cpupower frequency-set -g performance   # or: sudo apt install linux-tools-common linux-tools-$(uname -r)
bench/bench.sh
```

`bench/bench.sh` prints an **environment block** (CPU model, governor, turbo
state, AVX2/AVX-512 presence, compiler versions) followed by **two tables**:

- **baseline** — plain `-O3` / zig `-mcpu=baseline`. On x86 this is SSE2
  (2-wide), the honest "portable binary" number.
- **native** — `-march=native` (clang), `-C target-cpu=native` (rustc), and
  `-mcpu=native` (zig) applied **equally to all six contenders** (hotlang,
  C++ default, C++ tuned, Rust, Zig default, Zig tuned). This unlocks
  AVX2/AVX-512 fairly for everyone and is the real test of the
  vectorization win at full width.

Prerequisites (install if missing): `cargo`/`rustc` (rustup), `clang`,
`clang++`, `zig` (ziglang.org tarball; set `ZIG=/path/to/zig` if not on
PATH). The script checks and complains if any are absent.

## What to capture and report back

Paste back to Vikas (or write to `bench/RESULTS-x86.md`):

1. The full **environment block** (verbatim — CPU model and AVX level are
   the context for everything).
2. Both **results tables** (baseline + native), ideally the median of the
   3 runs each.
3. Answers to the three questions the script prints:
   - Does hotlang still beat **default** C++/Rust on x86? (Expected: yes,
     and by *more* at `-march=native` if AVX-512 is present.)
   - How big is the reassociation win at native width vs Apple's 128-bit
     NEON? (This is the new, never-measured result.)
   - Does **tuned** C++ still reach parity, or pull ahead on x86?

## Fairness rules (don't break these — the whole brand is honesty)

- The `-march`/`target-cpu` flag must be applied to **hotlang's clang step
  AND every reference compile**, identically. The script does this; don't
  hand-tune one side.
- Don't touch `bench/ref.cpp` (default C++), `bench/ref_tuned.cpp`
  (`__restrict` + `#pragma clang fp reassociate`), `bench/ref.rs`,
  `bench/ref.zig`, or `bench/ref_tuned.zig` (`noalias` +
  `@setFloatMode(.optimized)`) — they are the agreed contenders. If you
  want to try an even-more-tuned variant (hand-written AVX intrinsics),
  add it as a *new* column, clearly labeled, never by editing the
  existing refs.
- Report variance if runs disagree by more than a few %. If the governor
  isn't `performance`, say so — thermal/frequency drift makes numbers
  meaningless otherwise.

## After the run

If the x86 numbers are solid, they should **replace** the Apple-LPM numbers
as the headline (with the environment disclosed), because x86 is the target
platform. Two concrete follow-ups the review panel wants and this run
enables:

- Update `README.md`'s benchmark section: disclose the exact CPU, governor,
  and clock; consider leading with the x86 table.
- If AVX-512 makes `dot` win big, that's a genuinely strong, honest headline
  ("beats default C++ by Nx on AVX-512, and here's exactly why").

There is nothing destructive here — it only builds and runs. Safe to
execute autonomously and report results.
