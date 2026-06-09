cmake_minimum_required(VERSION 3.16)
project(RealAccountTradingFramework LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ==================== 依赖查找 ====================
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(pybind11 REQUIRED)

# hiredis
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h
    PATHS /usr/include /usr/local/include)
find_library(HIREDIS_LIB hiredis
    PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)

# zmq
find_path(ZMQ_INCLUDE_DIR zmq.hpp
    PATHS /usr/include /usr/local/include $ENV{HOME}/miniconda3/include)
find_library(ZMQ_LIB zmq
    PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib $ENV{HOME}/miniconda3/lib)

# websocketpp (header-only)
find_path(WEBSOCKETPP_INCLUDE_DIR websocketpp/server.hpp)

# nlohmann_json (header-only)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)

# ==================== 公共编译选项 ====================
add_compile_definitions(USE_WEBSOCKETPP)
if(HIREDIS_LIB)
    add_compile_definitions(HAS_HIREDIS)
endif()

set(COMMON_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/trading
    ${ZMQ_INCLUDE_DIR}
    ${WEBSOCKETPP_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)
# hiredis include: 只在非 /usr/include 时添加（避免 -isystem /usr/include 破坏 GCC 头文件搜索）
if(HIREDIS_INCLUDE_DIR AND NOT HIREDIS_INCLUDE_DIR STREQUAL "/usr/include")
    list(APPEND COMMON_INCLUDE_DIRS ${HIREDIS_INCLUDE_DIR})
endif()

set(COMMON_LIBS
    ${HIREDIS_LIB}
    ${ZMQ_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    Threads::Threads
    stdc++fs
)

# ==================== 公共源文件 (库) ====================
set(CORE_SOURCES
    core/logger.cpp
)

set(NETWORK_SOURCES
    network/websocket_server.cpp
    network/ws_client.cpp
    network/zmq_server.cpp
)

set(ADAPTER_SOURCES
    adapters/okx/okx_rest_api.cpp
    adapters/okx/okx_websocket.cpp
    adapters/binance/binance_rest_api.cpp
    adapters/binance/binance_websocket.cpp
)

set(SERVER_LIB_SOURCES
    server/config/server_config.cpp
    server/callbacks/websocket_callbacks.cpp
    server/handlers/frontend_command_handler.cpp
    server/handlers/order_processor.cpp
    server/handlers/query_handler.cpp
    server/handlers/subscription_manager.cpp
    server/managers/account_manager.cpp
    server/managers/redis_data_provider.cpp
    server/managers/redis_recorder.cpp
)

set(KLINE_LIB_SOURCES
    server/klinedata/gap_detector.cpp
    server/klinedata/historical_data_fetcher.cpp
    server/klinedata/kline_utils.cpp
)

# ==================== 静态库 ====================
add_library(trading_core STATIC
    ${CORE_SOURCES}
    ${NETWORK_SOURCES}
    ${ADAPTER_SOURCES}
    ${SERVER_LIB_SOURCES}
    ${KLINE_LIB_SOURCES}
)
set_target_properties(trading_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(trading_core PUBLIC ${COMMON_INCLUDE_DIRS})
target_link_libraries(trading_core PUBLIC ${COMMON_LIBS})

# ==================== 可执行文件 ====================

# 1. trading_server_full
add_executable(trading_server_full server/trading_server_main.cpp)
target_link_libraries(trading_server_full PRIVATE trading_core)

# 2. data_recorder
add_executable(data_recorder server/klinedata/data_recorder.cpp)
target_link_libraries(data_recorder PRIVATE trading_core)

# 3. kline_gap_filler
add_executable(kline_gap_filler server/klinedata/kline_gap_filler.cpp)
target_link_libraries(kline_gap_filler PRIVATE trading_core)

# 4. kline_fast_filler
add_executable(kline_fast_filler server/klinedata/kline_fast_filler.cpp)
target_link_libraries(kline_fast_filler PRIVATE trading_core)

# ==================== pybind11 模块 ====================
pybind11_add_module(strategy_base strategies/core/py_strategy_bindings.cpp)
target_link_libraries(strategy_base PRIVATE trading_core)

