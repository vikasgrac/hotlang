// C contenders for basket/index arb (matvec fair value). Plain = strict IEEE
// (what compiles without effort); tuned = restrict + a scoped reassociate
// pragma (what a latency desk ships). Both clang -O3 -march=native.
double basket_c(const double* w,const double* px,const double* traded,double th,double* sig){
    double hits=0;
    for(int b=0;b<32;b++){ double fair=0;
        for(int k=0;k<32;k++) fair+=w[b*32+k]*px[k];
        double e=traded[b]-fair; sig[b]=e; hits+=(e>th||e<-th)?1.0:0.0; }
    return hits;
}
double basket_c_tuned(const double* restrict w,const double* restrict px,
                      const double* restrict traded,double th,double* restrict sig){
    double hits=0;
    for(int b=0;b<32;b++){ double fair=0;
        { _Pragma("clang fp reassociate(on) contract(fast)")
          for(int k=0;k<32;k++) fair+=w[b*32+k]*px[k]; }
        double e=traded[b]-fair; sig[b]=e; hits+=(e>th||e<-th)?1.0:0.0; }
    return hits;
}
