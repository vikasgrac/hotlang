#include <stdint.h>
#include <stdio.h>
#include <time.h>
extern uint32_t bucketize(const uint32_t*, uint32_t*);
extern int32_t cpp_bucketize(const int32_t*, int32_t*);
static double now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
static uint64_t rng=1; static uint32_t nv(){rng=rng*6364136223846793005ull+1;return (uint32_t)((rng>>20)&0xFFFFF);}
#define N 4096
static _Alignas(64) uint32_t P[N], O[N]; static _Alignas(64) int32_t PS[N], OS[N];
static uint64_t sink=0;
int main(void){
  for(int i=0;i<N;i++){uint32_t v=100000+nv();P[i]=v;PS[i]=(int32_t)v;}
  // correctness
  bucketize(P,O); cpp_bucketize(PS,OS);
  int ok=1; for(int i=0;i<N;i++) if((int32_t)O[i]!=OS[i]) ok=0;
  printf("correctness: %s\n", ok?"identical outputs":"DIFFER");
  uint32_t (*volatile vh)(const uint32_t*,uint32_t*)=bucketize;
  int32_t (*volatile vc)(const int32_t*,int32_t*)=cpp_bucketize;
  const long IT=200000; double bh=1e18,bc=1e18;
  for(int r=0;r<7;r++){
    double t0=now_ns(); for(long i=0;i<IT;i++)sink+=vh(P,O); double h=(now_ns()-t0)/IT; if(h<bh)bh=h;
    t0=now_ns(); for(long i=0;i<IT;i++)sink+=vc(PS,OS); double c=(now_ns()-t0)/IT; if(c<bc)bc=c;
  }
  printf("bucketize 4096, best-of-7 ns/call:\n");
  printf("  hotlang u32          %7.1f ns\n", bh);
  printf("  tuned C++ (signed)   %7.1f ns\n", bc);
  printf("  WIN: %.2fx\n", bc/bh);
  printf("(sink %llu)\n",(unsigned long long)sink);
  return 0;
}
