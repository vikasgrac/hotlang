#include <stdint.h>
#include <stdio.h>
extern int32_t mid(int32_t,int32_t), spread(int32_t,int32_t), imbalance_bps(int32_t,int32_t);
extern int16_t delta_ticks(int32_t,int32_t);
static int fails=0;
#define CK(e,w) do{long g=(long)(e);if(g!=(long)(w)){printf("FAIL %-26s got %ld want %ld\n",#e,g,(long)(w));fails++;}else printf("ok   %-26s = %ld\n",#e,g);}while(0)
int main(void){
  CK(mid(10000,10002), 10001);
  CK(spread(10000,10002), 2);
  CK(spread(10002,10000), 0);          // crossed book -> 0
  CK(imbalance_bps(600,400), 2000);
  CK(imbalance_bps(300000,0), 10000);   // large sizes: no i32 overflow/sign-flip
  CK(imbalance_bps(0,250000), -10000);
  CK(imbalance_bps(0,0), 0);           // empty book -> 0, total
  CK(delta_ticks(60000123, 60000000), 123);
  CK(delta_ticks(59999900, 60000000), -100);
  printf(fails?"\n%d NARROW FAILURES\n":"\nALL NARROW TESTS PASSED\n",fails);
  return fails!=0;
}
