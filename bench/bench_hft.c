// bench_hft.c — hotlang vs C++ (default AND tuned) vs Rust vs Zig (default
// AND tuned) on the same HFT kernels. All contenders lower through LLVM,
// though not the same copy: clang compiles hotlang's IR and the C++ columns
// with the system LLVM; rustc and zig bundle their own (versions printed in
// the bench.sh environment block).
//
// Columns:
//   C++      = clang++ -O3, default semantics (what you get without effort)
//   C++tuned = clang++ -O3 + __restrict + per-function
//              `#pragma clang fp reassociate(on) contract(fast)` — the
//              annotations a latency-critical C++ codebase actually ships.
//   Rust     = rustc -O3, safe idiomatic iterator style
//   Zig      = zig -O ReleaseFast, default semantics (strict IEEE float)
//   Zigtuned = zig -O ReleaseFast + `noalias` params +
//              `@setFloatMode(.optimized)` (full fast-math incl. nnan/ninf —
//              strictly MORE optimizer freedom than hotlang gets)
//
// All calls go through volatile function pointers so nothing gets inlined
// or constant-folded away. Each benchmark is timed exactly once per table
// cell (results stored in locals; ratios computed from the same runs that
// are printed — an earlier macro version re-evaluated its arguments and
// printed ratios from different runs than the displayed numbers).
//
// Build (from repo root — or just run bench/bench.sh, which does all this):
//   ./compiler/target/release/hotc build examples/book.hot -o hotout
//   clang++ -O3 -c bench/ref.cpp -o hotout/ref_cpp.o
//   clang++ -O3 -c bench/ref_tuned.cpp -o hotout/ref_cppt.o
//   rustc -C opt-level=3 -C codegen-units=1 --crate-type=staticlib \
//         bench/ref.rs -o hotout/libref_rs.a
//   zig build-obj -O ReleaseFast -mcpu=baseline bench/ref.zig \
//         -femit-bin=hotout/ref_zig.o
//   zig build-obj -O ReleaseFast -mcpu=baseline bench/ref_tuned.zig \
//         -femit-bin=hotout/ref_zigt.o
//   clang -O2 bench/bench_hft.c hotout/book.o hotout/ref_cpp.o \
//         hotout/ref_cppt.o hotout/libref_rs.a hotout/ref_zig.o \
//         hotout/ref_zigt.o -o hotout/bench_hft
//   ./hotout/bench_hft

#include <stdint.h>
#include <stdio.h>
#include <time.h>

// hotlang (C ABI from hotc)
extern double dot(const double*, const double*);
extern double book_pressure(const double*, const double*);
extern double vwap(const double*, const double*);
extern double scale_ladder(const double*, double, double*);
extern double matvec(const double*, const double*, double*);
extern int64_t decide(double, double, double, int64_t);

// C++ default (clang++ -O3)
extern double cpp_dot(const double*, const double*);
extern double cpp_book_pressure(const double*, const double*);
extern double cpp_vwap(const double*, const double*);
extern double cpp_scale_ladder(const double*, double, double*);
extern double cpp_matvec(const double*, const double*, double*);
extern int64_t cpp_decide(double, double, double, int64_t);

// C++ tuned (restrict + fp pragma)
extern double cppt_dot(const double*, const double*);
extern double cppt_book_pressure(const double*, const double*);
extern double cppt_vwap(const double*, const double*);
extern double cppt_scale_ladder(const double*, double, double*);
extern double cppt_matvec(const double*, const double*, double*);
extern int64_t cppt_decide(double, double, double, int64_t);

// Rust (rustc -C opt-level=3)
extern double rs_dot(const double*, const double*);
extern double rs_book_pressure(const double*, const double*);
extern double rs_vwap(const double*, const double*);
extern double rs_scale_ladder(const double*, double, double*);
extern double rs_matvec(const double*, const double*, double*);
extern int64_t rs_decide(double, double, double, int64_t);

// Zig default (zig -O ReleaseFast)
extern double zig_dot(const double*, const double*);
extern double zig_book_pressure(const double*, const double*);
extern double zig_vwap(const double*, const double*);
extern double zig_scale_ladder(const double*, double, double*);
extern double zig_matvec(const double*, const double*, double*);
extern int64_t zig_decide(double, double, double, int64_t);

// Zig tuned (noalias + @setFloatMode(.optimized))
extern double zigt_dot(const double*, const double*);
extern double zigt_book_pressure(const double*, const double*);
extern double zigt_vwap(const double*, const double*);
extern double zigt_scale_ladder(const double*, double, double*);
extern double zigt_matvec(const double*, const double*, double*);
extern int64_t zigt_decide(double, double, double, int64_t);

typedef double (*dot_fn)(const double*, const double*);
typedef double (*scale_fn)(const double*, double, double*);
typedef double (*matvec_fn)(const double*, const double*, double*);
typedef int64_t (*decide_fn)(double, double, double, int64_t);

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static uint64_t rng = 0x243F6A8885A308D3ull;
static double frand(void) {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (double)((rng >> 11) & 0xFFFFF) / 1048576.0 + 0.25;
}

// Cache-line aligned: .bss layout shifts between links (adding/removing an
// object file moved arrays from 0 to 16 mod 64 and swung the streaming-store
// kernels ~50%). Pinning to 64 makes runs comparable across rebuilds; every
// contender reads the same arrays, so this is column-neutral.
static _Alignas(64) double A[256], B[256], OUT[256];
static _Alignas(64) double BID[64], ASK[64], PX[64], SZ[64];
static _Alignas(64) double M[1024], V[32], MV[32];

static double gsink = 0.0;
static int64_t isink = 0;

// Each argument is a value, evaluated exactly once by the caller.
static void row(const char* name, double hot, double cpp, double cppt, double rs,
                double zig, double zigt) {
    printf("%-14s %8.2f %9.2f %9.2f %9.2f %9.2f %9.2f   %5.2fx %5.2fx %5.2fx %5.2fx %5.2fx\n",
           name, hot, cpp, cppt, rs, zig, zigt,
           cpp / hot, cppt / hot, rs / hot, zig / hot, zigt / hot);
}

static double bench_dot(dot_fn volatile f, const double* x, const double* y, long n) {
    double t0 = now_ns();
    for (long i = 0; i < n; i++) gsink += f(x, y);
    return (now_ns() - t0) / (double)n;
}

static double bench_scale(scale_fn volatile f, long n) {
    double t0 = now_ns();
    for (long i = 0; i < n; i++) gsink += f(A, 1.0 + (double)(i & 15) * 1e-6, OUT);
    return (now_ns() - t0) / (double)n;
}

static double bench_matvec(matvec_fn volatile f, long n) {
    double t0 = now_ns();
    for (long i = 0; i < n; i++) gsink += f(M, V, MV);
    return (now_ns() - t0) / (double)n;
}

// Disclosure: the periodic argument patterns (i & 1023, i & 7) are perfectly
// branch-predictable, which flatters the branchy C++/Rust/Zig codegen. This
// is deliberate — it makes `decide` the WORST case for hotlang's branchless
// select (predictable branches cost ~0; the select always pays both arms).
// Real market data is less predictable; this row is a floor, not a win.
static double bench_decide(decide_fn volatile f, long n) {
    double t0 = now_ns();
    for (long i = 0; i < n; i++) {
        double p = (double)(i & 1023) / 1024.0;
        isink += f(p, 100.0 + (double)(i & 7) * 0.01, 100.02, 100);
    }
    return (now_ns() - t0) / (double)n;
}

// Typical per-tick flow across layers: normalize -> vwap -> pressure ->
// signal -> branchless decision.
typedef struct {
    scale_fn scale;
    dot_fn vwap_f;
    dot_fn pressure;
    dot_fn dot_f;
    decide_fn decide_f;
} pipeline;

static double bench_pipeline(pipeline volatile pl, long n) {
    double t0 = now_ns();
    for (long i = 0; i < n; i++) {
        double fx = 1.0 + (double)(i & 255) * 1e-7;
        pl.scale(A, fx, OUT);
        double v = pl.vwap_f(OUT, SZ);
        double p = pl.pressure(BID, ASK);
        double sig = pl.dot_f(OUT, B);
        isink += pl.decide_f(p, v, 100.02, (int64_t)(sig > 0.0 ? 100 : 50));
        gsink += v + p;
    }
    return (now_ns() - t0) / (double)n;
}

int main(void) {
    for (int i = 0; i < 256; i++) { A[i] = frand() * 100.0; B[i] = frand(); OUT[i] = 0; }
    for (int i = 0; i < 64; i++) { BID[i] = frand() * 10; ASK[i] = frand() * 10; PX[i] = 100 + frand(); SZ[i] = frand() * 5 + 0.1; }
    for (int i = 0; i < 1024; i++) M[i] = frand();
    for (int i = 0; i < 32; i++) { V[i] = frand(); MV[i] = 0; }

    const long N = 2000000, ND = 20000000;

    printf("kernel          hotlang       C++  C++tuned      Rust       Zig  Zigtuned    vs C++  tuned   Rust    Zig   Zigt   (ns/call)\n");
    printf("--------------------------------------------------------------------------------------------------------------------------\n");

    {
        double h = bench_dot(dot, A, B, N), c = bench_dot(cpp_dot, A, B, N);
        double ct = bench_dot(cppt_dot, A, B, N), r = bench_dot(rs_dot, A, B, N);
        double z = bench_dot(zig_dot, A, B, N), zt = bench_dot(zigt_dot, A, B, N);
        row("dot(256)", h, c, ct, r, z, zt);
    }
    {
        double h = bench_dot(book_pressure, BID, ASK, N), c = bench_dot(cpp_book_pressure, BID, ASK, N);
        double ct = bench_dot(cppt_book_pressure, BID, ASK, N), r = bench_dot(rs_book_pressure, BID, ASK, N);
        double z = bench_dot(zig_book_pressure, BID, ASK, N), zt = bench_dot(zigt_book_pressure, BID, ASK, N);
        row("pressure(64)", h, c, ct, r, z, zt);
    }
    {
        double h = bench_dot(vwap, PX, SZ, N), c = bench_dot(cpp_vwap, PX, SZ, N);
        double ct = bench_dot(cppt_vwap, PX, SZ, N), r = bench_dot(rs_vwap, PX, SZ, N);
        double z = bench_dot(zig_vwap, PX, SZ, N), zt = bench_dot(zigt_vwap, PX, SZ, N);
        row("vwap(64)", h, c, ct, r, z, zt);
    }
    {
        double h = bench_scale(scale_ladder, N), c = bench_scale(cpp_scale_ladder, N);
        double ct = bench_scale(cppt_scale_ladder, N), r = bench_scale(rs_scale_ladder, N);
        double z = bench_scale(zig_scale_ladder, N), zt = bench_scale(zigt_scale_ladder, N);
        row("scale(256)", h, c, ct, r, z, zt);
    }
    {
        double h = bench_matvec(matvec, N), c = bench_matvec(cpp_matvec, N);
        double ct = bench_matvec(cppt_matvec, N), r = bench_matvec(rs_matvec, N);
        double z = bench_matvec(zig_matvec, N), zt = bench_matvec(zigt_matvec, N);
        row("matvec(32x32)", h, c, ct, r, z, zt);
    }
    {
        double h = bench_decide(decide, ND), c = bench_decide(cpp_decide, ND);
        double ct = bench_decide(cppt_decide, ND), r = bench_decide(rs_decide, ND);
        double z = bench_decide(zig_decide, ND), zt = bench_decide(zigt_decide, ND);
        row("decide", h, c, ct, r, z, zt);
    }
    {
        pipeline hot_pl = {scale_ladder, vwap, book_pressure, dot, decide};
        pipeline cpp_pl = {cpp_scale_ladder, cpp_vwap, cpp_book_pressure, cpp_dot, cpp_decide};
        pipeline cppt_pl = {cppt_scale_ladder, cppt_vwap, cppt_book_pressure, cppt_dot, cppt_decide};
        pipeline rs_pl = {rs_scale_ladder, rs_vwap, rs_book_pressure, rs_dot, rs_decide};
        pipeline zig_pl = {zig_scale_ladder, zig_vwap, zig_book_pressure, zig_dot, zig_decide};
        pipeline zigt_pl = {zigt_scale_ladder, zigt_vwap, zigt_book_pressure, zigt_dot, zigt_decide};
        double h = bench_pipeline(hot_pl, N), c = bench_pipeline(cpp_pl, N);
        double ct = bench_pipeline(cppt_pl, N), r = bench_pipeline(rs_pl, N);
        double z = bench_pipeline(zig_pl, N), zt = bench_pipeline(zigt_pl, N);
        row("tick pipeline", h, c, ct, r, z, zt);
    }

    printf("\n(sinks: %f %lld — ignore, defeats dead-code elimination)\n", gsink, (long long)isink);
    return 0;
}
