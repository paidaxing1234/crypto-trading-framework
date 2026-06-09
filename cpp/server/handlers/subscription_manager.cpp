/**
 * @file subscription_manager.cpp
 * @brief 订阅管理模块实现
 */

#include "subscription_manager.h"
#include "../config/server_config.h"
#include "../../adapters/okx/okx_websocket.h"
#include "../../adapters/binance/binance_websocket.h"
#include <iostream>
#include <algorithm>

namespace trading {
namespace server {

// OKX K线订阅引用计数：key = "symbol:interval", value = 引用计数
static std::map<std::string, int> g_okx_kline_ref_count;

void handle_subscription(const nlohmann::json& request) {
    std::string action = request.value("action", "subscribe");
    std::string channel = request.value("channel", "");
    std::string symbol = request.value("symbol", "");
    std::string interval = request.value("interval", "1m");
    std::string exchange = request.value("exchange", "okx");  // 默认 OKX
    std::string strategy_id = request.value("strategy_id", "");  // 策略ID（用于日志）

    std::cout << "[订阅] " << exchange << " | " << action << " | " << channel << " | " << symbol;
    if (!strategy_id.empty()) {
        std::cout << " | 策略:" << strategy_id;
    }
    std::cout << "\n";

    std::lock_guard<std::mutex> lock(g_sub_mutex);

    // ========================================
    // Binance 订阅处理
    // ========================================
    if (exchange == "binance" || exchange == "BINANCE") {
        // 转换为小写（Binance 要求小写 symbol）
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(), lower_symbol.begin(), ::tolower);

        if (channel == "trades" || channel == "trade") {
            if (action == "subscribe" && g_binance_ws_market) {
                g_binance_ws_market->subscribe_trade(lower_symbol);
                std::cout << "[订阅] Binance trades: " << symbol << " ✓\n";
            } else if (action == "unsubscribe" && g_binance_ws_market) {
                g_binance_ws_market->unsubscribe(lower_symbol + "@trade");
                std::cout << "[取消订阅] Binance trades: " << symbol << " ✓\n";
            }
        }
        else if (channel == "kline" || channel == "candle") {
            if (action == "subscribe" && g_binance_ws_market) {
                g_binance_ws_market->subscribe_kline(lower_symbol, interval);
                std::cout << "[订阅] Binance K线: " << symbol << " " << interval << " ✓\n";
            } else if (action == "unsubscribe" && g_binance_ws_market) {
                g_binance_ws_market->unsubscribe(lower_symbol + "@kline_" + interval);
                std::cout << "[取消订阅] Binance K线: " << symbol << " " << interval << " ✓\n";
            }
        }
        else if (channel == "orderbook" || channel == "depth") {
            int levels = request.value("levels", 20);
            if (action == "subscribe" && g_binance_ws_market) {
                g_binance_ws_market->subscribe_depth(lower_symbol, levels);
                std::cout << "[订阅] Binance 深度: " << symbol << " ✓\n";
            } else if (action == "unsubscribe" && g_binance_ws_market) {
                g_binance_ws_market->unsubscribe(lower_symbol + "@depth" + std::to_string(levels));
                std::cout << "[取消订阅] Binance 深度: " << symbol << " ✓\n";
            }
        }
        else if (channel == "mark_price" || channel == "markPrice") {
            if (action == "subscribe" && g_binance_ws_market) {
                g_binance_ws_market->subscribe_mark_price(lower_symbol);
                std::cout << "[订阅] Binance 标记价格: " << symbol << " ✓\n";
            }
        }
        return;
    }

    // ========================================
    // OKX 订阅处理（默认）
    // ========================================
    if (channel == "trades") {
        if (action == "subscribe" && g_ws_public) {
            if (g_subscribed_trades.find(symbol) == g_subscribed_trades.end()) {
                g_ws_public->subscribe_trades(symbol);
                g_subscribed_trades.insert(symbol);
                std::cout << "[订阅] OKX trades: " << symbol << " ✓\n";
            }
        } else if (action == "unsubscribe" && g_ws_public) {
            if (g_subscribed_trades.find(symbol) != g_subscribed_trades.end()) {
                g_ws_public->unsubscribe_trades(symbol);
                g_subscribed_trades.erase(symbol);
                std::cout << "[取消订阅] OKX trades: " << symbol << " ✓\n";
            }
        }
    }
    else if (channel == "kline" || channel == "candle") {
        std::string ref_key = symbol + ":" + interval;

        if (action == "subscribe" && g_ws_business) {
            // 检查 OKX WebSocket 是否已经订阅了（主程序可能直接订阅了）
            bool already_subscribed = (g_subscribed_klines[symbol].find(interval) != g_subscribed_klines[symbol].end());

            // 增加引用计数
            if (g_okx_kline_ref_count.find(ref_key) == g_okx_kline_ref_count.end()) {
                // 第一次通过 handle_subscription 订阅
                if (already_subscribed) {
                    // 但 OKX WebSocket 已经订阅了（主程序直接订阅的）
                    // 初始化引用计数为 2：主程序(1) + 当前策略(1)
                    g_okx_kline_ref_count[ref_key] = 2;
                    std::cout << "[订阅] OKX K线: " << symbol << " " << interval << " ✓ (检测到主程序已订阅，引用计数: 2)\n";
                } else {
                    // OKX WebSocket 还没订阅，这是第一次订阅
                    g_okx_kline_ref_count[ref_key] = 1;
                    g_ws_business->subscribe_kline(symbol, interval);
                    g_subscribed_klines[symbol].insert(interval);
                    std::cout << "[订阅] OKX K线: " << symbol << " " << interval << " ✓ (首次订阅)\n";
                }
            } else {
                // 已经有引用计数了，继续增加
                g_okx_kline_ref_count[ref_key]++;
                int ref_count = g_okx_kline_ref_count[ref_key];
                std::cout << "[订阅] OKX K线: " << symbol << " " << interval << " ✓ (引用计数: " << ref_count << ")\n";
            }
        } else if (action == "unsubscribe" && g_ws_business) {
            // 减少引用计数
            if (g_okx_kline_ref_count.find(ref_key) != g_okx_kline_ref_count.end()) {
                g_okx_kline_ref_count[ref_key]--;
                int ref_count = g_okx_kline_ref_count[ref_key];

                // 只有引用计数为0时才真正取消订阅
                if (ref_count <= 0) {
                    g_ws_business->unsubscribe_kline(symbol, interval);
                    g_subscribed_klines[symbol].erase(interval);
                    g_okx_kline_ref_count.erase(ref_key);
                    std::cout << "[取消订阅] OKX K线: " << symbol << " " << interval << " ✓ (已完全取消)\n";
                } else {
                    std::cout << "[取消订阅] OKX K线: " << symbol << " " << interval << " (保留订阅，引用计数: " << ref_count << ")\n";
                }
            } else {
                // 没有引用计数记录，说明是主程序直接订阅的，策略从未订阅过
                // 不应该取消订阅！
                std::cout << "[取消订阅] OKX K线: " << symbol << " " << interval << " (忽略，主程序订阅中)\n";
            }
        }
    }
    else if (channel == "orderbook" || channel == "books" || channel == "books5" ||
             channel == "bbo-tbt" || channel == "books-l2-tbt" ||
             channel == "books50-l2-tbt" || channel == "books-elp") {
        std::string depth_channel = channel == "orderbook" ? "books5" : channel;

        if (action == "subscribe" && g_ws_public) {
            g_ws_public->subscribe_orderbook(symbol, depth_channel);
            g_subscribed_orderbooks[symbol].insert(depth_channel);
            std::cout << "[订阅] OKX 深度: " << symbol << " " << depth_channel << " ✓\n";
        } else if (action == "unsubscribe" && g_ws_public) {
            g_ws_public->unsubscribe_orderbook(symbol, depth_channel);
            g_subscribed_orderbooks[symbol].erase(depth_channel);
            std::cout << "[取消订阅] OKX 深度: " << symbol << " " << depth_channel << " ✓\n";
        }
    }
    else if (channel == "funding_rate" || channel == "funding-rate") {
        if (action == "subscribe" && g_ws_public) {
            if (g_subscribed_funding_rates.find(symbol) == g_subscribed_funding_rates.end()) {
                g_ws_public->subscribe_funding_rate(symbol);
                g_subscribed_funding_rates.insert(symbol);
                std::cout << "[订阅] OKX 资金费率: " << symbol << " ✓\n";
            }
        } else if (action == "unsubscribe" && g_ws_public) {
            if (g_subscribed_funding_rates.find(symbol) != g_subscribed_funding_rates.end()) {
                g_ws_public->unsubscribe_funding_rate(symbol);
                g_subscribed_funding_rates.erase(symbol);
                std::cout << "[取消订阅] OKX 资金费率: " << symbol << " ✓\n";
            }
        }
    }
}

} // namespace server
} // namespace trading
