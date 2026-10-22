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

    output logic              candidate_valid_o,
    output logic [1:0]        direction_o,
    output logic signed [48:0] net_profit_o,
    output logic [15:0]       symbol_id_o,
    output logic [31:0]       transaction_id_o
);
    localparam logic [15:0] FEE_SCALE = 16'd10000;
    localparam logic [1:0] NO_DIRECTION = 2'd0;
    localparam logic [1:0] BUY_BYBIT_SELL_BINANCE = 2'd1;
    localparam logic [1:0] BUY_BINANCE_SELL_BYBIT = 2'd2;

    logic [47:0] bybit_bid_proceeds;
    logic [47:0] bybit_ask_cost;
    logic [47:0] binance_bid_proceeds;
    logic [47:0] binance_ask_cost;
    logic signed [48:0] buy_bybit_net;
    logic signed [48:0] buy_binance_net;
    logic signed [48:0] best_net;
    logic [1:0] best_direction;
    logic fees_valid;

    assign input_ready = 1'b1;
    assign fees_valid = (bybit_fee_bps_i <= FEE_SCALE) &&
                        (binance_fee_bps_i <= FEE_SCALE);

    always_comb begin
        bybit_bid_proceeds =
            {16'b0, bybit_bid_i} * {32'b0, (FEE_SCALE - bybit_fee_bps_i)};
        bybit_ask_cost =
            {16'b0, bybit_ask_i} * {32'b0, (FEE_SCALE + bybit_fee_bps_i)};
        binance_bid_proceeds =
            {16'b0, binance_bid_i} * {32'b0, (FEE_SCALE - binance_fee_bps_i)};
        binance_ask_cost =
            {16'b0, binance_ask_i} * {32'b0, (FEE_SCALE + binance_fee_bps_i)};

        buy_bybit_net =
            $signed({1'b0, binance_bid_proceeds}) -
            $signed({1'b0, bybit_ask_cost});
        buy_binance_net =
            $signed({1'b0, bybit_bid_proceeds}) -
            $signed({1'b0, binance_ask_cost});

        if (buy_bybit_net >= buy_binance_net) begin
            best_net = buy_bybit_net;
            best_direction = BUY_BYBIT_SELL_BINANCE;
        end else begin
            best_net = buy_binance_net;
            best_direction = BUY_BINANCE_SELL_BYBIT;
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            candidate_valid_o <= 1'b0;
            direction_o <= NO_DIRECTION;
            net_profit_o <= '0;
            symbol_id_o <= '0;
            transaction_id_o <= '0;
        end else begin
            candidate_valid_o <= 1'b0;
            direction_o <= NO_DIRECTION;
            net_profit_o <= '0;
            symbol_id_o <= '0;
            transaction_id_o <= '0;

            if (input_valid && input_ready && quotes_valid && fees_valid &&
                bybit_bid_i != 0 && bybit_ask_i != 0 &&
                binance_bid_i != 0 && binance_ask_i != 0) begin
                net_profit_o <= best_net;
                symbol_id_o <= symbol_id_i;
                transaction_id_o <= transaction_id_i;
                if (best_net > 0 &&
                    {1'b0, best_net} > {1'b0, min_profit_i}) begin
                    candidate_valid_o <= 1'b1;
                    direction_o <= best_direction;
                end
            end
        end
    end
endmodule
