// What C++'s signed default costs on `out[i] = price[i] % 8` (4096 elems).
// hotlang `u32` vs six C++ spellings of the identical loop. Every row but the
// naive `signed % 8` ties hotlang — the gap is the default *type*, not the
// language. Build: see tests/run.sh (links examples/unsigned.hot's bucketize).
#include <stdint.h>
#include <stdio.h>
#include <time.h>
extern uint32_t bucketize(const uint32_t*, uint32_t*);          // hotlang u32 %8
extern int32_t  cpp_signed_mod (const int32_t*,  int32_t*);      // signed %8 (slow)
extern int32_t  cpp_signed_mask(const int32_t*,  int32_t*);      // signed &7 (ties)
extern uint32_t cpp_unsigned   (const uint32_t*, uint32_t*);     // uint32 %8 (ties)
extern int32_t  cpp_assume     (const int32_t*,  int32_t*);      // signed+assume (ties)
extern int64_t  cpp_i64_mod    (const int64_t*,  int64_t*);      // signed i64 %8
extern uint64_t cpp_u64_mod    (const uint64_t*, uint64_t*);     // unsigned u64 %8

static double now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
static uint64_t rng=1; static uint32_t nv(){rng=rng*6364136223846793005ull+1;return (uint32_t)((rng>>20)&0xFFFFF);}
#define N 4096
static _Alignas(64) uint32_t P[N],O[N];   static _Alignas(64) int32_t PS[N],OS[N];
static _Alignas(64) uint64_t P8[N],O8[N]; static _Alignas(64) int64_t PS8[N],OS8[N];
static uint64_t sink=0;

#define TIME32U(fn,in,out) do{ double b=1e18; for(int r=0;r<9;r++){ double t0=now_ns(); \
  for(long i=0;i<IT;i++) sink+=fn(in,out); double d=(now_ns()-t0)/IT; if(d<b)b=d;} res=b; }while(0)
#define TIME32S(fn,in,out) do{ double b=1e18; for(int r=0;r<9;r++){ double t0=now_ns(); \
  for(long i=0;i<IT;i++) sink+=(uint32_t)fn(in,out); double d=(now_ns()-t0)/IT; if(d<b)b=d;} res=b; }while(0)
#define TIME64U(fn,in,out) do{ double b=1e18; for(int r=0;r<9;r++){ double t0=now_ns(); \
  for(long i=0;i<IT;i++) sink+=fn(in,out); double d=(now_ns()-t0)/IT; if(d<b)b=d;} res=b; }while(0)
#define TIME64S(fn,in,out) do{ double b=1e18; for(int r=0;r<9;r++){ double t0=now_ns(); \
  for(long i=0;i<IT;i++) sink+=(uint64_t)fn(in,out); double d=(now_ns()-t0)/IT; if(d<b)b=d;} res=b; }while(0)

int main(void){
  for(int i=0;i<N;i++){ uint32_t v=100000+nv(); P[i]=v; PS[i]=(int32_t)v; P8[i]=v; PS8[i]=(int64_t)v; }

  // correctness: every spelling must produce the identical remainder as hotlang.
  bucketize(P,O);
  int ok=1;
  cpp_signed_mod(PS,OS);  for(int i=0;i<N;i++) if((int32_t)O[i]!=OS[i]) ok=0;
  cpp_signed_mask(PS,OS); for(int i=0;i<N;i++) if((int32_t)O[i]!=OS[i]) ok=0;
  cpp_unsigned(P,O);      for(int i=0;i<N;i++) if(O[i]!=(P[i]%8u)) ok=0;
  printf("correctness: %s\n\n", ok?"all spellings identical output":"DIFFER");

  const long IT=200000; double res, hot;
  TIME32U(bucketize,P,O); hot=res;
  printf("bucketize 4096 (out[i]=price[i]%%8), best-of-9 ns/call:\n");
  printf("  %-42s %7.1f ns    -\n", "hotlang u32", hot);
  TIME32S(cpp_signed_mask,PS,OS); printf("  %-42s %7.1f ns    %.2fx\n","C++ int with x & 7 (pow2 mask idiom)",res,res/hot);
  TIME32U(cpp_unsigned,P,O);      printf("  %-42s %7.1f ns    %.2fx\n","C++ uint32_t (the type fix)",res,res/hot);
  TIME32S(cpp_assume,PS,OS);      printf("  %-42s %7.1f ns    %.2fx\n","C++ int + __builtin_assume (UB if negative)",res,res/hot);
  TIME32S(cpp_signed_mod,PS,OS);  printf("  %-42s %7.1f ns    %.2fx  <-- signed default\n","C++ int, naive x % 8",res,res/hot);
  printf("\n64-bit width (penalty shrinks with width):\n");
  TIME64U(cpp_u64_mod,P8,O8);     double u64=res; printf("  %-42s %7.1f ns\n","C++ uint64_t x % 8",u64);
  TIME64S(cpp_i64_mod,PS8,OS8);   printf("  %-42s %7.1f ns    %.2fx vs uint64_t\n","C++ int64_t  x % 8",res,res/u64);
  printf("\n(sink %llu)\n",(unsigned long long)sink);
  return 0;
}
