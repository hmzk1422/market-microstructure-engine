# Find Verilator package
find_package(verilator HINTS $ENV{VERILATOR_ROOT} REQUIRED)

# Generate C++ classes from SystemVerilog
verilate(spread_detector
  SOURCES
    hardware/rtl/spread_detector.sv
  TOP_MODULE
    spread_detector
  PREFIX
    Vspread_detector
  TRACE
)

# Create executable combining C++ testbench with Verilated SV module
add_executable(sim_spread_detector
  hardware/sim/sim_main.cpp
)

target_link_libraries(sim_spread_detector PRIVATE Vspread_detector)