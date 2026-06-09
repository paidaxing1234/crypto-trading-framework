/**
 * @file websocket_callbacks.h
 * @brief WebSocket 回调设置模块
 */

#pragma once

#include "../../network/zmq_server.h"
#include <functional>
#include <string>

namespace trading {
namespace binance {
class BinanceWebSocket;
}
namespace server {

/**
 * @brief 设置 OKX WebSocket 回调
 */
void setup_websocket_callbacks(ZmqServer& zmq_server);

/**
 * @brief 设置 Binance WebSocket 回调（用于 g_binance_ws_market 等全局对象）
 */
void setup_binance_websocket_callbacks(ZmqServer& zmq_server);

/**
 * @brief 设置 Binance K线回调（用于动态创建的 K线连接）
 * @param ws Binance WebSocket 指针
 * @param zmq_server ZMQ 服务器引用
 */
void setup_binance_kline_callback(binance::BinanceWebSocket* ws, ZmqServer& zmq_server);
void setup_binance_kline_callback(
    binance::BinanceWebSocket* ws,
    ZmqServer& zmq_server,
    std::function<void(const std::string& symbol, int64_t timestamp)> on_closed_kline
);

} // namespace server
} // namespace trading
