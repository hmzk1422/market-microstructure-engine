cmake_minimum_required(VERSION 3.20)
project(market_microstructure_sim)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

add_executable(sim_spread_detector
    sim/sim_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/socket_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../exchange/terminal_plot.cpp
)

target_include_directories(sim_spread_detector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../exchange
)

if(WIN32)
    target_link_libraries(sim_spread_detector PRIVATE ws2_32)
endif()