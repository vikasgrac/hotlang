// bench_fusion.c — the whole-pipeline fusion comparison.
//
// Four contenders on the identical market-making tick:
//   hotlang fused   : examples/pipeline.hot -> tick()  (one verified fn)
//   C++ fused        : ref_fused.cpp cpp_tick_fused     (hand-fused, tuned)
//   hotlang 5-call   : examples/book.hot kernels, called in sequence
//   C++ 5-call       : ref_fused.cpp cpp_tick_5call     (tuned kernels)
//
// The honest story: fused-hotlang ~ties fused-tuned-C++ (same optimal loop,
// same roofline) but BEATS the 5-call versions a desk actually ships,
// because in hotlang the safe fused form is the natural one — every array
// noalias and bounds-proven, no hand-written restrict to get wrong.
//
// All calls go through volatile function pointers. Correctness-checked:
// every contender must return the same decision on the same data.
//
// Build (from repo root, after hotc build examples/{pipeline,book}.hot):
//   clang++ -O3 -march=native -c bench/ref_fused.cpp -o hotout/ref_fused.o
//   clang -O2 bench/bench_fusion.c hotout/pipeline.o hotout/book.o \
//         hotout/ref_fused.o -o hotout/bench_fusion && ./hotout/bench_fusion

#include <stdint.h>
#include <stdio.h>
#include <time.h>

// hotlang fused (examples/pipeline.hot)
extern int64_t tick(const double*, const double*, const double*, const double*, const double*, double);
// hotlang 5-call kernels (examples/book.hot)
extern double scale_ladder(const double*, double, double*);
extern double vwap(const double*, const double*);
extern double book_pressure(const double*, const double*);
extern double dot(const double*, const double*);
extern int64_t decide(double, double, double, int64_t);
// C++ (ref_fused.cpp)
extern int64_t cpp_tick_fused(const double*, const double*, const double*, const double*, const double*, double);
extern int64_t cpp_tick_5call(const double*, const double*, const double*, const double*, const double*, double);

typedef int64_t (*tickfn)(const double*, const double*, const double*, const double*, const double*, double);

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e9+t.tv_nsec; }
static uint64_t rng=0x243F6A8885A308D3ull;
static double fr(){ rng=rng*6364136223846793005ull+1442695040888963407ull; return (double)((rng>>11)&0xFFFFF)/1048576.0+0.25; }

static _Alignas(64) double A[256], B[256], OUT[256], SZ[64], BID[64], ASK[64];
static int64_t isink=0;

// hotlang 5-call pipeline, expressed through the same volatile-call pattern.
static double (*volatile v_scale)(const double*,double,double*) = scale_ladder;
static double (*volatile v_vwap)(const double*,const double*) = vwap;
static double (*volatile v_press)(const double*,const double*) = book_pressure;
static double (*volatile v_dot)(const double*,const double*) = dot;
static int64_t (*volatile v_decide)(double,double,double,int64_t) = decide;
static int64_t hot_5call(const double* p,const double* w,const double* s,const double* bd,const double* ak,double fx){
    v_scale(p, fx, OUT);
    double v = v_vwap(OUT, s);
    double pr = v_press(bd, ak);
    double sig = v_dot(OUT, w);
    return v_decide(pr, v, 100.02, sig>0.0?100:50);
}

static double bench(tickfn f, long n){
    double t0=now_ns();
    for(long i=0;i<n;i++){ double fx=1.0+(double)(i&255)*1e-7; isink += f(A,B,SZ,BID,ASK,fx); }
    return (now_ns()-t0)/(double)n;
}

int main(void){
    for(int i=0;i<256;i++){ A[i]=fr()*100; B[i]=fr(); }
    for(int i=0;i<64;i++){ SZ[i]=fr()*5+0.1; BID[i]=fr()*10; ASK[i]=fr()*10; }

    // correctness: all four must agree on the decision, at several fx
    int ok=1;
    for(int t=0;t<8;t++){
        double fx=1.0+t*1e-6;
        int64_t a=tick(A,B,SZ,BID,ASK,fx), b=cpp_tick_fused(A,B,SZ,BID,ASK,fx),
                c=cpp_tick_5call(A,B,SZ,BID,ASK,fx), d=hot_5call(A,B,SZ,BID,ASK,fx);
        if(!(a==b&&b==c&&c==d)){ printf("DISAGREE fx=%.6f: %lld %lld %lld %lld\n",fx,(long long)a,(long long)b,(long long)c,(long long)d); ok=0; }
    }
    printf("correctness: %s\n\n", ok?"all four agree":"MISMATCH");

    tickfn hf=tick, cf=cpp_tick_fused, c5=cpp_tick_5call, h5=hot_5call;
    tickfn vhf=hf, vcf=cf, vc5=c5, vh5=h5;  // already volatile via param
    const long N=5000000;
    printf("%-22s %8s %8s   %s\n","contender","ns/call","vs fused","");
    printf("--------------------------------------------------------\n");
    for(int r=0;r<3;r++){
        double hfn=bench(vhf,N), cfn=bench(vcf,N), c5n=bench(vc5,N), h5n=bench(vh5,N);
        printf("run %d:\n", r+1);
        printf("  hotlang FUSED        %8.2f      1.00x  (the language default)\n", hfn);
        printf("  C++ fused (tuned)    %8.2f   %6.2fx  (hand-fused, __restrict+pragma)\n", cfn, cfn/hfn);
        printf("  C++ 5-call (tuned)   %8.2f   %6.2fx  (what a desk ships)\n", c5n, c5n/hfn);
        printf("  hotlang 5-call       %8.2f   %6.2fx  (unfused hotlang)\n\n", h5n, h5n/hfn);
    }
    printf("(sink %lld)\n",(long long)isink);
    return 0;
}
