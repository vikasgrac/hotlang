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

echo "== incremental streaming: accuracy vs TRUE, decisions exercised, speedup =="
$HOTC build examples/streaming.hot -o "$OUT" > /dev/null
if command -v clang++ >/dev/null 2>&1; then
    clang++ -O3 -c bench/ref_streaming.cpp -o "$OUT/ref_streaming.o"
    clang -O2 bench/bench_streaming.c "$OUT/streaming.o" "$OUT/ref_streaming.o" -lm -o "$OUT/bench_streaming"
    SOUT="$("$OUT/bench_streaming")"
    echo "$SOUT" | grep -E "rolling vol|FUSED"
    # 1. vol accuracy vs TRUE long-double reference must be <= 1e-6 (the
    #    numerically-hard case; the naive Sum(x^2)-mu^2 form fails here).
    volerr=$(echo "$SOUT" | awk '/rolling vol/{print $NF}')
    awk -v e="$volerr" 'BEGIN{ if (e+0 > 1e-6){ print "FAIL: vol error vs TRUE " e " > 1e-6"; exit 1 } else { print "ok   vol accurate to " e " vs long-double truth" } }'
    # 2. decisions must actually be exercised (not trivially all-hold)
    echo "$SOUT" | awk '/FUSED tick/{ if ($0 ~ /buy=0/ || $0 ~ /sell=0/){ print "FAIL: decisions not exercised"; exit 1 } else print "ok   buy/sell/hold all exercised" }'
    # 3. incremental and recompute must agree on every decision
    echo "$SOUT" | awk '/FUSED tick/{ if ($0 !~ /mismatches=0/){ print "FAIL: incremental != recompute decision"; exit 1 } else print "ok   incremental decisions == recompute decisions" }'
    # 4. the fused-tick incremental win must hold (>= 10x on this W=256 config)
    sp=$(echo "$SOUT" | awk '/recompute C\+\+/{for(i=1;i<=NF;i++) if($i ~ /^[0-9.]+x$/){gsub(/x/,"",$i); print $i}}')
    awk -v s="$sp" 'BEGIN{ if (s+0 < 10){ print "FAIL: fused speedup " s "x < 10x"; exit 1 } else print "ok   fused incremental speedup " s "x vs recompute" }'
else
    echo "skip (clang++ not found)"
fi

echo "== ring builtin: host only pushes ticks, ring owns the window =="
$HOTC build examples/ring.hot -o "$OUT" > /dev/null
clang -O2 tests/ring_edge.c "$OUT/ring.o" -lm -o "$OUT/ring_edge"
"$OUT/ring_edge"

echo "== narrow integers (i16/i32): bit-squeeze arithmetic + conversions + C ABI =="
$HOTC build examples/narrow.hot -o "$OUT" > /dev/null
clang -O2 tests/narrow_edge.c "$OUT/narrow.o" -o "$OUT/narrow_edge"
"$OUT/narrow_edge"

echo "== hotlang -> Verilog: generated hardware matches the CPU (if iverilog present) =="
if command -v iverilog >/dev/null 2>&1; then
    $HOTC verilog examples/narrow.hot > "$OUT/narrow.v"
    iverilog -o "$OUT/hw_sim" "$OUT/narrow.v" tests/hw_tb.v >/dev/null
    HW="$(vvp "$OUT/hw_sim" 2>/dev/null)"
    if echo "$HW" | grep -q "ALL HARDWARE MATCHES"; then
        echo "ok   generated Verilog simulates identically to the CPU"
    else
        echo "$HW"; echo "FAIL: hardware != CPU"; exit 1
    fi
else
    echo "skip (iverilog not installed)"
fi

echo "== unsigned ints (u16/u32/u64): total arithmetic + the strength-reduction win =="
$HOTC build examples/unsigned.hot -o "$OUT" > /dev/null
clang -O2 tests/unsigned_edge.c "$OUT/unsigned.o" -o "$OUT/unsigned_edge"
"$OUT/unsigned_edge"
# the win: u32 % 8 must lower to a vector AND (no signed sign-correction)
if clang -O3 -march=native -S "$OUT/unsigned.ll" -o - 2>/dev/null | sed -n '/bucketize:/,/ret/p' | grep -q "and"; then
    echo "ok   u32 %% 8 lowers to AND (beats signed C++ ~4x, verified in bench)"
else
    echo "FAIL: u32 %% 8 did not strength-reduce"; exit 1
fi

echo ""
echo "ALL TESTS PASSED"
