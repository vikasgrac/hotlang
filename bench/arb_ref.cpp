// C++ contender for the futures calendar-spread arb. Idiomatic, tuned:
// __restrict pointers, -O3 -march=native. Same logic as examples/arb.hot,
// field for field, so outputs are bitwise-identical to every other language.
#include <cstdint>
#include <algorithm>

extern "C" int64_t arb_cpp(const int64_t* __restrict tick, int64_t fair,
                           int64_t thresh, int64_t* __restrict out) {
    int64_t n_bid_px=tick[0], n_bid_qty=tick[1], n_ask_px=tick[2], n_ask_qty=tick[3];
    int64_t f_bid_px=tick[4], f_bid_qty=tick[5], f_ask_px=tick[6], f_ask_qty=tick[7];

    int64_t edge_rich  = (f_bid_px - n_ask_px) - fair;   // sell far / buy near
    int64_t edge_cheap = fair - (f_ask_px - n_bid_px);   // buy far  / sell near
    bool rich_ok  = edge_rich  > thresh;
    bool cheap_ok = edge_cheap > thresh;
    int64_t sig = (rich_ok && edge_rich >= edge_cheap) ? 1 : (cheap_ok ? 2 : 0);

    int64_t qty_rich  = std::min(f_bid_qty, n_ask_qty);
    int64_t qty_cheap = std::min(f_ask_qty, n_bid_qty);
    int64_t qty = (sig == 1) ? qty_rich : qty_cheap;

    int64_t near_side = (sig == 1) ? 1 : ((sig == 2) ? 2 : 0);
    int64_t near_px   = (sig == 1) ? n_ask_px : n_bid_px;
    int64_t far_side  = (sig == 1) ? 2 : ((sig == 2) ? 1 : 0);
    int64_t far_px    = (sig == 1) ? f_bid_px : f_ask_px;

    bool none = (sig == 0);
    out[0] = near_side;
    out[1] = none ? 0 : near_px;
    out[2] = none ? 0 : qty;
    out[3] = far_side;
    out[4] = none ? 0 : far_px;
    out[5] = none ? 0 : qty;
    return sig;
}
