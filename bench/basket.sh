#!/bin/sh
# Build + run the basket/index-arb benchmark (hot path = matvec) across
# hotlang / C / C++ / Rust / Zig. This is the shape where hotlang beats even
# TUNED C++: its per-loop !llvm.loop metadata avoids the register-spill blowup
# the tuned autovectorizer hits on the nested reduction. The win scales with
# vector width — measured 2.1x on M4 NEON; RUN THIS ON ZEN5/AVX-512 for the big
# number (the bare-matvec precedent is 7-8x there).
#
# Usage:  bench/basket.sh          (Linux or macOS; needs clang, clang++, rustc; zig optional)
set -e
cd "$(dirname "$0")/.."
HOTC=compiler/target/release/hotc
OUT=bench/out; mkdir -p "$OUT"

echo "== env =="
uname -srm
clang --version | head -1
rustc --version
if command -v zig >/dev/null 2>&1; then
    ZV=$(zig version)
    echo "zig $ZV"
    case "$ZV" in 0.16.*|0.17.*) echo "  !! zig $ZV disables LLVM loop vectorization upstream — install 0.14.x for a fair Zig column";; esac
else
    echo "zig MISSING (building 4 of 5)"
fi
echo

echo "== build =="
(cd compiler && cargo build --release --quiet)
$HOTC build examples/basket.hot -o "$OUT" >/dev/null
clang   -O3 -march=native -c bench/basket_ref.c   -o "$OUT/basket_c.o"
clang++ -O3 -march=native -c bench/basket_ref.cpp -o "$OUT/basket_cpp.o"
rustc   -C opt-level=3 -C target-cpu=native --edition 2021 --emit=obj --crate-type staticlib \
        bench/basket_ref.rs -o "$OUT/basket_rust.o" 2>/dev/null

ZIGDEF=""; ZIGOBJ=""
if command -v zig >/dev/null 2>&1; then
    zig build-obj -O ReleaseFast -mcpu=native bench/basket_ref.zig -femit-bin="$OUT/basket_zig.o"
    ZIGDEF="-DHAVE_ZIG"; ZIGOBJ="$OUT/basket_zig.o"
fi

clang -O3 -march=native $ZIGDEF bench/basket_bench.c \
      "$OUT/basket.o" "$OUT/basket_c.o" "$OUT/basket_cpp.o" "$OUT/basket_rust.o" $ZIGOBJ \
      -o "$OUT/basket_bench"
echo
echo "== run =="
"$OUT/basket_bench"
