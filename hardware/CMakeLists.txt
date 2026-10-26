cmake_minimum_required(VERSION 3.20)
project(market_microstructure_sim)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

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

option(BUILD_STRATEGY_TESTS
       "Build strategy rolling-statistics and multi-symbol tests"
       ON)
if(BUILD_STRATEGY_TESTS)
    enable_testing()
    add_executable(strategy_core_test test/strategy_core_test.cpp)
    target_include_directories(strategy_core_test PRIVATE sim)
    add_test(NAME strategy_core_test COMMAND strategy_core_test)
    add_executable(spsc_ring_test test/spsc_ring_test.cpp)
    target_include_directories(spsc_ring_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../exchange)
    add_test(NAME spsc_ring_test COMMAND spsc_ring_test)
    target_link_libraries(spsc_ring_test PRIVATE Threads::Threads)
    add_executable(mmap_spsc_ring_test test/mmap_spsc_ring_test.cpp)
    target_link_libraries(mmap_spsc_ring_test PRIVATE Threads::Threads)
    add_test(NAME mmap_spsc_ring_test COMMAND mmap_spsc_ring_test)
    add_executable(network_latency_model_test
        test/network_latency_model_test.cpp)
    target_include_directories(network_latency_model_test PRIVATE latency)
    add_test(NAME network_latency_model_test
        COMMAND network_latency_model_test)
endif()

option(BUILD_VERILATED_RTL_TESTS
       "Build the synthesizable spread-detector tests when Verilator is available"
       ON)

if(BUILD_VERILATED_RTL_TESTS)
    find_package(verilator QUIET)
    if(verilator_FOUND)
        enable_testing()
        add_executable(spread_detector_rtl_test
            test/spread_detector_test.cpp
        )
        target_include_directories(spread_detector_rtl_test PRIVATE sim)
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
        find_package(PostgreSQL REQUIRED)
        find_package(Threads REQUIRED)

        add_executable(market_data_engine
            sim/source_main.cpp
            latency/network_latency_model.h
            metrics/timescale_telemetry.cpp
            ../exchange/socket_reader.cpp
            ../exchange/websocket_feed.cpp
            ../exchange/websocket_common.cpp
            ../exchange/bybit/connector.cpp
            ../exchange/binance/connector.cpp
        )
        target_include_directories(market_data_engine PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/latency
            ${CMAKE_CURRENT_SOURCE_DIR}/sim
            ${CMAKE_CURRENT_SOURCE_DIR}/../exchange
        )
        target_compile_definitions(market_data_engine PRIVATE
            BOOST_ERROR_CODE_HEADER_ONLY
        )
        target_link_libraries(market_data_engine PRIVATE
            OpenSSL::SSL
            OpenSSL::Crypto
            PostgreSQL::PostgreSQL
            Threads::Threads
        )
        verilate(market_data_engine
            SOURCES rtl/spread_detector.sv
            TOP_MODULE spread_detector
        )
        if(WIN32)
            target_link_libraries(market_data_engine PRIVATE ws2_32 Psapi)
        endif()

        # Keep the common developer workflow safe: people often build only
        # market_data_engine and then invoke CTest. Make every registered unit
        # and RTL test executable available from that target build as well.
        if(BUILD_STRATEGY_TESTS)
            add_dependencies(market_data_engine
                strategy_core_test
                spsc_ring_test
                mmap_spsc_ring_test
                network_latency_model_test
            )
        endif()
        if(TARGET spread_detector_rtl_test)
            add_dependencies(market_data_engine spread_detector_rtl_test)
        endif()

        enable_testing()
        set(STRESS_SYMBOLS)
        foreach(index RANGE 0 511)
            string(LENGTH "${index}" index_length)
            math(EXPR padding_length "4 - ${index_length}")
            string(REPEAT "0" ${padding_length} padding)
            list(APPEND STRESS_SYMBOLS "ASSET${padding}${index}")
        endforeach()
        list(JOIN STRESS_SYMBOLS "," STRESS_SYMBOL_CSV)
        add_test(
            NAME synthetic_512_symbol_pipeline
            COMMAND market_data_engine
                    --source synthetic
                    --symbols ${STRESS_SYMBOL_CSV}
                    --duration-sec 2
                    --synthetic-tick-ms 20
                    --no-telemetry
        )
        set_tests_properties(synthetic_512_symbol_pipeline PROPERTIES
            PASS_REGULAR_EXPRESSION
                "pending_decisions=0.*spread_mismatches=0.*decision_queue_dropped=0"
            TIMEOUT 30
        )
        set_tests_properties(synthetic_512_symbol_pipeline PROPERTIES LABELS "integration;performance")

        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(
                NAME performance_report_smoke
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/test/latency_resource_report.py
                        --binary $<TARGET_FILE:market_data_engine>
                        --duration-sec 3
                        --symbols 2
            )
            set_tests_properties(performance_report_smoke PROPERTIES
                LABELS "performance;report"
                TIMEOUT 45
            )
        endif()
    else()
        message(WARNING
            "Verilator was not found; market_data_engine is unavailable")
    endif()
endif()
