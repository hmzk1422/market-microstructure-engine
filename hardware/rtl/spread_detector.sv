module spread_detector #(
    parameter DATA_WIDTH = 32,
    parameter THRESHOLD  = 50    // Spread threshold value
)(
    input  logic                   clk,
    input  logic                   rst_n, // Active-low reset

    // AXI-Stream Slave Interface (Receiving data from your C++ pipeline)
    input  logic [DATA_WIDTH-1:0]  s_axis_tdata,
    input  logic                   s_axis_tvalid,
    output logic                   s_axis_tready,

    // Output Trigger (Signals your trading engine/strategy when spread is breached)
    output logic                   threshold_breached_o,
    output logic [DATA_WIDTH-1:0]  current_spread_o
);

    // We are always ready to accept data from the pipeline in this basic design
    assign s_axis_tready = 1'b1;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            threshold_breached_o <= 1'b0;
            current_spread_o     <= '0;
        end else if (s_axis_tvalid && s_axis_tready) begin
            // Store current incoming spread
            current_spread_o <= s_axis_tdata;

            // Check against threshold logic at hardware speed
            if (s_axis_tdata >= THRESHOLD) begin
                threshold_breached_o <= 1'b1;
            end else begin
                threshold_breached_o <= 1'b0;
            end
        end
    end

endmodule