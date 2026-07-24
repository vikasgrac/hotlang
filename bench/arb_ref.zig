// Zig contender for the futures calendar-spread arb. Same logic as
// examples/arb.hot, field for field. Built -O ReleaseFast -mcpu=native.
// C ABI (export fn) so the shared C harness drives it identically.

export fn arb_zig(tick: [*]const i64, fair: i64, thresh: i64, out: [*]i64) i64 {
    const n_bid_px = tick[0];
    const n_bid_qty = tick[1];
    const n_ask_px = tick[2];
    const n_ask_qty = tick[3];
    const f_bid_px = tick[4];
    const f_bid_qty = tick[5];
    const f_ask_px = tick[6];
    const f_ask_qty = tick[7];

    const edge_rich: i64 = (f_bid_px - n_ask_px) - fair; // sell far / buy near
    const edge_cheap: i64 = fair - (f_ask_px - n_bid_px); // buy far / sell near
    const rich_ok = edge_rich > thresh;
    const cheap_ok = edge_cheap > thresh;
    const sig: i64 = if (rich_ok and edge_rich >= edge_cheap) 1 else (if (cheap_ok) 2 else 0);

    const qty_rich = @min(f_bid_qty, n_ask_qty);
    const qty_cheap = @min(f_ask_qty, n_bid_qty);
    const qty: i64 = if (sig == 1) qty_rich else qty_cheap;

    const near_side: i64 = if (sig == 1) 1 else (if (sig == 2) 2 else 0);
    const near_px: i64 = if (sig == 1) n_ask_px else n_bid_px;
    const far_side: i64 = if (sig == 1) 2 else (if (sig == 2) 1 else 0);
    const far_px: i64 = if (sig == 1) f_bid_px else f_ask_px;

    const none = (sig == 0);
    out[0] = near_side;
    out[1] = if (none) 0 else near_px;
    out[2] = if (none) 0 else qty;
    out[3] = far_side;
    out[4] = if (none) 0 else far_px;
    out[5] = if (none) 0 else qty;
    return sig;
}
