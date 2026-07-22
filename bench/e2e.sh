#!/bin/sh
# One-click end-to-end simulated tick-to-trade latency test.
#
#   ./bench/e2e.sh                 # 100k timed ticks (default)
#   ./bench/e2e.sh 500000          # custom iteration count
#   ./bench/e2e.sh 100000 5000 7   # iters, warmup ticks, RNG seed
#
# Builds natively for the machine it runs on — run it on the mac for the mac
# binary, on the ubuntu box for the linux binary (bench/out/e2e_sim either way).
set -e
cd "$(dirname "$0")/.."
HOTC=compiler/target/release/hotc
OUT=bench/out; mkdir -p "$OUT"

echo "== env =="; uname -srm; clang --version | head -1; echo

(cd compiler && cargo build --release --quiet)
$HOTC build examples/e2e.hot -o "$OUT" >/dev/null
clang -O3 -march=native bench/e2e_sim.c "$OUT/e2e.o" -lm -o "$OUT/e2e_sim"

"$OUT/e2e_sim" "$@"
