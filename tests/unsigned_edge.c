#include <stdint.h>
#include <stdio.h>
extern uint32_t price_level(uint32_t, uint32_t);
extern uint64_t ring_slot(uint64_t, uint64_t);
extern uint32_t cap_size(uint32_t, uint32_t);
static int fails=0;
#define CK(e,w) do{unsigned long long g=(unsigned long long)(e);if(g!=(unsigned long long)(w)){printf("FAIL %-24s got %llu want %llu\n",#e,g,(unsigned long long)(w));fails++;}else printf("ok   %-24s = %llu\n",#e,g);}while(0)
int main(void){
  CK(price_level(10000,5), 2000);
  CK(price_level(10007,8), 1250);
  CK(price_level(100,0), 0);           // total: /0 -> 0
  CK(ring_slot(1000,64), 1000%64);
  CK(ring_slot(5,0), 5);               // total: %0 -> a
  CK(cap_size(500,100), 100);
  CK(cap_size(50,100), 50);
  CK(price_level(4294967295u,1), 4294967295u);  // full u32 range, no sign issues
  printf(fails?"\n%d UNSIGNED FAILURES\n":"\nALL UNSIGNED TESTS PASSED\n",fails);
  return fails!=0;
}
