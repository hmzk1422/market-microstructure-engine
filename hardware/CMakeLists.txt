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

option(BUILD_VERILATED_RTL_TESTS
       "Build the synthesizable spread-detector tests when Verilator is available"
       ON)

if(BUILD_VERILATED_RTL_TESTS)
    find_package(verilator QUIET)
    if(verilator_FOUND)
        enable_testing()
        add_executable(spread_detector_rtl_test
            sim/spread_detector_test.cpp
        )
        verilate(spread_detector_rtl_test
            SOURCES rtl/spread_detector.sv
            TOP_MODULE spread_detector
            TRACE
        )
        add_test(
            NAME spread_detector_rtl_test
            COMMAND spread_detector_rtl_test
                    ${CMAKE_CURRENT_BINARY_DIR}/spread_detector.vcd
        )
    else()
        message(WARNING
            "Verilator was not found; spread_detector_rtl_test is unavailable")
    endif()
endif()

option(BUILD_MARKET_DATA_ENGINE
       "Build the ABIDES/WebSocket source-selectable Verilated application"
       ON)

if(BUILD_MARKET_DATA_ENGINE)
    find_package(verilator QUIET)
    if(verilator_FOUND)
        find_package(OpenSSL REQUIRED)
        find_package(Threads REQUIRED)

        add_executable(market_data_engine
            sim/source_main.cpp
            ../exchange/socket_reader.cpp
            ../exchange/websocket_feed.cpp
            ../exchange/websocket_common.cpp
            ../exchange/bybit/connector.cpp
            ../exchange/binance/connector.cpp
        )
        target_include_directories(market_data_engine PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../exchange
        )
        target_compile_definitions(market_data_engine PRIVATE
            BOOST_ERROR_CODE_HEADER_ONLY
        )
        target_link_libraries(market_data_engine PRIVATE
            OpenSSL::SSL
            OpenSSL::Crypto
            Threads::Threads
        )
        verilate(market_data_engine
            SOURCES rtl/spread_detector.sv
            TOP_MODULE spread_detector
        )
        if(WIN32)
            target_link_libraries(market_data_engine PRIVATE ws2_32)
        endif()
    else()
        message(WARNING
            "Verilator was not found; market_data_engine is unavailable")
    endif()
endif()