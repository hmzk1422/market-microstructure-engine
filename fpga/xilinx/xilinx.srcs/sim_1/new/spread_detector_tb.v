`timescale 1ns / 1ps

module spread_detector_tb;

    logic clk = 0;
    logic rst_n = 0;

    logic input_valid = 0;
    logic input_ready;
    logic quotes_valid = 0;

    logic [31:0] bybit_bid_i = 0;
    logic [31:0] bybit_ask_i = 0;
    logic [31:0] binance_bid_i = 0;
    logic [31:0] binance_ask_i = 0;

    logic [15:0] bybit_fee_bps_i = 0;
    logic [15:0] binance_fee_bps_i = 0;
    logic [48:0] min_profit_i = 0;
    logic [15:0] symbol_id_i = 0;
    logic [31:0] transaction_id_i = 0;

    logic candidate_valid_o;
    logic [1:0] direction_o;
    logic signed [48:0] net_profit_o;
    logic [15:0] symbol_id_o;
    logic [31:0] transaction_id_o;
    logic [31:0] latency_o;

    // 100 MHz test clock: 10 ns period.
    always #5 clk = ~clk;

    spread_detector dut (
        .clk(clk),
        .rst_n(rst_n),
        .input_valid(input_valid),
        .input_ready(input_ready),
        .quotes_valid(quotes_valid),
        .bybit_bid_i(bybit_bid_i),
        .bybit_ask_i(bybit_ask_i),
        .binance_bid_i(binance_bid_i),
        .binance_ask_i(binance_ask_i),
        .bybit_fee_bps_i(bybit_fee_bps_i),
        .binance_fee_bps_i(binance_fee_bps_i),
        .min_profit_i(min_profit_i),
        .symbol_id_i(symbol_id_i),
        .transaction_id_i(transaction_id_i),
        .candidate_valid_o(candidate_valid_o),
        .direction_o(direction_o),
        .net_profit_o(net_profit_o),
        .symbol_id_o(symbol_id_o),
        .transaction_id_o(transaction_id_o),
        .latency_o(latency_o)
    );

    initial begin
        $display("Starting spread_detector test");

        // Hold reset, then release it.
        repeat (2) @(negedge clk);
        rst_n = 1;

        // Submit a profitable quote:
        // Buy on Bybit at 100, sell on Binance at 120.
        @(negedge clk);
        bybit_ask_i = 100;
        bybit_bid_i = 99;
        binance_bid_i = 120;
        binance_ask_i = 121;

        bybit_fee_bps_i = 0;
        binance_fee_bps_i = 0;
        min_profit_i = 100;
        symbol_id_i = 7;
        transaction_id_i = 1234;

        quotes_valid = 1;
        input_valid = 1;

        // Keep input stable through the sampling edge.
        @(negedge clk);
        input_valid = 0;
        quotes_valid = 0;

        // Allow pipeline output to settle.
        repeat (2) @(negedge clk);

        $display("candidate_valid_o = %0d", candidate_valid_o);
        $display("direction_o       = %0d", direction_o);
        $display("net_profit_o      = %0d", net_profit_o);
        $display("symbol_id_o       = %0d", symbol_id_o);
        $display("transaction_id_o  = %0d", transaction_id_o);
        $display("latency_o         = %0d cycles", latency_o);

        if (candidate_valid_o !== 1'b0)
            $display("NOTE: candidate_valid_o is a one-cycle pulse; inspect waveform.");
        $display("Test stimulus completed.");
        $finish;
    end

endmodule
