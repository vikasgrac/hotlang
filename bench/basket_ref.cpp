// C++ tuned contender: __restrict + scoped reassociate pragma, -O3 -march=native.
extern "C" double basket_cpp_tuned(const double* __restrict w,const double* __restrict px,
                                   const double* __restrict traded,double th,double* __restrict sig){
    double hits=0;
    for(int b=0;b<32;b++){ double fair=0;
        { _Pragma("clang fp reassociate(on) contract(fast)")
          for(int k=0;k<32;k++) fair+=w[b*32+k]*px[k]; }
        double e=traded[b]-fair; sig[b]=e; hits+=(e>th||e<-th)?1.0:0.0; }
    return hits;
}
