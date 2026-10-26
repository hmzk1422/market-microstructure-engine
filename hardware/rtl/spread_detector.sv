
module spread_detector (
    input  logic              clk,
    input  logic              rst_n,

    input  logic              input_valid,
    output logic              input_ready,
    input  logic              quotes_valid,
    input  logic [31:0]       bybit_bid_i,
    input  logic [31:0]       bybit_ask_i,
    input  logic [31:0]       binance_bid_i,
    input  logic [31:0]       binance_ask_i,
    input  logic [15:0]       bybit_fee_bps_i,
    input  logic [15:0]       binance_fee_bps_i,
    input  logic [48:0]       min_profit_i,
    input  logic [15:0]       symbol_id_i,
    input  logic [31:0]       transaction_id_i,

    // C++ strategy response returned into the simulated RTL execution edge.
    input  logic              decision_valid_i,
    input  logic              decision_execute_i,
    input  logic [31:0]       decision_quantity_i,
    input  logic [31:0]       decision_transaction_id_i,

    output logic              candidate_valid_o,
    output logic [1:0]        direction_o,
    output logic signed [48:0] net_profit_o,
    output logic [15:0]       symbol_id_o,
    output logic [31:0]       transaction_id_o,
    output logic [31:0]       latency_o,
    output logic              decision_ack_valid_o,
    output logic              decision_executed_o,
    output logic [31:0]       decision_quantity_o,
    output logic [31:0]       decision_transaction_id_o
);

    localparam logic [15:0] FEE_SCALE = 16'd10000;
    localparam logic [1:0] NO_DIRECTION = 2'd0;
    localparam logic [1:0] BUY_BYBIT_SELL_BINANCE = 2'd1;
    localparam logic [1:0] BUY_BINANCE_SELL_BYBIT = 2'd2;

    // Free-running cycle counter for latency measurement.
    logic [31:0] cycle_counter;
    logic [31:0] accepted_cycle;

    // Input pipeline registers.
    logic        stage_valid;
    logic        quotes_valid_q;
    logic [31:0] bybit_bid_q, bybit_ask_q;
    logic [31:0] binance_bid_q, binance_ask_q;
    logic [15:0] bybit_fee_q, binance_fee_q;
    logic [48:0] min_profit_q;
    logic [15:0] symbol_id_q;
    logic [31:0] transaction_id_q;

    logic [47:0] bybit_bid_proceeds;
    logic [47:0] bybit_ask_cost;
    logic [47:0] binance_bid_proceeds;
    logic [47:0] binance_ask_cost;

    logic signed [48:0] buy_bybit_net;
    logic signed [48:0] buy_binance_net;
    logic signed [48:0] best_net;
    logic [1:0] best_direction;
    logic fees_valid;

    // One input can be accepted every cycle.
    assign input_ready = 1'b1;

    assign fees_valid =
        (bybit_fee_q <= FEE_SCALE) &&
        (binance_fee_q <= FEE_SCALE);

    // Combinational spread calculation uses captured inputs.
    always_comb begin
        bybit_bid_proceeds =
            {16'b0, bybit_bid_q} *
            {32'b0, (FEE_SCALE - bybit_fee_q)};

        bybit_ask_cost =
            {16'b0, bybit_ask_q} *
            {32'b0, (FEE_SCALE + bybit_fee_q)};

        binance_bid_proceeds =
            {16'b0, binance_bid_q} *
            {32'b0, (FEE_SCALE - binance_fee_q)};

        binance_ask_cost =
            {16'b0, binance_ask_q} *
            {32'b0, (FEE_SCALE + binance_fee_q)};

        buy_bybit_net =
            $signed({1'b0, binance_bid_proceeds}) -
            $signed({1'b0, bybit_ask_cost});

        buy_binance_net =
            $signed({1'b0, bybit_bid_proceeds}) -
            $signed({1'b0, binance_ask_cost});

        if (buy_bybit_net >= buy_binance_net) begin
            best_net       = buy_bybit_net;
            best_direction = BUY_BYBIT_SELL_BINANCE;
        end else begin
            best_net       = buy_binance_net;
            best_direction = BUY_BINANCE_SELL_BYBIT;
        end
    end

    // Stage 1: accept and capture each input transaction.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cycle_counter   <= '0;
            accepted_cycle  <= '0;
            stage_valid     <= 1'b0;
            quotes_valid_q  <= 1'b0;
            bybit_bid_q     <= '0;
            bybit_ask_q     <= '0;
            binance_bid_q   <= '0;
            binance_ask_q   <= '0;
            bybit_fee_q     <= '0;
            binance_fee_q   <= '0;
            min_profit_q    <= '0;
            symbol_id_q     <= '0;
            transaction_id_q <= '0;
        end else begin
            cycle_counter <= cycle_counter + 1'b1;

            stage_valid <= input_valid && input_ready;

            if (input_valid && input_ready) begin
                accepted_cycle   <= cycle_counter;
                quotes_valid_q   <= quotes_valid;
                bybit_bid_q      <= bybit_bid_i;
                bybit_ask_q      <= bybit_ask_i;
                binance_bid_q    <= binance_bid_i;
                binance_ask_q    <= binance_ask_i;
                bybit_fee_q      <= bybit_fee_bps_i;
                binance_fee_q    <= binance_fee_bps_i;
                min_profit_q     <= min_profit_i;
                symbol_id_q      <= symbol_id_i;
                transaction_id_q <= transaction_id_i;
            end
        end
    end

    // Stage 2: produce the result and report elapsed cycles.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            candidate_valid_o <= 1'b0;
            direction_o       <= NO_DIRECTION;
            net_profit_o      <= '0;
            symbol_id_o       <= '0;
            transaction_id_o  <= '0;
            latency_o         <= '0;
            decision_ack_valid_o <= 1'b0;
            decision_executed_o <= 1'b0;
            decision_quantity_o <= '0;
            decision_transaction_id_o <= '0;
        end else begin
            candidate_valid_o <= 1'b0;
            direction_o       <= NO_DIRECTION;
            net_profit_o      <= '0;
            symbol_id_o       <= '0;
            transaction_id_o  <= '0;
            decision_ack_valid_o <= 1'b0;

            if (decision_valid_i) begin
                decision_ack_valid_o <= 1'b1;
                decision_executed_o <= decision_execute_i;
                decision_quantity_o <= decision_execute_i
                                           ? decision_quantity_i
                                           : 32'd0;
                decision_transaction_id_o <= decision_transaction_id_i;
            end

            if (stage_valid) begin
                // Difference is measured in clock cycles.
                latency_o <= cycle_counter - accepted_cycle;

                if (quotes_valid_q && fees_valid &&
                    bybit_bid_q != 0 && bybit_ask_q != 0 &&
                    binance_bid_q != 0 && binance_ask_q != 0) begin

                    net_profit_o     <= best_net;
                    symbol_id_o      <= symbol_id_q;
                    transaction_id_o <= transaction_id_q;

                    if (best_net > 0 &&
                        {1'b0, best_net} > {1'b0, min_profit_q}) begin
                        candidate_valid_o <= 1'b1;
                        direction_o       <= best_direction;
                    end
                end
            end
        end
    end

endmodule
