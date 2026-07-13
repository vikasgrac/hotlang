#![crate_type="staticlib"]
#[no_mangle]
pub extern "C" fn basket_rust(w:*const f64,px:*const f64,traded:*const f64,th:f64,sig:*mut f64)->f64{
    unsafe{ let mut hits=0.0;
        for b in 0..32usize{ let mut fair=0.0;
            for k in 0..32usize{ fair += *w.add(b*32+k) * *px.add(k); }
            let e=*traded.add(b)-fair; *sig.add(b)=e; hits += if e>th||e< -th {1.0} else {0.0}; }
        hits } }
