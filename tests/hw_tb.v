// Testbench: verify hotlang-generated hardware computes identically to the
// CPU semantics. Drives the combinational modules and checks outputs.
module tb;
  reg  signed [31:0] bid, ask, bid_sz, ask_sz, price, reference;
  wire signed [31:0] mid_o, spread_o, imb_o;
  wire signed [15:0] delta_o;
  integer fails;
  mid           u1(.bid(bid), .ask(ask), .out(mid_o));
  spread        u2(.bid(bid), .ask(ask), .out(spread_o));
  imbalance_bps u3(.bid_sz(bid_sz), .ask_sz(ask_sz), .out(imb_o));
  delta_ticks   u4(.price(price), .reference(reference), .out(delta_o));
  task chk(input signed [63:0] got, input signed [63:0] want);
    begin if (got !== want) begin $display("FAIL got %0d want %0d", got, want); fails=fails+1; end end
  endtask
  initial begin
    fails = 0;
    bid=10000; ask=10002; #1; chk(mid_o, 10001); chk(spread_o, 2);
    bid=10002; ask=10000; #1; chk(spread_o, 0);
    bid_sz=600; ask_sz=400; #1; chk(imb_o, 2000);
    bid_sz=0; ask_sz=0; #1; chk(imb_o, 0);
    price=60000123; reference=60000000; #1; chk(delta_o, 123);
    price=59999900; reference=60000000; #1; chk(delta_o, -100);
    if (fails==0) $display("ALL HARDWARE MATCHES THE CPU"); else $display("%0d HARDWARE MISMATCHES", fails);
    $finish;
  end
endmodule
