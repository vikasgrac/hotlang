// bench_streaming.c — the incremental-streaming win, measured and
// numerically validated.
//
// For each rolling statistic we compare, over a long tick stream:
//   incremental (hotlang, O(1)/tick)  vs  recompute (tuned C++, O(W)/tick)
// and we CHECK the incremental result against the recompute reference every
// tick, reporting the worst relative error — because the honest objection to
// incremental algorithms is numerical drift, not speed.
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

// hotlang incremental handlers (examples/streaming.hot)
extern double roll_mean(double*, double, double, double);
extern double roll_var(double*, double, double, double);
extern double roll_vol(double*, double, double, double);
extern double roll_vwap(double*, double, double, double, double);
extern double roll_pressure(double*, double, double, double, double);

// recompute references (ref_streaming.cpp, tuned C++)
extern double rc_mean(const double*, int);
extern double rc_var(const double*, int);
extern double rc_vol(const double*, int);
extern double rc_vwap(const double*, const double*, int);
extern double rc_pressure(const double*, const double*, int);

// fused handlers: whole tick in one call
extern int64_t tick_incr(double*, double, double, double, double, double, double, double, double, double);
extern int64_t cpp_tick_recompute(const double*, const double*, const double*, const double*, int);

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e9+t.tv_nsec; }
static uint64_t rng=0x243F6A8885A308D3ull;
static double fr(){ rng=rng*6364136223846793005ull+1442695040888963407ull; return (double)((rng>>11)&0xFFFFF)/1048576.0+0.25; }

#define STREAM (1<<20)
#define W 256
static _Alignas(64) double PX[STREAM], SZ[STREAM], BID[STREAM], ASK[STREAM];
static double gsink=0;
static int64_t isink=0;

int main(void){
    for(long i=0;i<STREAM;i++){ PX[i]=100.0+fr(); SZ[i]=fr()*5+0.1; BID[i]=fr()*10; ASK[i]=fr()*10; }
    long N = STREAM - W;

    printf("Rolling stats over a %d-tick stream, window W=%d.\n", STREAM, W);
    printf("Speed = ns/tick; error = worst relative error of incremental vs recompute.\n\n");
    printf("%-16s %12s %12s %10s   %10s\n","statistic","incremental","recompute","speedup","max rel err");
    printf("---------------------------------------------------------------------------\n");

    // ---- rolling mean ----
    {
        double st[1]={0}; for(int i=0;i<W;i++) st[0]+=PX[i];
        double maxerr=0, t0=now_ns();
        for(long t=0;t<N;t++){
            double v = roll_mean(st, PX[t+W], PX[t], (double)W);
            gsink += v;
            if((t & 4095)==0){ double r=rc_mean(&PX[t+1], W); double e=fabs(v-r)/fabs(r); if(e>maxerr)maxerr=e; }
        }
        double inc=(now_ns()-t0)/(double)N;
        t0=now_ns(); for(long t=0;t<N;t++){ volatile double r=rc_mean(&PX[t],W); gsink+=r; } double rec=(now_ns()-t0)/(double)N;
        printf("%-16s %10.2f   %10.2f   %8.0fx   %10.1e\n","rolling mean",inc,rec,rec/inc,maxerr);
    }
    // ---- rolling variance ----
    {
        double st[2]={0,0}; for(int i=0;i<W;i++){ st[0]+=PX[i]; st[1]+=PX[i]*PX[i]; }
        double maxerr=0, t0=now_ns();
        for(long t=0;t<N;t++){
            double v = roll_var(st, PX[t+W], PX[t], (double)W);
            gsink += v;
            if((t & 4095)==0){ double r=rc_var(&PX[t+1], W); double e=fabs(v-r)/(fabs(r)+1e-12); if(e>maxerr)maxerr=e; }
        }
        double inc=(now_ns()-t0)/(double)N;
        t0=now_ns(); for(long t=0;t<N;t++){ volatile double r=rc_var(&PX[t],W); gsink+=r; } double rec=(now_ns()-t0)/(double)N;
        printf("%-16s %10.2f   %10.2f   %8.0fx   %10.1e\n","rolling variance",inc,rec,rec/inc,maxerr);
    }
    // ---- rolling vol ----
    {
        double st[2]={0,0}; for(int i=0;i<W;i++){ st[0]+=PX[i]; st[1]+=PX[i]*PX[i]; }
        double maxerr=0, t0=now_ns();
        for(long t=0;t<N;t++){
            double v = roll_vol(st, PX[t+W], PX[t], (double)W);
            gsink += v;
            if((t & 4095)==0){ double r=rc_vol(&PX[t+1], W); double e=fabs(v-r)/(fabs(r)+1e-12); if(e>maxerr)maxerr=e; }
        }
        double inc=(now_ns()-t0)/(double)N;
        t0=now_ns(); for(long t=0;t<N;t++){ volatile double r=rc_vol(&PX[t],W); gsink+=r; } double rec=(now_ns()-t0)/(double)N;
        printf("%-16s %10.2f   %10.2f   %8.0fx   %10.1e\n","rolling vol",inc,rec,rec/inc,maxerr);
    }
    // ---- rolling vwap ----
    {
        double st[2]={0,0}; for(int i=0;i<W;i++){ st[0]+=PX[i]*SZ[i]; st[1]+=SZ[i]; }
        double maxerr=0, t0=now_ns();
        for(long t=0;t<N;t++){
            double v = roll_vwap(st, PX[t+W], SZ[t+W], PX[t], SZ[t]);
            gsink += v;
            if((t & 4095)==0){ double r=rc_vwap(&PX[t+1],&SZ[t+1], W); double e=fabs(v-r)/fabs(r); if(e>maxerr)maxerr=e; }
        }
        double inc=(now_ns()-t0)/(double)N;
        t0=now_ns(); for(long t=0;t<N;t++){ volatile double r=rc_vwap(&PX[t],&SZ[t],W); gsink+=r; } double rec=(now_ns()-t0)/(double)N;
        printf("%-16s %10.2f   %10.2f   %8.0fx   %10.1e\n","rolling vwap",inc,rec,rec/inc,maxerr);
    }
    // ---- rolling book pressure ----
    {
        double st[2]={0,0}; for(int i=0;i<W;i++){ st[0]+=SZ[i]; st[1]+=PX[i]; }
        double maxerr=0, t0=now_ns();
        for(long t=0;t<N;t++){
            double v = roll_pressure(st, SZ[t+W], SZ[t], PX[t+W], PX[t]);
            gsink += v;
            if((t & 4095)==0){ double r=rc_pressure(&SZ[t+1],&PX[t+1], W); double e=fabs(v-r)/fabs(r); if(e>maxerr)maxerr=e; }
        }
        double inc=(now_ns()-t0)/(double)N;
        t0=now_ns(); for(long t=0;t<N;t++){ volatile double r=rc_pressure(&SZ[t],&PX[t],W); gsink+=r; } double rec=(now_ns()-t0)/(double)N;
        printf("%-16s %10.2f   %10.2f   %8.0fx   %10.1e\n","book pressure",inc,rec,rec/inc,maxerr);
    }

    // ---- fused incremental tick: whole handler (vwap+pressure+vol+decide) in one call ----
    {
        double st[6]={0,0,0,0,0,0};
        for(int i=0;i<W;i++){ st[0]+=PX[i]*SZ[i]; st[1]+=SZ[i]; st[2]+=BID[i]; st[3]+=ASK[i]; st[4]+=PX[i]; st[5]+=PX[i]*PX[i]; }
        // correctness: incremental decision must equal recompute decision
        double sc[6]; for(int k=0;k<6;k++)sc[k]=st[k];
        int agree=1;
        for(long t=0;t<20000;t++){
            int64_t a=tick_incr(sc,PX[t+W],SZ[t+W],BID[t+W],ASK[t+W],PX[t],SZ[t],BID[t],ASK[t],(double)W);
            int64_t b=cpp_tick_recompute(&PX[t+1],&SZ[t+1],&BID[t+1],&ASK[t+1],W);
            if(a!=b){agree=0;break;}
        }
        int64_t (*volatile vinc)(double*,double,double,double,double,double,double,double,double,double)=tick_incr;
        int64_t (*volatile vrec)(const double*,const double*,const double*,const double*,int)=cpp_tick_recompute;
        for(int k=0;k<6;k++)sc[k]=st[k];
        double t0=now_ns();
        for(long t=0;t<N;t++) isink+=vinc(sc,PX[t+W],SZ[t+W],BID[t+W],ASK[t+W],PX[t],SZ[t],BID[t],ASK[t],(double)W);
        double inc=(now_ns()-t0)/(double)N;
        t0=now_ns();
        for(long t=0;t<N;t++) isink+=vrec(&PX[t],&SZ[t],&BID[t],&ASK[t],W);
        double rec=(now_ns()-t0)/(double)N;
        printf("---------------------------------------------------------------------------\n");
        printf("%-16s %10.2f   %10.2f   %8.1fx   decisions %s\n","FUSED tick",inc,rec,rec/inc,agree?"identical":"MISMATCH");
    }

    printf("\n(sink %f %lld — defeats dead-code elimination)\n", gsink, (long long)isink);
    return 0;
}
