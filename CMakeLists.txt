cmake_minimum_required(VERSION 3.20)
project(BybitIngestion)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

# Find software dependencies
find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(nlohmann_json REQUIRED)

# Find Verilator for hardware co-simulation
find_package(verilator HINTS $ENV{VERILATOR_ROOT} REQUIRED)

add_library(spread_detector STATIC)
# 1. Verilate the SystemVerilog spread detector into C++ classes
verilate(spread_detector
    SOURCES
        hardware/rtl/spread_detector.sv
    TOP_MODULE
        spread_detector
    PREFIX
        Vspread_detector
    TRACE
)

# 2. Create the main ingestion engine executable
add_executable(ws_ingestion
    src/pipeline/main.cpp
    src/storage/timescale_writer.cpp
)

# 3. Link software libraries AND the Verilated hardware module
target_link_libraries(ws_ingestion PRIVATE
    Boost::boost
    OpenSSL::SSL
    OpenSSL::Crypto
    PostgreSQL::PostgreSQL
    nlohmann_json::nlohmann_json
    spread_detector   # <-- Links your FPGA hardware module directly into C++
)

# Include directories (ensures Vspread_detector.h can be found)
target_include_directories(ws_ingestion PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)