# hotlang End-to-End Simulated Tick-to-Trade Benchmark

## Objective

Measure the real, deployable cost of the hotlang hot path — the full
**host → hotlang → host** round trip per tick, FFI boundary included — under
a realistic simulated market-data feed, and report latency *percentiles*
(not just means), because in HFT the tail is the product.

Per tick, the loop exercises exactly the architecture described in
[hotlang-philosophy.md](hotlang-philosophy.md):

1. Host synthesizes an order-book snapshot (stand-in for feed decode).
2. Host calls the hotlang kernel synchronously on the same thread.
3. Inside that one call: fused depth-book reductions (pressure, VWAP) +
   O(1) rolling volatility of returns + microprice/edge signal +
   branchless decision.
4. The return value (signed qty; 0 = no order) goes to an order-adapter
   stand-in.

Only step 2–3 is timed. All hotlang state is allocated once before the
loop; the timed region performs zero allocation.

## Design

### The kernel — `examples/e2e.hot`

One verified function, `on_tick`, combining the repo's book and streaming
idioms:

- **Book operation**: one fused pass over 32 levels per side computing
  total depth, pressure (bid mass / total mass), and VWAP — intermediates
  never leave registers.
- **Rolling volatility**: Welford-form variance of per-tick *returns*
  (`mid − prev_mid`) over a 256-tick ring window, updated in O(1) from the
  one sample that changed. Returns, not price levels — the noise floor a
  short-horizon strategy actually compares against.
- **Strategy**: edge = microprice skew + ¼·VWAP skew; buy when pressure
  > 0.55 and edge clears the vol floor, mirror for sell. Compiles to
  branchless selects.

The compiler proves the kernel allocates nothing, terminates in bounded
steps, never indexes out of bounds, and is total (an empty book yields 0,
never a fault).

### The simulated feed — deterministic and seedable

Two stages, one seeded xorshift RNG throughout (same seed ⇒ bit-identical
stream on every machine):

**Stage 1 (pre-generated, before timing)** — per-tick scalar processes:

| process | model |
|---|---|
| mid price | random walk; drift re-randomized every 2,000 ticks (regimes) |
| volatility | mean-reverting stochastic process, clamped [0.005, 0.2] |
| half-spread | widens with vol: `tick · (0.5 + 20·vol + noise)` |
| order-flow imbalance | mean-reverting OU process (pressure autocorrelates) + burst events ~1/300 ticks (`imb ± 5`) |

**Stage 2 (in-loop, untimed)** — each tick expands into a 32-level SoA
snapshot: price ladder at `top ± i·0.05`; level sizes
`e^(−0.08·i) · (8 + 40·U)` (exponential depth decay + noise), whole side
scaled `e^(±0.3·imb)` so imbalance appears as size asymmetry. In-loop
expansion keeps memory flat (~5 doubles/tick pre-generated) and gives each
call realistic cache state instead of artificially hot buffers.

*Not modeled (deliberately):* order-by-order add/cancel/execute dynamics,
trades tape, queue position, and tick arrival timing — the loop free-runs
because the objective is per-call latency, not event-time realism. A
captured L2 feed can replace stage 1+2 at the `fill_book` boundary.

### Timing — cycle counters, granularity-aware

- **x86-64**: `rdtscp`, TSC calibrated against `CLOCK_MONOTONIC`
  (~0.3 ns/tick) → **true per-call percentiles**.
- **arm64 (Apple Silicon)**: `cntvct_el0` — but the counter quantizes at
  ~42 ns while the call costs ~16 ns, so naive per-call numbers are
  garbage (they snap to 0 or 42). The harness detects a quantized counter
  at runtime (consecutive reads returning identical values) and switches
  each sample to the **mean of a 64-call block**, labeled in the output.
  Block means keep quantization error < 2% but attenuate single-call tail
  events ~64×.

**Read tail percentiles from the x86 run; treat the mac run as median/
throughput ground truth.** The timer read floor is printed and included in
reported latencies.

### Metrics reported

- Percentiles: min, mean, **p50, p75, p90, p95, p99, p99.9, p99.99**, max
- **Tail jitter** (p99.9 − p50) — the single number an HFT reader looks at
- Stddev; log2 latency histogram (spots bimodality/tail shape at a glance)
- Decisions: buy/sell/hold counts, order rate (~15% with defaults)
- **Decision checksum** — same seed + iters + block mode must produce the
  identical checksum on every platform: proves both binaries computed the
  same trades on the same stream
- Throughput (M ticks/sec over the timed region)

## Artifacts

| file | role |
|---|---|
| `examples/e2e.hot` | the verified hot-path kernel (book + vol + strategy) |
| `bench/e2e_sim.c` | host simulator: feed model, timed loop, stats |
| `bench/e2e.sh` | one-click build + run |
| `bench/out/e2e_sim` | the native executable (produced per machine) |
| `bench/out/e2e.o` | the compiled hotlang kernel object |

## Running it

One click, on each machine (builds natively — run on the mac for the mac
binary, on the ubuntu box for the linux binary):

```sh
./bench/e2e.sh                    # 100,000 timed ticks (default)
./bench/e2e.sh 500000             # custom iteration count
./bench/e2e.sh 100000 5000 7      # iters, warmup ticks, RNG seed
```

Defaults: 100k timed iterations after 5k warmup ticks (excluded from
stats), seed 42.

Prerequisites (same as the other benches): Rust toolchain (`cargo`) and
`clang` on PATH. The script builds the compiler, compiles the kernel, and
links the harness; the built `bench/out/e2e_sim` can then be re-run
directly with the same arguments, no rebuild.

For the cleanest numbers: close heavy apps; on linux prefer
`performance` governor; on macOS disable Low Power Mode. Larger runs
(≥ 1M iters) stabilize p99.99.

### Comparing mac vs ubuntu

1. Run the same command (same iters/warmup/seed) on both.
2. Confirm the **decision checksum** matches (if block modes differ —
   mac 64-call blocks vs x86 per-call — checksums differ by design; force
   comparability by comparing buy/sell/hold counts instead).
3. Compare medians for raw speed; use the **x86 run** for tail
   percentiles.

## Reference result

Apple M4 (macOS, full clock), 100k iters, seed 42, 64-call block mode:
median **~16 ns** per host→hotlang→host round trip, p99 ~23 ns,
throughput ~60 M ticks/sec, order rate 15.4%.

## Honest caveats

- This measures the hotlang call, not wire-to-wire: feed decode and order
  encoding are host stand-ins, deliberately outside the timed region.
- The process is not isolated (no core pinning / IRQ shielding), so the
  far tail (p99.99, max) includes OS scheduling noise — which is also
  informative, but is a property of the box, not the kernel.
- The feed is synthetic; order rate and signal behavior are
  representative, not calibrated to a venue.
