// C++ contenders for the market-making tick->trade flow. flow_cpp = strict-IEEE
// default (what compiles without effort — the reductions stay serial). flow_cpp_tuned
// = __restrict + a scoped reassociate pragma (what a latency desk ships — vectorizes).
// Same logic field-for-field as examples/flow.hot.
#include <algorithm>

extern "C" double flow_cpp(const double* bp,const double* bq,const double* ap,const double* aq,
                           const double* aw,const double* ft,double th,double base,double maxq,double* out){
    double bsum=0,asum=0,bnot=0,anot=0,alpha=0;
    for(int i=0;i<32;i++){ bsum+=bq[i]; asum+=aq[i]; bnot+=bp[i]*bq[i]; anot+=ap[i]*aq[i]; alpha+=aw[i]*ft[i]; }
    double depth=bsum+asum, pressure=bsum/depth, vwap=(bnot+anot)/depth;
    double tb=bp[0],ta=ap[0],mid=(tb+ta)/2.0, micro=(tb*aq[0]+ta*bq[0])/(bq[0]+aq[0]);
    double edge=alpha+(micro-mid)+(vwap-mid);
    bool buy=pressure>0.6&&edge>th, sell=pressure<0.4&&edge<-th;
    double side=buy?1.0:(sell?2.0:0.0);
    double px=buy?ta:(sell?tb:0.0);
    double qty=buy?std::min(base+edge,maxq):(sell?std::min(base-edge,maxq):0.0);
    out[0]=side; out[1]=px; out[2]=qty; return side;
}

extern "C" double flow_cpp_tuned(const double* __restrict bp,const double* __restrict bq,
                                 const double* __restrict ap,const double* __restrict aq,
                                 const double* __restrict aw,const double* __restrict ft,
                                 double th,double base,double maxq,double* __restrict out){
    double bsum=0,asum=0,bnot=0,anot=0,alpha=0;
    { _Pragma("clang fp reassociate(on) contract(fast)")
      for(int i=0;i<32;i++){ bsum+=bq[i]; asum+=aq[i]; bnot+=bp[i]*bq[i]; anot+=ap[i]*aq[i]; alpha+=aw[i]*ft[i]; } }
    double depth=bsum+asum, pressure=bsum/depth, vwap=(bnot+anot)/depth;
    double tb=bp[0],ta=ap[0],mid=(tb+ta)/2.0, micro=(tb*aq[0]+ta*bq[0])/(bq[0]+aq[0]);
    double edge=alpha+(micro-mid)+(vwap-mid);
    bool buy=pressure>0.6&&edge>th, sell=pressure<0.4&&edge<-th;
    double side=buy?1.0:(sell?2.0:0.0);
    double px=buy?ta:(sell?tb:0.0);
    double qty=buy?std::min(base+edge,maxq):(sell?std::min(base-edge,maxq):0.0);
    out[0]=side; out[1]=px; out[2]=qty; return side;
}
