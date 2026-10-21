module spread_detector #(
    parameter DATA_WIDTH = 32,
    parameter THRESHOLD  = 200000000  // Z-score threshold (2.0 in 1e8 units, i.e. +/- 2 SD)
)(
    input  logic                   clk,
    input  logic                   rst_n, // Active-low reset

    // AXI-Stream Slave Interface (Receiving data from your C++ pipeline)
    // s_axis_tdata: z-score (scaled by 1e8, e.g. 2.0 -> 200000000)
    input  logic [DATA_WIDTH-1:0]  s_axis_tdata,
    input  logic                   s_axis_tvalid,
    output logic                   s_axis_tready,

    // Output Trigger (Signals your trading engine/strategy when z-score threshold is breached)
    output logic                   threshold_breached_o,
    output logic [DATA_WIDTH-1:0]  current_zscore_o
);

    // We are always ready to accept data from the pipeline in this basic design
    assign s_axis_tready = 1'b1;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            threshold_breached_o <= 1'b0;
            current_zscore_o     <= '0;
        end else if (s_axis_tvalid && s_axis_tready) begin
            // Store current incoming z-score
            current_zscore_o     <= s_axis_tdata;

            // Check against z-score threshold logic at hardware speed
            // Breached when |z-score| >= THRESHOLD
            if (s_axis_tdata >= THRESHOLD || s_axis_tdata <= -THRESHOLD) begin
                threshold_breached_o <= 1'b1;
            end else begin
                threshold_breached_o <= 1'b0;
            end
        end
    end

endmodule