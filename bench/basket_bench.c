// Basket/index arbitrage — the arb whose hot path IS a matrix-vector product:
// every tick, recompute B baskets' fair values from K constituents
// (fair[b] = sum_k W[b][k]*px[k]) and fire where fair diverges from traded.
// Same computation in hotlang / C / C++ / Rust / Zig, one shared harness.
//
// This is the shape where hotlang can beat even TUNED contenders: hotc tags the
// inner reduction with per-loop !llvm.loop policy metadata (interleave
// accumulators, unroll the vectorized loop) that no C++/Zig pragma expresses.
// The effect scales with vector width: on M4 NEON (2 doubles) it ties tuned C++;
// on Zen5 AVX-512 (8 doubles) tuned C++'s autovectorizer can blow up into a
// register-spill and hotlang's metadata avoids it (the matvec win). RUN THIS ON
// THE ZEN5 BOX — that is where "beat tuned" is decided.
#include <stdint.h>
#include <stdio.h>
#include <time.h>

extern double basket_arb      (const double*,const double*,const double*,double,double*); // hotlang
extern double basket_c        (const double*,const double*,const double*,double,double*);
extern double basket_c_tuned  (const double*,const double*,const double*,double,double*);
extern double basket_cpp_tuned(const double*,const double*,const double*,double,double*);
extern double basket_rust     (const double*,const double*,const double*,double,double*);
#ifdef HAVE_ZIG
extern double basket_zig      (const double*,const double*,const double*,double,double*);
#endif
typedef double(*fn)(const double*,const double*,const double*,double,double*);

static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
#define B 32
#define K 32
static double W[B*K],PX[K],TR[B],SG[B]; static volatile double sink;
static uint64_t rng=1; static double u(){rng=rng*6364136223846793005ull+1;return ((rng>>11)&0xFFFFF)/1048576.0;}

static double bench1(fn f){
    volatile fn vf=f; double best=1e18;
    for(int r=0;r<30000;r++){ double t0=now(); double a=0;
        for(int k=0;k<256;k++) a+=vf(W,PX,TR,5.0,SG);
        double d=(now()-t0)/256; sink+=a; if(d<best)best=d; }
    return best;
}
static double measure(fn f){ double v[5]; for(int i=0;i<5;i++)v[i]=bench1(f);
    for(int i=0;i<5;i++)for(int j=i+1;j<5;j++)if(v[j]<v[i]){double t=v[i];v[i]=v[j];v[j]=t;} return v[2]; }

int main(void){
    for(int i=0;i<B*K;i++)W[i]=u()*0.1;
    for(int k=0;k<K;k++)PX[k]=100.0+u()*10;
    for(int b=0;b<B;b++)TR[b]=200.0+u()*20;

    struct{const char*n;fn f;int tuned;}C[]={
        {"hotlang",basket_arb,1},{"C plain",basket_c,0},{"C tuned",basket_c_tuned,1},
        {"C++ tuned",basket_cpp_tuned,1},{"Rust",basket_rust,0},
#ifdef HAVE_ZIG
        {"Zig tuned",basket_zig,1},
#endif
    };
    int NC=(int)(sizeof(C)/sizeof(C[0]));

    // correctness: max |deviation| of each contender's fair-value vector vs hotlang.
    double ref[B]; basket_arb(W,PX,TR,5.0,ref);
    double hot=measure(C[0].f);
    printf("basket/index arb: %d baskets x %d constituents (matvec), best-of median-of-5\n\n", B, K);
    printf("  %-11s  ns/tick   vs hotlang   max|dev|\n","lang");
    for(int c=0;c<NC;c++){
        double s[B]; C[c].f(W,PX,TR,5.0,s);
        double md=(c==0)?hot:measure(C[c].f), dev=0;
        for(int i=0;i<B;i++){double d=s[i]-ref[i]; if(d<0)d=-d; if(d>dev)dev=d;}
        printf("  %-11s  %6.1f    %6.2fx     %.1e%s\n", C[c].n, md, md/hot, dev,
               C[c].tuned?"":"   (strict IEEE default)");
    }
    printf("\nRead: hotlang reassociates the f64 reduction by construction, so it\n");
    printf("vectorizes where strict-IEEE C/Rust stay serial (~3x). Against TUNED\n");
    printf("(reassoc) C/C++ it ties on narrow vectors and can win on AVX-512, where\n");
    printf("the tuned autovectorizer blows up and hotlang's per-loop metadata does\n");
    printf("not. Zig: use 0.14.x — 0.16 disables LLVM loop vectorization upstream.\n");
    printf("(sink=%.0f)\n", sink);
    return 0;
}
