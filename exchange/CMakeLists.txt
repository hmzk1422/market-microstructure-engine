cmake_minimum_required(VERSION 3.20)
project(market_microstructure_exchange)

# C++20 for the exchange node.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

# ---------------------------------------------------------------------------
# Verilator RTL integration
# ---------------------------------------------------------------------------
# If Verilator is installed, verilate the SystemVerilog spread detector and use
# the real Vspread_detector. Otherwise, fall back to the mock implementation in
# verilator_hooked.h (cycle-accurate behavioural model only).
option(USE_REAL_VERILATOR "Use real Verilator-generated RTL (requires verilator)" OFF)

if(USE_REAL_VERILATOR)
    find_package(verilator HINTS $ENV{VERILATOR_ROOT} REQUIRED)
    verilate(spread_detector
        SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/../hardware/rtl/spread_detector.sv
        TOP_MODULE
            spread_detector
        PREFIX
            Vspread_detector
        TRACE
    )
    set(RTL_LIB Vspread_detector)
    get_target_property(VL_INCLUDE_DIRS Vspread_detector
        INTERFACE_INCLUDE_DIRECTORIES)
    list(APPEND RTL_INCLUDE_DIRS ${VL_INCLUDE_DIRS})
else()
    set(RTL_LIB )
    list(APPEND RTL_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ---------------------------------------------------------------------------
# Build the exchange node: UDS reader + RTL hook + terminal plot.
# ---------------------------------------------------------------------------
add_executable(exchange_node
    main.cpp
    socket_reader.cpp
    terminal_plot.cpp
)

target_link_libraries(exchange_node PRIVATE ${RTL_LIB})
target_include_directories(exchange_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RTL_INCLUDE_DIRS}
)

if(WIN32)
    target_link_libraries(exchange_node PRIVATE ws2_32)
endif()
