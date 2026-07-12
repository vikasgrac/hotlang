#!/bin/sh
# hotlang test runner: compiler build, example pass/fail matrix, division
# edge cases, and the sema/codegen range-consistency regression.
set -e
cd "$(dirname "$0")/.."
HOTC=compiler/target/release/hotc
OUT=tests/out
mkdir -p "$OUT"

echo "== build compiler =="
(cd compiler && cargo build --release --quiet)

echo "== examples that must compile =="
$HOTC check examples/signals.hot
$HOTC check examples/book.hot
$HOTC check tests/div.hot

echo "== examples that must be rejected =="
for f in examples/rejected_recursion.hot examples/rejected_oob.hot; do
    if $HOTC check "$f" 2>/dev/null; then
        echo "FAIL: $f compiled but must be rejected"; exit 1
    else
        echo "ok   $f rejected as expected"
    fi
done

echo "== sema/codegen range consistency (let-bound index) =="
cat > "$OUT/let_index.hot" << 'EOF'
fn f(m: [f64; 1024]) -> f64 {
    let mut acc = 0.0;
    for i in 0..32 {
        let base = i * 32;
        for j in 0..32 {
            acc = acc + m[base + j];
        }
    }
    return acc;
}
EOF
$HOTC build "$OUT/let_index.hot" -o "$OUT" > /dev/null
echo "ok   let-bound index compiles through emit + clang"

echo "== nested inner loops carry !llvm.loop metadata =="
if grep -q 'br label %loop.cond2, !llvm.loop' "$OUT/let_index.ll" \
   && grep -q 'llvm.loop.vectorize.followup_vectorized' "$OUT/let_index.ll"; then
    echo "ok   inner loop tagged (unroll.disable + interleave + followup unroll)"
else
    echo "FAIL: nested inner loop missing !llvm.loop metadata"; exit 1
fi

echo "== division totality (edge cases from a C host) =="
$HOTC build tests/div.hot -o "$OUT" > /dev/null
clang -O2 tests/div_edge.c "$OUT/div.o" -o "$OUT/div_edge"
"$OUT/div_edge"

echo "== proven-safe divisor emits raw sdiv (no guards) =="
if sed -n '/_div_const:/,/ret/p' "$OUT/div.s" | grep -q "csel\|cmp"; then
    echo "FAIL: div_const has guards despite provably safe divisor"; exit 1
else
    echo "ok   div_const is guard-free"
fi

echo "== math builtins + stats (Black-Scholes vs libm ground truth) =="
$HOTC build tests/math.hot -o "$OUT" > /dev/null
$HOTC build examples/stats.hot -o "$OUT" > /dev/null
clang -O2 tests/math_edge.c "$OUT/math.o" "$OUT/stats.o" -o "$OUT/math_edge" -lm
"$OUT/math_edge"

echo "== incremental streaming: correctness + numerical accuracy vs recompute =="
$HOTC build examples/streaming.hot -o "$OUT" > /dev/null
if command -v clang++ >/dev/null 2>&1; then
    clang++ -O3 -c bench/ref_streaming.cpp -o "$OUT/ref_streaming.o"
    clang -O2 bench/bench_streaming.c "$OUT/streaming.o" "$OUT/ref_streaming.o" -lm -o "$OUT/bench_streaming"
    SOUT="$("$OUT/bench_streaming")"
    echo "$SOUT" | grep -E "FUSED|vol|variance"
    echo "$SOUT" | grep -q "MISMATCH" && { echo "FAIL: incremental decision != recompute"; exit 1; }
    # variance is the worst-drift case; assert its error line is <= 1e-6
    verr=$(echo "$SOUT" | awk '/rolling variance/{print $NF}')
    awk -v e="$verr" 'BEGIN{ if (e+0 > 1e-6){ print "FAIL: variance drift " e " > 1e-6"; exit 1 } else { print "ok   incremental accuracy within 1e-6 (variance err " e ")" } }'
else
    echo "skip (clang++ not found)"
fi

echo ""
echo "ALL TESTS PASSED"
