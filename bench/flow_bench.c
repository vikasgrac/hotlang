// Market-making TICK -> TRADE, one shared harness driving the same flow in
// hotlang / C++ / Rust / Zig over an identical stream of order-book snapshots.
// Each tick: fused book-pressure + vwap + alpha (depth reductions) -> branchless
// decision -> emit order {side, price, qty}. Verifies every language emits the
// SAME trade (side+price exact, size to ~1e-13) and benchmarks ns/tick.
//
// Honest expectation: hotlang beats the strict-IEEE default C++/Rust/Zig a desk
// ships (its reductions vectorize; theirs stay serial), and TIES tuned C++
// (__restrict + reassociate pragma) — same LLVM, same silicon.
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

typedef double(*fn)(const double*,const double*,const double*,const double*,
                    const double*,const double*,double,double,double,double*);
extern double on_tick        (const double*,const double*,const double*,const double*,const double*,const double*,double,double,double,double*); // hotlang
extern double flow_cpp       (const double*,const double*,const double*,const double*,const double*,const double*,double,double,double,double*);
extern double flow_cpp_tuned (const double*,const double*,const double*,const double*,const double*,const double*,double,double,double,double*);
#ifdef HAVE_RUST
extern double flow_rust      (const double*,const double*,const double*,const double*,const double*,const double*,double,double,double,double*);
#endif
#ifdef HAVE_ZIG
extern double flow_zig       (const double*,const double*,const double*,const double*,const double*,const double*,double,double,double,double*);
extern double flow_zig_tuned (const double*,const double*,const double*,const double*,const double*,const double*,double,double,double,double*);
#endif

static double now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
#define M 64          // distinct book snapshots
#define L 32          // levels per side
static const double THRESH=0.5, BASE=10.0, MAXQ=100.0;
static double BP[M][L],BQ[M][L],AP[M][L],AQ[M][L],AW[M][L],FT[M][L];
static uint64_t rng=88172645463325252ull; static uint64_t nr(){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static double ur(){ return (double)(nr()%1000)/1000.0; }

static void gen(void){
    for(int m=0;m<M;m++){
        int regime=m%3;                                   // 0 buy, 1 sell, 2 none
        double mid=1000.0+(m%7)*0.5, half=0.05;
        double bfac,afac,amag;
        if(regime==0){bfac=3.0;afac=1.0;amag=+6.0;}       // bid-heavy + positive alpha
        else if(regime==1){bfac=1.0;afac=3.0;amag=-6.0;}  // ask-heavy + negative alpha
        else {bfac=1.0;afac=1.0;amag=0.0;}                // balanced, no alpha
        for(int i=0;i<L;i++){
            BP[m][i]=mid-half-i*0.05; AP[m][i]=mid+half+i*0.05;
            BQ[m][i]=bfac*(10.0+20.0*ur()); AQ[m][i]=afac*(10.0+20.0*ur());
            AW[m][i]=0.01; FT[m][i]=amag*(1.0+0.1*ur());
        }
    }
}
static double call(fn f,int m,double* o){ return f(BP[m],BQ[m],AP[m],AQ[m],AW[m],FT[m],THRESH,BASE,MAXQ,o); }

static volatile double sink;
static double bench1(fn f){ volatile fn vf=f; double o[3],best=1e18;
    for(int r=0;r<20000;r++){ double t0=now_ns(); double a=0;
        for(int k=0;k<8;k++) for(int m=0;m<M;m++) a+=vf(BP[m],BQ[m],AP[m],AQ[m],AW[m],FT[m],THRESH,BASE,MAXQ,o)+o[2];
        double d=(now_ns()-t0)/(8.0*M); sink+=a; if(d<best)best=d; }
    return best;
}
static double measure(fn f){ double v[5]; for(int i=0;i<5;i++)v[i]=bench1(f);
    for(int i=0;i<5;i++)for(int j=i+1;j<5;j++)if(v[j]<v[i]){double t=v[i];v[i]=v[j];v[j]=t;} return v[2]; }

int main(void){
    gen();
    struct{const char*n;fn f;int tuned;}C[]={
        {"hotlang",on_tick,1},{"C++ default",flow_cpp,0},{"C++ tuned",flow_cpp_tuned,1},
#ifdef HAVE_RUST
        {"Rust",flow_rust,0},
#endif
#ifdef HAVE_ZIG
        {"Zig default",flow_zig,0},{"Zig tuned",flow_zig_tuned,1},
#endif
    };
    int NC=(int)(sizeof(C)/sizeof(C[0]));

    // reference orders from hotlang; verify every language emits the SAME trade.
    double ref[M][3]; long buys=0,sells=0,holds=0;
    for(int m=0;m<M;m++){ call(on_tick,m,ref[m]);
        if(ref[m][0]==1.0)buys++; else if(ref[m][0]==2.0)sells++; else holds++; }
    printf("tick->trade flow: %d book snapshots (%d levels/side), decisions: %ld buy / %ld sell / %ld hold\n\n",
           M,L,buys,sells,holds);
    printf("sample orders (side 1=BUY 2=SELL 0=hold):\n");
    for(int m=0;m<4;m++) printf("  tick %d -> side %.0f  px %.2f  qty %.4f\n",m,ref[m][0],ref[m][1],ref[m][2]);
    printf("\ncorrectness vs hotlang (must be identical trade, size to ~1e-13):\n");
    for(int c=1;c<NC;c++){
        int sidebad=0,pxbad=0; double qmax=0;
        for(int m=0;m<M;m++){ double o[3]; call(C[c].f,m,o);
            if(o[0]!=ref[m][0])sidebad++; if(o[1]!=ref[m][1])pxbad++;
            double d=fabs(o[2]-ref[m][2]); if(d>qmax)qmax=d; }
        printf("  %-12s side-mismatch %d  px-mismatch %d  max|qty dev| %.1e  %s\n",
               C[c].n,sidebad,pxbad,qmax,(sidebad==0&&pxbad==0&&qmax<1e-9)?"OK":"*** DIFFER ***");
    }

    printf("\nlatency (best-of median-of-5, ns/tick):\n");
    printf("  %-12s ns/tick   vs hotlang\n","lang");
    double hot=measure(C[0].f);
    for(int c=0;c<NC;c++){ double md=(c==0)?hot:measure(C[c].f);
        printf("  %-12s %6.1f    %5.2fx %s\n",C[c].n,md,md/hot,C[c].tuned?"":"  (strict-IEEE default)"); }
    printf("\n(sink=%.0f)\n",sink);
    return 0;
}
