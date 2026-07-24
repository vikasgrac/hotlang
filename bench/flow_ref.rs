// Rust contender for the tick->trade flow. Safe idiomatic stable Rust: no scoped
// fp-reassociation exists on stable, so its reduction ceiling is the strict-IEEE
// (serial) number. Same logic field-for-field as examples/flow.hot.
#![crate_type = "staticlib"]

#[no_mangle]
pub extern "C" fn flow_rust(bp:*const f64,bq:*const f64,ap:*const f64,aq:*const f64,
                            aw:*const f64,ft:*const f64,th:f64,base:f64,maxq:f64,out:*mut f64)->f64{
    unsafe{
        let (mut bsum,mut asum,mut bnot,mut anot,mut alpha)=(0.0,0.0,0.0,0.0,0.0);
        for i in 0..32usize{
            bsum += *bq.add(i); asum += *aq.add(i);
            bnot += (*bp.add(i))*(*bq.add(i)); anot += (*ap.add(i))*(*aq.add(i));
            alpha += (*aw.add(i))*(*ft.add(i));
        }
        let depth=bsum+asum; let pressure=bsum/depth; let vwap=(bnot+anot)/depth;
        let (tb,ta)=(*bp.add(0),*ap.add(0)); let mid=(tb+ta)/2.0;
        let micro=(tb*(*aq.add(0))+ta*(*bq.add(0)))/((*bq.add(0))+(*aq.add(0)));
        let edge=alpha+(micro-mid)+(vwap-mid);
        let buy=pressure>0.6 && edge>th; let sell=pressure<0.4 && edge< -th;
        let side= if buy {1.0} else if sell {2.0} else {0.0};
        let px=   if buy {ta}  else if sell {tb}  else {0.0};
        let qty=  if buy {(base+edge).min(maxq)} else if sell {(base-edge).min(maxq)} else {0.0};
        *out.add(0)=side; *out.add(1)=px; *out.add(2)=qty; side
    }
}
