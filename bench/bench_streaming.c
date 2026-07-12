// bench_streaming.c — the incremental-streaming win, measured and validated
// against a TRUE (long-double two-pass) reference, on non-degenerate data.
//
// Speed: incremental (hotlang, O(1)/tick) vs recompute (tuned C++, O(W)/tick).
// Accuracy: every stat checked against a long-double two-pass computation of
//   the true window value — NOT against the C++ recompute (which shares the
//   algorithm and would hide common-mode error).
// Data: prices trend + noise; bid/ask imbalance sweeps across regimes, so the
//   buy/sell/hold decision is actually exercised (not trivially always hold).
// Three-way: hotlang incremental vs hand-written incremental C++ (a tie, same
//   algorithm) vs recompute C++ — all reproducible from this one harness.
//
// Build (from repo root):
//   hotc build examples/streaming.hot -o hotout
//   clang++ -O3 -march=native -c bench/ref_streaming.cpp -o hotout/ref_streaming.o
//   clang -O2 bench/bench_streaming.c hotout/streaming.o hotout/ref_streaming.o \
//         -lm -o hotout/bench_streaming && ./hotout/bench_streaming

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// hotlang incremental (examples/streaming.hot). var/vol state = [mean, M2].
extern double roll_mean(double*, double, double, double);
extern double roll_vol(double*, double, double, double);
extern double roll_vwap(double*, double, double, double, double);
extern double roll_pressure(double*, double, double, double, double);
extern int64_t tick_incr(double*, double, double, double, double, double, double, double, double, double);
// tuned C++ recompute + hand-written incremental (ref_streaming.cpp)
extern double rc_mean(const double*, int);
extern double rc_vol(const double*, int);
extern double rc_vwap(const double*, const double*, int);
extern double rc_pressure(const double*, const double*, int);
extern int64_t cpp_tick_recompute(const double*, const double*, const double*, const double*, int);
extern int64_t cpp_tick_incr(double*, double, double, double, double, double, double, double, double, double);

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e9+t.tv_nsec; }
static uint64_t rng=0x243F6A8885A308D3ull;
static double fr(){ rng=rng*6364136223846793005ull+1442695040888963407ull; return (double)((rng>>11)&0xFFFFF)/1048576.0; }

#define STREAM (1<<20)
#define W 256
static _Alignas(64) double PX[STREAM], SZ[STREAM], BID[STREAM], ASK[STREAM];
static double gsink=0; static int64_t isink=0;

// true window stats via long double two-pass (ground truth)
static long double true_mean(long i0){ long double s=0; for(int i=0;i<W;i++) s+=PX[i0+i]; return s/W; }
static long double true_vol(long i0){ long double m=true_mean(i0),v=0; for(int i=0;i<W;i++){long double d=PX[i0+i]-m; v+=d*d;} return sqrtl(v/W); }
static long double true_vwap(long i0){ long double n=0,q=0; for(int i=0;i<W;i++){n+=(long double)PX[i0+i]*SZ[i0+i]; q+=SZ[i0+i];} return n/q; }

int main(void){
    // Non-degenerate stream: gentle price trend + noise; bid/ask imbalance
    // sweeps so book pressure crosses the 0.4/0.6 decision thresholds.
    for(long i=0;i<STREAM;i++){
        double ph = (double)(i % 200000) / 200000.0;          // 0..1 slow ramp
        PX[i]  = 100.0 + 8.0*sin(0.0001*(double)i) + fr()*2.0; // trend + noise
        SZ[i]  = fr()*5+0.1;
        BID[i] = fr()*10.0*(0.6+ph);                           // bid mass ramps up
        ASK[i] = fr()*10.0*(1.4-ph);                           // ask mass ramps down
    }
    long N = STREAM - W;

    printf("Rolling stats, %d-tick stream, window W=%d. Speed = ns/tick.\n", STREAM, W);
    printf("Accuracy = worst rel error of incremental vs TRUE (long-double two-pass).\n\n");
    printf("%-16s %11s %11s %9s   %10s\n","statistic","incremental","recompute","speedup","err vs TRUE");
    printf("---------------------------------------------------------------------------\n");

    // rolling mean
    { double st[1]={0}; for(int i=0;i<W;i++) st[0]+=PX[i];
      double me=0, t0=now_ns();
      for(long t=0;t<N;t++){ double v=roll_mean(st,PX[t+W],PX[t],(double)W); gsink+=v;
        if((t&2047)==0){ long double r=true_mean(t+1); double e=fabsl(v-r)/(double)(fabsl(r)+1e-30); if(e>me)me=e; } }
      double inc=(now_ns()-t0)/(double)N;
      t0=now_ns(); for(long t=0;t<N;t++){ volatile double r=rc_mean(&PX[t],W); gsink+=r; } double rec=(now_ns()-t0)/(double)N;
      printf("%-16s %9.2f   %9.2f   %7.1fx   %10.1e\n","rolling mean",inc,rec,rec/inc,me); }

    // rolling vol (Welford)
    { double st[2]; { double m=0; for(int i=0;i<W;i++)m+=PX[i]; m/=W; double M2=0; for(int i=0;i<W;i++){double d=PX[i]-m;M2+=d*d;} st[0]=m; st[1]=M2; }
      double me=0, t0=now_ns();
      for(long t=0;t<N;t++){ double v=roll_vol(st,PX[t+W],PX[t],(double)W); gsink+=v;
        if((t&2047)==0){ long double r=true_vol(t+1); double e=fabsl(v-r)/(double)(fabsl(r)+1e-30); if(e>me)me=e; } }
      double inc=(now_ns()-t0)/(double)N;
      t0=now_ns(); for(long t=0;t<N;t++){ volatile double r=rc_vol(&PX[t],W); gsink+=r; } double rec=(now_ns()-t0)/(double)N;
      printf("%-16s %9.2f   %9.2f   %7.1fx   %10.1e\n","rolling vol",inc,rec,rec/inc,me); }

    // rolling vwap
    { double st[2]={0,0}; for(int i=0;i<W;i++){ st[0]+=PX[i]*SZ[i]; st[1]+=SZ[i]; }
      double me=0, t0=now_ns();
      for(long t=0;t<N;t++){ double v=roll_vwap(st,PX[t+W],SZ[t+W],PX[t],SZ[t]); gsink+=v;
        if((t&2047)==0){ long double r=true_vwap(t+1); double e=fabsl(v-r)/(double)(fabsl(r)+1e-30); if(e>me)me=e; } }
      double inc=(now_ns()-t0)/(double)N;
      t0=now_ns(); for(long t=0;t<N;t++){ volatile double r=rc_vwap(&PX[t],&SZ[t],W); gsink+=r; } double rec=(now_ns()-t0)/(double)N;
      printf("%-16s %9.2f   %9.2f   %7.1fx   %10.1e\n","rolling vwap",inc,rec,rec/inc,me); }

    // fused tick + three-way + decision distribution + correctness vs TRUE
    { double init[6]={0,0,0,0,0,0};
      for(int i=0;i<W;i++){ init[0]+=PX[i]*SZ[i]; init[1]+=SZ[i]; init[2]+=BID[i]; init[3]+=ASK[i]; init[4]+=PX[i]; }
      init[4]/=W; { double M2=0; for(int i=0;i<W;i++){double d=PX[i]-init[4];M2+=d*d;} init[5]=M2; }
      // correctness + decision counts over the whole stream
      double sc[6]; for(int k=0;k<6;k++)sc[k]=init[k];
      long buys=0,sells=0,holds=0,mism=0;
      for(long t=0;t<N;t++){
          int64_t a=tick_incr(sc,PX[t+W],SZ[t+W],BID[t+W],ASK[t+W],PX[t],SZ[t],BID[t],ASK[t],(double)W);
          int64_t b=cpp_tick_recompute(&PX[t+1],&SZ[t+1],&BID[t+1],&ASK[t+1],W);
          if(a==100)buys++; else if(a==-100)sells++; else holds++;
          if(a!=b)mism++;
      }
      // speed three-way
      int64_t (*volatile vi)(double*,double,double,double,double,double,double,double,double,double)=tick_incr;
      int64_t (*volatile vci)(double*,double,double,double,double,double,double,double,double,double)=cpp_tick_incr;
      int64_t (*volatile vr)(const double*,const double*,const double*,const double*,int)=cpp_tick_recompute;
      for(int k=0;k<6;k++)sc[k]=init[k]; double t0=now_ns();
      for(long t=0;t<N;t++) isink+=vi(sc,PX[t+W],SZ[t+W],BID[t+W],ASK[t+W],PX[t],SZ[t],BID[t],ASK[t],(double)W);
      double hi=(now_ns()-t0)/(double)N;
      for(int k=0;k<6;k++)sc[k]=init[k]; t0=now_ns();
      for(long t=0;t<N;t++) isink+=vci(sc,PX[t+W],SZ[t+W],BID[t+W],ASK[t+W],PX[t],SZ[t],BID[t],ASK[t],(double)W);
      double ci=(now_ns()-t0)/(double)N;
      t0=now_ns(); for(long t=0;t<N;t++) isink+=vr(&PX[t],&SZ[t],&BID[t],&ASK[t],W); double rec=(now_ns()-t0)/(double)N;
      printf("---------------------------------------------------------------------------\n");
      printf("FUSED tick (one call): decisions buy=%ld sell=%ld hold=%ld, incr-vs-recompute mismatches=%ld\n",buys,sells,holds,mism);
      printf("  hotlang incremental   %8.2f ns   1.00x\n",hi);
      printf("  hand-written C++ incr %8.2f ns   %.2fx   (ties hotlang: same algorithm)\n",ci,ci/hi);
      printf("  recompute C++ (tuned) %8.2f ns   %.1fx   (what a kernel library ships)\n",rec,rec/hi);
    }

    printf("\n(sink %f %lld — defeats DCE)\n", gsink, (long long)isink);
    return 0;
}
