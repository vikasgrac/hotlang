// math_edge.c — verifies hotlang math builtins and stats.hot from a C host,
// against libm ground truth.
#include <math.h>
#include <stdint.h>
#include <stdio.h>

// tests/math.hot
extern double tsqrt(double);
extern double tminf(double, double);
extern double tmaxf(double, double);
extern int64_t tmini(int64_t, int64_t);
extern int64_t tabsi(int64_t);
extern double tfma(double, double, double);
extern double texp(double);
extern double tlog(double);

// examples/stats.hot
extern double tf64(int64_t);
extern int64_t ti64(double);
extern double notional(int64_t, double);

extern double norm_cdf(double);
extern double bs_call(double, double, double, double, double);
extern double bs_delta(double, double, double, double, double);
extern double zscore(double, double, double);

static int failures = 0;
#define CHECK_F(expr, want, tol)                                              \
    do {                                                                      \
        double got = (expr), w = (want);                                      \
        if (!(fabs(got - w) <= (tol))) {                                      \
            printf("FAIL %-34s got %.10f want %.10f\n", #expr, got, w);       \
            failures++;                                                       \
        } else {                                                              \
            printf("ok   %-34s = %.10f\n", #expr, got);                       \
        }                                                                     \
    } while (0)
#define CHECK_I(expr, want)                                                   \
    do {                                                                      \
        int64_t got = (expr);                                                 \
        if (got != (want)) {                                                  \
            printf("FAIL %-34s got %lld\n", #expr, (long long)got);           \
            failures++;                                                       \
        } else {                                                              \
            printf("ok   %-34s = %lld\n", #expr, (long long)got);             \
        }                                                                     \
    } while (0)

int main(void) {
    CHECK_F(tsqrt(4.0), 2.0, 1e-15);
    CHECK_F(tminf(1.5, -2.5), -2.5, 0);
    CHECK_F(tmaxf(1.5, -2.5), 1.5, 0);
    CHECK_I(tmini(7, -9), -9);
    CHECK_I(tabsi(-5), 5);
    CHECK_I(tabsi(INT64_MIN), INT64_MIN);  // total: abs(MIN) == MIN
    CHECK_F(tfma(2.0, 3.0, 4.0), 10.0, 1e-15);
    CHECK_F(texp(0.0), 1.0, 1e-15);
    CHECK_F(texp(1.0), M_E, 1e-12);
    CHECK_F(tlog(1.0), 0.0, 1e-15);
    CHECK_F(tlog(M_E), 1.0, 1e-12);

    CHECK_F(tf64(3), 3.0, 0);
    CHECK_F(tf64(-1000000), -1000000.0, 0);
    CHECK_I(ti64(1.9), 1);                    // truncates toward zero
    CHECK_I(ti64(-1.9), -1);
    CHECK_I(ti64(0.0 / 0.0), 0);              // NaN -> 0, total
    CHECK_I(ti64(1e300), INT64_MAX);          // saturates
    CHECK_I(ti64(-1e300), INT64_MIN);
    CHECK_F(notional(100, 101.5), 10150.0, 1e-9);

    CHECK_F(norm_cdf(0.0), 0.5, 1e-7);
    CHECK_F(norm_cdf(1.959963985), 0.975, 1e-6);
    CHECK_F(norm_cdf(-1.959963985), 0.025, 1e-6);
    CHECK_F(norm_cdf(0.0) + norm_cdf(-0.0), 1.0, 1e-7);

    // Black-Scholes vs textbook: S=100 K=100 vol=0.20 T=1 r=0.05
    CHECK_F(bs_call(100, 100, 0.20, 1.0, 0.05), 10.4506, 5e-3);
    CHECK_F(bs_delta(100, 100, 0.20, 1.0, 0.05), 0.6368, 5e-4);

    CHECK_F(zscore(3.0, 1.0, 2.0), 1.0, 1e-15);
    CHECK_F(zscore(1.0, 1.0, 0.0), 0.0, 1e-3);  // sigma=0 guarded, no inf

    printf(failures ? "\n%d FAILURES\n" : "\nALL MATH TESTS PASSED\n", failures);
    return failures != 0;
}
