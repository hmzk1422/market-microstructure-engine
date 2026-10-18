#include <iostream>
#include <memory>
#include "Vspread_detector.h"
#include "verilated.h"

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    // Instantiate the Verilated spread detector module
    auto tfp = std::make_unique<Vspread_detector>();

    std::cout << "[INFO] Starting Verilated Spread Detector Simulation..." << std::endl;

    // Apply active-low reset
    tfp->rst_n = 0;
    tfp->clk = 0;
    tfp->s_axis_tvalid = 0;
    tfp->s_axis_tdata = 0;
    tfp->eval();

    // Release reset
    tfp->clk = 1; tfp->eval();
    tfp->clk = 0; tfp->eval();
    tfp->rst_n = 1;

    // Test vectors: simulating incoming spreads
    // Threshold is set to 50 in spread_detector.sv
    uint32_t test_spreads[] = {10, 25, 49, 50, 75, 100, 30};

    for (uint32_t spread : test_spreads) {
        // Clock low: drive inputs
        tfp->clk = 0;
        tfp->s_axis_tdata = spread;
        tfp->s_axis_tvalid = 1;
        tfp->eval();

        // Clock high: capture outputs on positive edge
        tfp->clk = 1;
        tfp->eval();

        std::cout << "[SIM] Input Spread: " << spread 
                  << " | Current Spread Reg: " << tfp->current_spread_o 
                  << " | Threshold Breached: " << (int)tfp->threshold_breached_o 
                  << std::endl;
    }

    tfp->final();
    std::cout << "[INFO] Verilator Simulation Passed Successfully." << std::endl;
    return 0;
}