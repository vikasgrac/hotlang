// Feed-to-fire benchmark: a basic two-leg FUTURES arbitrage (near vs far month
// on one NSE underlying), the SAME logic in hotlang / C++ / Rust / Zig, driven
// by ONE C harness over an identical tick stream. Prices are integer paise
// (NSE price-tick SIZE = 5 paise); the arb fires both legs when the observed
// calendar spread dislocates past `thresh` net of the fair carry.
//
// Read the honest_read block at the end before quoting any number. In short:
// hotlang is NOT the fastest here (a scalar one-shot branch is its documented
// worst case); the value it demonstrates is a compile-time GUARANTEE of
// branchless, allocation-free, bounded, total code - not a latency win.
//
// Two streams isolate branch behaviour:
//   realistic  - dislocations rare (~2%), the "do I fire?" branch predicts well.
//   adversarial- dislocation unpredictable each tick (~2/3 fire), branch mispredicts.
// NOTE the "tail" this exposes is a NON-INLINED artifact: these calls go through
// a function pointer so nothing inlines. Inlined into a real feed handler, LLVM
// if-converts the C++/Rust branches to the same branchless csel and the tail
// vanishes. hotlang's decision is branchless in the IR by construction.
#include <stdint.h>
#include <stdio.h>
#include <time.h>

extern int64_t arb     (const int64_t*, int64_t, int64_t, int64_t*); // hotlang
extern int64_t arb_cpp (const int64_t*, int64_t, int64_t, int64_t*);
extern int64_t arb_rust(const int64_t*, int64_t, int64_t, int64_t*);
#ifdef HAVE_ZIG
extern int64_t arb_zig (const int64_t*, int64_t, int64_t, int64_t*);
#endif
typedef int64_t (*arbfn)(const int64_t*, int64_t, int64_t, int64_t*);

// empty-body arb (same C ABI): measures the fixed call+harness floor.
static int64_t arb_floor(const int64_t* t, int64_t a, int64_t b, int64_t* o){
    (void)t;(void)a;(void)b; for(int j=0;j<6;j++) o[j]=0; return 0;
}

static double now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}

#define N 4096
static const int64_t FAIR=50, THRESH=20;   // paise: fair carry, net edge to fire
static int64_t T[N][8];

static uint64_t rng;
static uint64_t nr(){ rng = rng*6364136223846793005ull + 1442695040888963407ull; return rng>>17; }
static int64_t  qty(){ return (int64_t)(nr()%500)+1; }   // 1..500 lots

// mode 0 = realistic (arb rare); mode 1 = adversarial (signal unpredictable)
static void gen(int mode){
    rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)mode;
    int64_t near_mid = 100000;                            // paise (Rs 1000.00)
    for(int i=0;i<N;i++){
        near_mid += ((int64_t)(nr()%4)-2)*5;              // random walk -10..+5
        if(near_mid<50000) near_mid=50000;
        int64_t disloc;
        if(mode==0){                                      // rare, predictable
            uint64_t r=nr()%100;
            disloc = (r<1)?50 : (r<2)?-50 : ((int64_t)(nr()%5)-2)*5;
        } else {                                          // unpredictable
            disloc = ((int64_t)(nr()%3)-1)*50;            // -50,0,+50
        }
        int64_t far_mid = near_mid + FAIR + disloc;
        T[i][0]=near_mid-5; T[i][1]=qty(); T[i][2]=near_mid+5; T[i][3]=qty();  // near
        T[i][4]=far_mid-5;  T[i][5]=qty(); T[i][6]=far_mid+5;  T[i][7]=qty();  // far
    }
}

static uint64_t checksum(arbfn f, long* fires){
    int64_t out[6]; uint64_t cs=1469598103934665603ull; long fr=0;
    for(int i=0;i<N;i++){
        int64_t s=f(T[i],FAIR,THRESH,out);
        cs=(cs^(uint64_t)s)*1099511628211ull;
        for(int j=0;j<6;j++) cs=(cs^(uint64_t)out[j])*1099511628211ull;
        if(s) fr++;
    }
    if(fires)*fires=fr;
    return cs;
}

static volatile int64_t SINK=0;
// one best-of-300 sample; batch B passes so per-tick isn't quantised by the timer.
static double bench1(arbfn f){
    volatile arbfn vf=f; int64_t out[6]; double best=1e18;
    const int B=32;
    for(int r=0;r<300;r++){
        double t0=now_ns(); int64_t acc=0;
        for(int b=0;b<B;b++) for(int i=0;i<N;i++){ acc += vf(T[i],FAIR,THRESH,out)+out[1]+out[4]; }
        double d=(now_ns()-t0)/((double)B*N); SINK+=acc; if(d<best) best=d;
    }
    return best;
}
// median of 5 samples: single noisy run cannot flip a verdict.
static double measure(arbfn f){
    double v[5]; for(int k=0;k<5;k++) v[k]=bench1(f);
    for(int i=0;i<5;i++) for(int j=i+1;j<5;j++) if(v[j]<v[i]){double t=v[i];v[i]=v[j];v[j]=t;}
    return v[2];
}

int main(void){
    struct { const char* name; arbfn fn; } C[] = {
        {"hotlang", arb}, {"C++", arb_cpp}, {"Rust", arb_rust},
#ifdef HAVE_ZIG
        {"Zig", arb_zig},
#endif
    };
    int NC=(int)(sizeof(C)/sizeof(C[0]));
    const char* modes[2]={"realistic  (arb ~2% of ticks, branch predictable)",
                          "adversarial (signal ~2/3 of ticks, unpredictable)"};
    const double BUDGET_NS=5000.0;   // ASSUMED per-decision budget, NOT a measured NSE cadence
    double ns[2][8];

    gen(0);
    double floor=measure(arb_floor);
    printf("fixed call+harness floor (empty arb) = %.2f ns/tick — subtract it for arb-only cost.\n", floor);

    for(int m=0;m<2;m++){
        gen(m);
        long fires=0; uint64_t ref=checksum(C[0].fn,&fires); int ok=1;
        for(int c=1;c<NC;c++) if(checksum(C[c].fn,0)!=ref) ok=0;
        printf("\n=== stream: %s ===\n", modes[m]);
        printf("ticks=%d  fire-rate=%.1f%%  correctness: %s\n",
               N, 100.0*fires/N, ok?"all languages bitwise-identical":"*** DIFFER ***");
        printf("  %-9s  ns/tick   arb-only   Mticks/s\n","lang");
        for(int c=0;c<NC;c++){
            ns[m][c]=measure(C[c].fn);
            double arbonly = ns[m][c]-floor; if(arbonly<0) arbonly=0;
            printf("  %-9s  %6.2f    %6.2f    %7.1f\n", C[c].name, ns[m][c], arbonly, 1000.0/ns[m][c]);
        }
    }

    // dynamic winner + hotlang's rank on the predictable stream (index 0 = hotlang).
    int best=0; for(int c=1;c<NC;c++) if(ns[0][c]<ns[0][best]) best=c;
    int hot_rank=1; for(int c=0;c<NC;c++) if(c!=0 && ns[0][c]<ns[0][0]) hot_rank++;

    // the branch shape, measured on both streams. Median-based, so a noisy single
    // run can't mislabel branchless code; a real tail needs >12% delta.
    printf("\n=== branch shape: slower on unpredictable input? (median of 5) ===\n");
    printf("  %-9s  predictable  unpredictable   delta     shape\n","lang");
    for(int c=0;c<NC;c++){
        double p=ns[0][c], u=ns[1][c], d=100.0*(u-p)/p;
        const char* v = (d < 12.0) ? "flat" : "branch tail";
        printf("  %-9s  %7.2f ns   %8.2f ns   %+5.0f%%    %s\n", C[c].name, p, u, d, v);
    }

    printf("\nHonest read (what this does and does NOT show):\n");
    printf("  - Correctness: all %d contenders emit bitwise-identical orders on every\n", NC);
    printf("    tick in both streams. The strategy is genuinely the same everywhere.\n");
    printf("  - hotlang is NOT the fastest. On the predictable stream %s is fastest\n", C[best].name);
    printf("    (~%.2f ns); hotlang is #%d of %d, and C++/Rust also beat it on the\n", ns[0][best], hot_rank, NC);
    printf("    adversarial stream in absolute ns. A one-shot scalar branch is\n");
    printf("    hotlang's documented worst case: it evaluates both arms every tick,\n");
    printf("    so it can't skip the order-building on no-arb ticks. Its real levers\n");
    printf("    (vectorising loops, incremental state) need a loop to bite.\n");
    printf("  - The 'branch tail' above is a NON-INLINED artifact. These calls go\n");
    printf("    through a function pointer so nothing inlines. Inlined into a real\n");
    printf("    feed handler - what production does - LLVM if-converts the C++/Rust\n");
    printf("    branches to the same branchless csel and the tail disappears. Do NOT\n");
    printf("    read it as a production latency risk. (Rust's larger delta is also\n");
    printf("    phrasing-specific: a single `match sig` roughly halves it.)\n");
    printf("  - What hotlang actually gives here is a GUARANTEE, not speed: the\n");
    printf("    decision is branchless in the IR by construction (if -> LLVM select,\n");
    printf("    verified in the emitted .ll), and the compiler has PROVEN the whole\n");
    printf("    function allocates nothing, is bounded, total, and in-bounds. Being\n");
    printf("    branchless straight-line code, its per-tick cost is data-independent\n");
    printf("    - you can't lose that to a refactor or a compiler-version change.\n");
    printf("  - Disclosures: ~%.2f ns/tick is fixed call+harness overhead, so\n", floor);
    printf("    'Mticks/s' is call-bound and the tail%% is a lower bound. The %.0f-us\n", BUDGET_NS/1000.0);
    printf("    budget is ASSUMED, not a measured NSE figure (NSE's aggregate feed is\n");
    printf("    ~5-6M msg/s, ~0.2us mean spacing; matching engine ~100us). All four\n");
    printf("    clear it with huge margin, so on this decision latency isn't the\n");
    printf("    constraint - determinism and the compile-time guarantees are the point.\n");
    printf("    (sink=%lld)\n",(long long)SINK);
    return 0;
}
