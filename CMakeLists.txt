cmake_minimum_required(VERSION 3.20)
project(BybitIngestion)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fix the Boost Warning by targeting Windows 10
# Fix the Boost Warning ONLY when compiling on Windows
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

# Find Boost, OpenSSL, PostgreSQL, and nlohmann-json
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(nlohmann_json REQUIRED)

# Create the executable
add_executable(ws_ingestion
    src/ingestion/websocket/main.cpp
    src/ingestion/storage/timescale_writer.cpp
)

# Link the required libraries (Notice pthread is GONE)
target_link_libraries(ws_ingestion PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    PostgreSQL::PostgreSQL
    nlohmann_json::nlohmann_json
)

# Include directories
target_include_directories(ws_ingestion PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)