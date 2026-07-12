#!/usr/bin/env bash
# bench.sh — reproducible hotlang vs C++/Rust benchmark, cross-platform.
#
# Runs TWO tables:
#   baseline : plain -O3 (no -march). On arm64 NEON is baseline so this
#              already vectorizes; on x86-64 this is SSE2 (2-wide) — the
#              honest "portable binary" number.
#   native   : -march=native / -C target-cpu=native applied EQUALLY to
#              hotlang's clang step AND every reference compile. On x86-64
#              this unlocks AVX2/AVX-512 for all four contenders — the fair
#              way to measure the reassociation win at full width.
#
# Everything is fairness-matched: same LLVM backend, same flag on every
# contender, all calls through volatile function pointers.
#
# Usage (from repo root):  bench/bench.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
OUT="$ROOT/hotout"
HOTC="$ROOT/compiler/target/release/hotc"
mkdir -p "$OUT"

say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

# ---- toolchain check ----
for t in cargo clang clang++ rustc; do
    command -v "$t" >/dev/null 2>&1 || { echo "MISSING: $t"; exit 1; }
done

# ---- environment capture (paste this into any results report) ----
say "environment"
uname -srm
if [ -f /proc/cpuinfo ]; then
    echo "CPU:    $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
    echo "cores:  $(nproc) logical"
    gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "unknown")
    echo "governor: $gov  $([ "$gov" != performance ] && echo '<-- run: sudo cpupower frequency-set -g performance')"
    boost=$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo "n/a")
    echo "turbo/boost: $boost"
    for f in avx2 avx512f; do grep -qm1 "$f" /proc/cpuinfo && echo "  has $f" || true; done
else
    sysctl -n machdep.cpu.brand_string 2>/dev/null || true
    echo "(macOS: check Low Power Mode is OFF for full-clock numbers)"
fi
echo "clang:  $(clang --version | head -1)"
echo "rustc:  $(rustc --version)"

# ---- build compiler ----
say "build hotc"
( cd compiler && cargo build --release --quiet )

# ---- run the correctness suite first (a fast build must be a correct build) ----
say "correctness suite"
./tests/run.sh >/dev/null && echo "tests/run.sh: ALL TESTS PASSED"

build_and_run() {  # $1 = label, $2 = extra clang/clang++ flags, $3 = rustc flags
    local label="$1" cflags="$2" rflags="$3"
    say "build ($label)  flags: '${cflags:-none}' / '${rflags:-none}'"
    # hotlang: emit IR once, compile the .ll with the same flags as the refs
    "$HOTC" build examples/book.hot -o "$OUT" >/dev/null
    clang -O3 $cflags -c "$OUT/book.ll" -o "$OUT/book_$label.o"
    clang++ -O3 $cflags -c bench/ref.cpp        -o "$OUT/ref_cpp_$label.o"
    clang++ -O3 $cflags -c bench/ref_tuned.cpp  -o "$OUT/ref_cppt_$label.o"
    rustc -C opt-level=3 -C codegen-units=1 $rflags --crate-type=staticlib \
          bench/ref.rs -o "$OUT/libref_${label}.a" 2>/dev/null
    clang -O2 bench/bench_hft.c \
          "$OUT/book_$label.o" "$OUT/ref_cpp_$label.o" \
          "$OUT/ref_cppt_$label.o" "$OUT/libref_${label}.a" \
          -o "$OUT/bench_$label"
    say "results ($label) — 3 runs, ns/call, lower is better"
    for run in 1 2 3; do
        echo "--- run $run ---"
        "$OUT/bench_$label"
    done
}

build_and_run baseline "" ""
build_and_run native   "-march=native" "-C target-cpu=native"

say "done"
echo "Report back: the environment block above + both results tables."
echo "Key questions: (1) does hotlang still beat DEFAULT C++/Rust on x86?"
echo "               (2) at -march=native, how big is the reassociation win"
echo "                   with AVX2/AVX-512 vs Apple's 128-bit NEON?"
echo "               (3) does tuned C++ still reach parity, or pull ahead?"
