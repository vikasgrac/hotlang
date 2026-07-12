// div_edge.c — verifies hotlang's total division semantics from a C host.
#include <stdint.h>
#include <stdio.h>

extern int64_t tdiv(int64_t, int64_t);
extern int64_t trem(int64_t, int64_t);
extern int64_t div_const(int64_t);
extern int64_t imbalance_bps(int64_t, int64_t);

static int failures = 0;
#define CHECK(expr, want)                                                    \
    do {                                                                     \
        int64_t got = (expr);                                                \
        if (got != (want)) {                                                 \
            printf("FAIL %-28s got %lld want %lld\n", #expr, (long long)got, \
                   (long long)(want));                                       \
            failures++;                                                      \
        } else {                                                             \
            printf("ok   %-28s = %lld\n", #expr, (long long)got);            \
        }                                                                    \
    } while (0)

int main(void) {
    const int64_t MIN = INT64_MIN;
    CHECK(tdiv(7, 2), 3);
    CHECK(tdiv(-7, 2), -3);
    CHECK(tdiv(5, 0), 0);            // total: a / 0 == 0
    CHECK(trem(5, 0), 5);            // total: a % 0 == a
    CHECK(tdiv(MIN, -1), MIN);       // total: MIN / -1 == MIN (wraps)
    CHECK(trem(MIN, -1), 0);         // total: MIN % -1 == 0
    CHECK(tdiv(MIN, 1), MIN);
    CHECK(trem(7, 3), 1);
    CHECK(div_const(12345), 123);
    CHECK(imbalance_bps(600, 400), 2000);
    CHECK(imbalance_bps(0, 0), 0);   // 0/0 hot path: defined, no fault
    // identity a == (a/b)*b + a%b must hold everywhere, including b == 0
    for (int64_t a = -3; a <= 3; a++)
        for (int64_t b = -3; b <= 3; b++)
            if (a != tdiv(a, b) * b + trem(a, b)) {
                printf("FAIL identity a=%lld b=%lld\n", (long long)a, (long long)b);
                failures++;
            }
    printf(failures ? "\n%d FAILURES\n" : "\nALL DIVISION TESTS PASSED\n", failures);
    return failures != 0;
}
