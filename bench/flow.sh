#!/bin/sh
# Build + run the market-making tick->trade flow across hotlang/C++/Rust/Zig.
# All expose the same C ABI and are driven by one shared harness (bench/flow_bench.c).
set -e
cd "$(dirname "$0")/.."
HOTC=compiler/target/release/hotc
OUT=bench/out; mkdir -p "$OUT"

echo "== env =="; uname -srm; clang --version | head -1
command -v rustc >/dev/null 2>&1 && rustc --version || echo "rustc MISSING"
if command -v zig >/dev/null 2>&1; then Z=$(zig version); echo "zig $Z"
  case "$Z" in 0.16.*|0.17.*) echo "  !! zig $Z disables LLVM loop vectorization — use 0.14.x for a fair Zig";; esac
fi
echo

(cd compiler && cargo build --release --quiet)
$HOTC build examples/flow.hot -o "$OUT" >/dev/null
clang++ -O3 -march=native -c bench/flow_ref.cpp -o "$OUT/flow_cpp.o"

RSDEF=""; RSOBJ=""; ZIGDEF=""; ZIGOBJ=""
if command -v rustc >/dev/null 2>&1; then
    rustc -C opt-level=3 -C target-cpu=native --edition 2021 --emit=obj --crate-type staticlib \
          bench/flow_ref.rs -o "$OUT/flow_rust.o" 2>/dev/null
    RSDEF="-DHAVE_RUST"; RSOBJ="$OUT/flow_rust.o"
fi
if command -v zig >/dev/null 2>&1; then
    zig build-obj -O ReleaseFast -mcpu=native bench/flow_ref.zig -femit-bin="$OUT/flow_zig.o"
    ZIGDEF="-DHAVE_ZIG"; ZIGOBJ="$OUT/flow_zig.o"
fi

clang -O3 -march=native $RSDEF $ZIGDEF bench/flow_bench.c "$OUT/flow.o" "$OUT/flow_cpp.o" $RSOBJ $ZIGOBJ -lm -o "$OUT/flow_bench"
echo "== run =="
"$OUT/flow_bench"
