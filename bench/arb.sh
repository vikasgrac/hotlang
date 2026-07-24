#!/bin/sh
# Build + run the feed-to-fire futures-arb benchmark across hotlang/C++/Rust/Zig.
# All four expose the same C ABI (int64_t arb*(const int64_t* tick, int64_t fair,
# int64_t thresh, int64_t* out)) and are driven by the identical C harness.
set -e
cd "$(dirname "$0")/.."
export PATH="/opt/homebrew/bin:$PATH"
HOTC=compiler/target/release/hotc
OUT=bench/out; mkdir -p "$OUT"

echo "== toolchains =="
clang --version | head -1
rustc --version
command -v zig >/dev/null 2>&1 && zig version | sed 's/^/zig /' || echo "zig MISSING (building 3 of 4)"
echo

(cd compiler && cargo build --release --quiet)
$HOTC build examples/arb.hot -o "$OUT" >/dev/null                                   # hotlang -> arb.o
clang++ -O3 -march=native -c bench/arb_ref.cpp -o "$OUT/arb_cpp.o"                   # C++
rustc  -C opt-level=3 -C target-cpu=native -C codegen-units=1 -C panic=abort \
       --edition 2021 --emit=obj --crate-type staticlib \
       bench/arb_ref.rs -o "$OUT/arb_rust.o" 2>/dev/null                            # Rust (fully optimised)

ZIGDEF=""; ZIGOBJ=""
if command -v zig >/dev/null 2>&1; then
    zig build-obj -O ReleaseFast -mcpu=native bench/arb_ref.zig -femit-bin="$OUT/arb_zig.o"
    ZIGDEF="-DHAVE_ZIG"; ZIGOBJ="$OUT/arb_zig.o"
fi

clang -O3 -march=native $ZIGDEF bench/bench_arb.c \
      "$OUT/arb.o" "$OUT/arb_cpp.o" "$OUT/arb_rust.o" $ZIGOBJ -o "$OUT/bench_arb"
echo "== run (median of 5 x best-of-300 batched passes/lang/stream) =="
"$OUT/bench_arb"
