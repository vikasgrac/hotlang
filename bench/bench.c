// bench.c — calls hotlang-compiled native code through a volatile function
// pointer (defeats inlining/constant-folding so we measure real calls).
//
// Build:
//   hotc build examples/signals.hot -o hotout
//   clang -O2 bench/bench.c hotout/signals.o -o hotout/bench
//   ./hotout/bench

#include <stdint.h>
#include <stdio.h>
#include <time.h>

// hotlang exports plain C ABI symbols.
extern double ewma(double prev, double price, double alpha);
extern double microprice(double bid, double ask, double bid_sz, double ask_sz);
extern int64_t clamp(int64_t x, int64_t lo, int64_t hi);
extern int64_t mid(int64_t bid, int64_t ask);

typedef double (*ewma_fn)(double, double, double);
typedef double (*micro_fn)(double, double, double, double);
typedef int64_t (*clamp_fn)(int64_t, int64_t, int64_t);

static ewma_fn volatile ewma_p = ewma;
static micro_fn volatile micro_p = microprice;
static clamp_fn volatile clamp_p = clamp;

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

#define ITERS 100000000LL

int main(void) {
    // --- ewma over a synthetic tick stream ---
    double p = 100.0;
    double t0 = now_ns();
    for (long long i = 0; i < ITERS; i++) {
        double price = 100.0 + (double)(i & 1023) * 0.01;
        p = ewma_p(p, price, 0.05);
    }
    double t1 = now_ns();
    printf("ewma        : %6.2f ns/call   (sink=%f)\n", (t1 - t0) / ITERS, p);

    // --- microprice ---
    double m = 0.0;
    t0 = now_ns();
    for (long long i = 0; i < ITERS; i++) {
        double bid = 100.0 + (double)(i & 255) * 0.01;
        double ask = bid + 0.02;
        m += micro_p(bid, ask, (double)(1 + (i & 7)), (double)(1 + ((i >> 3) & 7)));
    }
    t1 = now_ns();
    printf("microprice  : %6.2f ns/call   (sink=%f)\n", (t1 - t0) / ITERS, m);

    // --- branchless clamp with unpredictable input ---
    int64_t acc = 0;
    t0 = now_ns();
    for (long long i = 0; i < ITERS; i++) {
        // xorshift-ish scramble → branch predictor gets no help; select/csel doesn't care
        int64_t x = (i * 2654435761LL) >> 7;
        acc += clamp_p(x & 4095, 1000, 3000);
    }
    t1 = now_ns();
    printf("clamp       : %6.2f ns/call   (sink=%lld)\n", (t1 - t0) / ITERS, (long long)acc);

    return 0;
}
