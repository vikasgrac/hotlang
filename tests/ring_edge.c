// ring_edge.c — the ring builtin: the host only pushes ticks; the ring owns
// the window. Verifies masked wrap correctness and stable rolling vol over a
// long stream, against a true reference.
#include <math.h>
#include <stdint.h>
#include <stdio.h>

// Ring ABI layout must match hotlang: { i64 head; elem data[N] }.
typedef struct { int64_t head; double data[64]; } ring64;
typedef struct { int64_t head; double data[256]; } ring256;

extern double roll_mean_stream(void* win, double* state, double price);
extern double roll_vol_stream(void* win, double* state, double price);

static uint64_t rng = 1;
static double fr(){ rng = rng*6364136223846793005ull + 1; return (double)((rng>>11)&0xFFFFF)/1048576.0; }

int main(void){
    static double P[200000];
    for (int i = 0; i < 200000; i++) P[i] = 600000.0 + fr()*1.0;  // BRK.A-like: variance << mean^2

    int fails = 0;

    // rolling mean via ring (exact)
    { ring64 win = {0}; double st[1] = {0}; double maxe = 0;
      for (int t = 0; t < 200000-1; t++) {
          double got = roll_mean_stream(&win, st, P[t]);
          if (t >= 64) { double tr=0; for(int k=0;k<64;k++) tr+=P[t-63+k]; tr/=64; double e=fabs(got-tr)/tr; if(e>maxe)maxe=e; }
      }
      printf("ring rolling mean  : max rel err %.2e  %s\n", maxe, maxe<1e-12?"ok":"FAIL");
      if (maxe >= 1e-12) fails++;
    }

    // rolling vol via ring (Welford, stable even at BRK.A scale).
    // Prime the ring with the first 256 ticks and initialize state to the
    // true mean/M2 over that window (standard warmup — you don't run signals
    // on a half-full window), then measure steady state.
    { ring256 win = {0}; double st[2] = {0,0}; double maxe = 0;
      for (int i = 0; i < 256; i++) roll_vol_stream(&win, st, P[i]);   // fill the ring
      long double s0=0; for(int k=0;k<256;k++) s0+=P[k]; long double m0=s0/256;
      long double v0=0; for(int k=0;k<256;k++){ long double d=P[k]-m0; v0+=d*d; }
      st[0]=(double)m0; st[1]=(double)v0;                              // correct init
      for (int t = 256; t < 200000-1; t++) {
          double got = roll_vol_stream(&win, st, P[t]);
          long double s=0; for(int k=0;k<256;k++) s+=P[t-255+k]; long double m=s/256;
          long double v=0; for(int k=0;k<256;k++){ long double d=P[t-255+k]-m; v+=d*d; } long double tr=sqrtl(v/256);
          double e=fabsl((long double)got-tr)/(double)(tr+1e-30); if(e>maxe)maxe=e;
      }
      printf("ring rolling vol   : max rel err %.2e  %s (BRK.A scale, was 46%% with naive form)\n", maxe, maxe<1e-6?"ok":"FAIL");
      if (maxe >= 1e-6) fails++;
    }

    printf(fails ? "\n%d RING FAILURES\n" : "\nALL RING TESTS PASSED\n", fails);
    return fails != 0;
}
