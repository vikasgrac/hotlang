// Rust contender for the futures calendar-spread arb. Same logic as
// examples/arb.hot, field for field. Built -O, --target-cpu=native.
// C ABI so the shared C harness drives it identically to the others.
#![crate_type = "staticlib"]

#[no_mangle]
pub extern "C" fn arb_rust(tick: *const i64, fair: i64, thresh: i64, out: *mut i64) -> i64 {
    unsafe {
        let n_bid_px = *tick.add(0); let n_bid_qty = *tick.add(1);
        let n_ask_px = *tick.add(2); let n_ask_qty = *tick.add(3);
        let f_bid_px = *tick.add(4); let f_bid_qty = *tick.add(5);
        let f_ask_px = *tick.add(6); let f_ask_qty = *tick.add(7);

        let edge_rich  = (f_bid_px - n_ask_px) - fair;   // sell far / buy near
        let edge_cheap = fair - (f_ask_px - n_bid_px);   // buy far  / sell near
        let rich_ok  = edge_rich  > thresh;
        let cheap_ok = edge_cheap > thresh;
        let sig: i64 = if rich_ok && edge_rich >= edge_cheap { 1 }
                       else if cheap_ok { 2 } else { 0 };

        let qty_rich  = f_bid_qty.min(n_ask_qty);
        let qty_cheap = f_ask_qty.min(n_bid_qty);
        let qty = if sig == 1 { qty_rich } else { qty_cheap };

        let near_side = if sig == 1 { 1 } else if sig == 2 { 2 } else { 0 };
        let near_px   = if sig == 1 { n_ask_px } else { n_bid_px };
        let far_side  = if sig == 1 { 2 } else if sig == 2 { 1 } else { 0 };
        let far_px    = if sig == 1 { f_bid_px } else { f_ask_px };

        let none = sig == 0;
        *out.add(0) = near_side;
        *out.add(1) = if none { 0 } else { near_px };
        *out.add(2) = if none { 0 } else { qty };
        *out.add(3) = far_side;
        *out.add(4) = if none { 0 } else { far_px };
        *out.add(5) = if none { 0 } else { qty };
        sig
    }
}
