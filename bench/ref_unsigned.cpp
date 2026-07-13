#include <cstdint>
// Realistic tuned C++: prices carried as signed int (what desks do), __restrict,
// -O3 -march=native. No __builtin_assume (nobody scatters those).
extern "C" int32_t cpp_bucketize(const int32_t* __restrict price, int32_t* __restrict out){
    for (int i=0;i<4096;i++) out[i] = price[i] % 8;
    return out[0];
}
