// e2e_sim.c — end-to-end simulated tick-to-trade latency test.
//
// The loop a real deployment runs, per tick, on one thread:
//   host: synthesize a realistic order-book snapshot (feed decode stand-in)
//   host -> hotlang: on_tick(&ret_hist, state, book..., max_size)
//     hotlang: fused depth reductions + O(1) rolling vol + strategy decision
//   hotlang -> host: signed order qty (0 = no order); host "sends" the order
//
// Only the hotlang call is timed (cycle counter read immediately before and
// after), so what's reported is the host->hotlang->host round trip per tick —
// FFI boundary included — exactly the cost this call adds to a real hot path.
// Feed synthesis runs between calls, untimed, which also keeps the cache
// state per call realistic rather than artificially hot.
//
// Timer granularity is detected at runtime. Where the counter is finer than
// the call (x86 rdtscp: ~0.3 ns), every sample is ONE call — true per-call
// percentiles. Where it is coarser (Apple Silicon cntvct: ~41.7 ns), each
// sample is the mean of a back-to-back block of calls, and the output says
// so — block means attenuate (but still expose) tail events; for true
// per-call tails run this on the x86 box.
//
// All state is allocated ONCE before the loop (see the hotlang host contract);
// the timed region performs zero allocation.
//
// Feed model (deterministic, seedable): mid follows a random walk with
// regime-switching drift and mean-reverting stochastic volatility; the spread
// widens with vol; per-level book sizes decay exponentially from the top with
// noise; order-flow imbalance follows a mean-reverting (OU-style) process so
// pressure autocorrelates like real flow, with occasional burst events.
//
// Usage: e2e_sim [iters] [warmup] [seed]     (defaults: 100000 5000 42)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ---- the hotlang kernel (examples/e2e.hot, C ABI per docs/SPEC.md §12) ----
typedef struct { int64_t head; double data[256]; } ring256;
extern int64_t on_tick(ring256 *ret_hist, double *state,
                       const double *bid_px, const double *bid_qty,
                       const double *ask_px, const double *ask_qty,
                       int64_t max_size);

// ---- cycle-accurate timing --------------------------------------------------
#if defined(__aarch64__)
static inline uint64_t ticks(void) {
    uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)); return v;
}
static double ticks_per_ns(void) {
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return (double)f / 1e9;
}
#elif defined(__x86_64__)
#include <x86intrin.h>
static inline uint64_t ticks(void) { unsigned a; return __rdtscp(&a); }
static double ticks_per_ns(void) {   // calibrate TSC against CLOCK_MONOTONIC
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0); uint64_t c0 = ticks();
    struct timespec req = {0, 200 * 1000 * 1000}; nanosleep(&req, NULL);
    uint64_t c1 = ticks(); clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    return (double)(c1 - c0) / ns;
}
#else
#error "unsupported architecture (need arm64 or x86_64)"
#endif

// A quantized counter (e.g. Apple Silicon cntvct, which steps ~every 42 ns
// regardless of nominal cntfrq) returns IDENTICAL values on consecutive reads
// most of the time; a fine counter (x86 TSC, ~0.3 ns/tick) never does. That —
// not the read cost — is what makes per-call timing of a ~15 ns call garbage.
static int timer_is_coarse(uint64_t *floor_ticks) {
    int zeros = 0; uint64_t g = UINT64_MAX;
    for (int i = 0; i < 100000; i++) {
        uint64_t a = ticks(), b = ticks();
        if (b == a) zeros++;
        else if (b - a < g) g = b - a;
    }
    *floor_ticks = g == UINT64_MAX ? 0 : g;
    return zeros > 1000;                         // >1% identical reads -> coarse
}

// ---- deterministic RNG ------------------------------------------------------
static uint64_t rng_s;
static inline uint64_t xrand(void) {
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s;
}
static inline double u01(void)  { return (double)(xrand() >> 11) / 9007199254740992.0; }
static inline double gauss(void) {           // sum of 12 uniforms - 6: N(0,1)-ish
    double s = 0; for (int i = 0; i < 12; i++) s += u01(); return s - 6.0;
}

// ---- synthetic feed ---------------------------------------------------------
#define LVLS 32
#define TICKSZ 0.05
typedef struct { double mid, half, bimb, aimb; uint64_t noise; } tick_t;

static void gen_feed(tick_t *feed, long n) {
    double mid = 1000.0, vol = 0.02, drift = 0.0, imb = 0.0;
    for (long t = 0; t < n; t++) {
        if (t % 2000 == 0)                       // drift regime switch
            drift = (u01() - 0.5) * 0.004;
        vol += 0.02 * (0.02 - vol) + 0.004 * gauss();  // mean-reverting stoch vol
        if (vol < 0.005) vol = 0.005; if (vol > 0.2) vol = 0.2;
        mid += drift + vol * gauss();            // random walk
        if (mid < 500.0) mid = 500.0;

        imb += 0.05 * (0.0 - imb) + 0.25 * gauss();    // OU order-flow imbalance
        if (xrand() % 300 == 0) imb += (u01() > 0.5 ? 5.0 : -5.0);  // burst event
        if (imb > 8.0) imb = 8.0; if (imb < -8.0) imb = -8.0;

        feed[t].mid  = mid;
        feed[t].half = TICKSZ * (0.5 + 20.0 * vol + u01());   // spread widens w/ vol
        feed[t].bimb = exp( 0.3 * imb);          // bid/ask size multipliers
        feed[t].aimb = exp(-0.3 * imb);
        feed[t].noise = xrand();                 // per-tick level-noise seed
    }
}

// Expand one tick into a 32-level SoA book snapshot (the "feed decode" step).
static void fill_book(const tick_t *tk,
                      double *bp, double *bq, double *ap, double *aq) {
    uint64_t s = tk->noise;
    double top_b = tk->mid - tk->half, top_a = tk->mid + tk->half;
    for (int i = 0; i < LVLS; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        double u1 = (double)(s >> 40) / 16777216.0;
        double u2 = (double)((s >> 16) & 0xFFFFFF) / 16777216.0;
        double decay = exp(-0.08 * i);           // depth decays from the top
        bp[i] = top_b - i * TICKSZ;
        ap[i] = top_a + i * TICKSZ;
        bq[i] = tk->bimb * decay * (8.0 + 40.0 * u1);
        aq[i] = tk->aimb * decay * (8.0 + 40.0 * u2);
    }
}

// ---- stats ------------------------------------------------------------------
static int cmp_dbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double pct(const double *sorted, long n, double p) {
    double idx = p / 100.0 * (double)(n - 1);
    long lo = (long)idx; double frac = idx - (double)lo;
    if (lo + 1 >= n) return sorted[n - 1];
    return sorted[lo] + frac * (sorted[lo + 1] - sorted[lo]);
}

int main(int argc, char **argv) {
    long iters  = argc > 1 ? atol(argv[1]) : 100000;
    long warmup = argc > 2 ? atol(argv[2]) : 5000;
    rng_s       = argc > 3 ? strtoull(argv[3], NULL, 10) : 42;
    if (iters < 1 || warmup < 0) { fprintf(stderr, "bad args\n"); return 1; }
    long total = warmup + iters;

    // -- allocate ONCE (host side of the hotlang contract + harness buffers) --
    static ring256 ret_hist;                     // zeroed static: hotlang state
    static double  state[3];
    static double  bp[LVLS], bq[LVLS], ap[LVLS], aq[LVLS];
    tick_t *feed = malloc((size_t)total * sizeof(tick_t));
    double *lat  = malloc((size_t)iters * sizeof(double));
    if (!feed || !lat) { fprintf(stderr, "out of memory\n"); return 1; }

    printf("hotlang end-to-end simulated tick-to-trade\n");
    printf("  iters=%ld  warmup=%ld  seed=%llu  levels=%d  vol window=256\n",
           iters, warmup, (unsigned long long)rng_s, LVLS);

    gen_feed(feed, total);
    double   tpns = ticks_per_ns();
    uint64_t floor_t;
    // Per-call sampling when the timer resolves single calls; on a quantized
    // counter, blocks of 64 make the quantization error <2% of a sample.
    int B = timer_is_coarse(&floor_t) ? 64 : 1;
    printf("  timer: %.2f ticks/ns, read floor %.1f ns -> %s\n\n",
           tpns, (double)floor_t / tpns,
           B == 1 ? "true per-call sampling (floor included in latencies)"
                  : "quantized counter: each sample = mean of a 64-call block "
                    "(tail events attenuated ~64x — run on x86 for per-call tails)");

    // -- the loop: decode -> call hotlang -> act on decision ------------------
    long buys = 0, sells = 0, holds = 0;
    int64_t qty_checksum = 0;                    // cross-platform reproducibility
    volatile int64_t order_sink = 0;             // stands in for send_order()

    for (long t = 0; t < total; t++) {
        fill_book(&feed[t], bp, bq, ap, aq);     // host: feed decode (untimed)

        uint64_t t0 = ticks();
        int64_t qty = on_tick(&ret_hist, state, bp, bq, ap, aq, 100);
        for (int b = 1; b < B; b++)              // B==1: loop never runs
            qty = on_tick(&ret_hist, state, bp, bq, ap, aq, 100);
        uint64_t t1 = ticks();

        if (qty) order_sink = qty;               // host: order adapter stand-in
        if (t >= warmup) {
            lat[t - warmup] = (double)(t1 - t0) / (double)B / tpns;
            if (qty > 0) buys++; else if (qty < 0) sells++; else holds++;
            qty_checksum = qty_checksum * 31 + qty + 1;
        }
    }
    (void)order_sink;

    // -- report ---------------------------------------------------------------
    double sum = 0, sumsq = 0, mn = 1e18, mx = 0;
    for (long i = 0; i < iters; i++) {
        sum += lat[i]; sumsq += lat[i] * lat[i];
        if (lat[i] < mn) mn = lat[i];
        if (lat[i] > mx) mx = lat[i];
    }
    double mean = sum / (double)iters;
    double sd   = sqrt(fmax(sumsq / (double)iters - mean * mean, 0.0));

    qsort(lat, (size_t)iters, sizeof(double), cmp_dbl);
    static const double PS[] = {50, 75, 90, 95, 99, 99.9, 99.99};
    printf("latency per tick, host -> hotlang -> host (ns)%s:\n",
           B == 1 ? "" : " [64-call block means]");
    printf("  %-8s %10.1f\n", "min",  mn);
    printf("  %-8s %10.1f\n", "mean", mean);
    for (size_t i = 0; i < sizeof(PS) / sizeof(PS[0]); i++) {
        char lbl[16]; snprintf(lbl, sizeof lbl, "p%g", PS[i]);
        printf("  %-8s %10.1f\n", lbl, pct(lat, iters, PS[i]));
    }
    printf("  %-8s %10.1f\n", "max",  mx);
    printf("  %-8s %10.1f   (p99.9 - p50: tail jitter)\n", "jitter",
           pct(lat, iters, 99.9) - pct(lat, iters, 50));
    printf("  %-8s %10.1f\n\n", "stddev", sd);

    // log2 latency histogram — spot bimodality / tail shape at a glance
    printf("distribution:\n");
    long buckets[40] = {0}; int lob = 39, hib = 0;
    for (long i = 0; i < iters; i++) {
        int b = lat[i] < 1 ? 0 : (int)(log2(lat[i]));
        if (b > 39) b = 39;
        buckets[b]++;
        if (b < lob) lob = b; if (b > hib) hib = b;
    }
    long peak = 0;
    for (int b = lob; b <= hib; b++) if (buckets[b] > peak) peak = buckets[b];
    for (int b = lob; b <= hib; b++) {
        int bar = (int)(50.0 * (double)buckets[b] / (double)peak);
        printf("  %6.0f-%-6.0fns |%-50.*s| %6.2f%%\n",
               pow(2, b), pow(2, b + 1),
               bar, "##################################################",
               100.0 * (double)buckets[b] / (double)iters);
    }

    printf("\ndecisions: %ld buys, %ld sells, %ld holds (order rate %.2f%%)\n",
           buys, sells, holds, 100.0 * (double)(buys + sells) / (double)iters);
    printf("decision checksum: %lld  (same seed+iters+block mode must match "
           "across runs)\n", (long long)qty_checksum);
    printf("throughput: %.2f M ticks/sec (timed region, %ld calls total)\n",
           (double)iters * (double)B / (sum * (double)B / 1e9) / 1e6,
           iters * (long)B);

    free(feed); free(lat);
    return 0;
}
