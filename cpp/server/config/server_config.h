/**
 * @file server_config.h
 * @brief 交易服务器全局配置和状态
 *
 * 注意: 推荐使用 config_center.h 中的 ConfigCenter 进行配置管理
 * 本文件保留向后兼容，新代码请使用 ConfigCenter
 */

#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <map>
#include <string>
#include <vector>
#include <memory>

#include <sys/types.h>
#include <nlohmann/json.hpp>
#include "../../core/config_center.h"

// 前向声明
namespace trading {
namespace okx {
class OKXWebSocket;
}
namespace binance {
class BinanceWebSocket;
class BinanceRestAPI;
}
namespace core {
class WebSocketServer;
}
namespace auth {
class AuthManager;
struct TokenInfo;
}
class AccountRegistry;
}

namespace trading {
namespace server {

// ============================================================
// 全局配置 (向后兼容，推荐使用 ConfigCenter)
// ============================================================

namespace Config {
    // OKX 配置 (现在从 ConfigCenter 获取)
    extern std::string api_key;
    extern std::string secret_key;
    extern std::string passphrase;
    extern bool is_testnet;
    extern std::vector<std::string> default_symbols;
    extern std::vector<std::string> spot_symbols;
    extern std::vector<std::string> swap_symbols;

    // Binance 配置 (现在从 ConfigCenter 获取)
    extern std::string binance_api_key;
    extern std::string binance_secret_key;
    extern bool binance_is_testnet;
    extern std::vector<std::string> binance_symbols;
}

// ============================================================
// 全局状态
// ============================================================

extern std::atomic<bool> g_running;

// 统计
extern std::atomic<uint64_t> g_trade_count;
extern std::atomic<uint64_t> g_kline_count;
extern std::atomic<uint64_t> g_orderbook_count;
extern std::atomic<uint64_t> g_funding_rate_count;
extern std::atomic<uint64_t> g_order_count;
extern std::atomic<uint64_t> g_order_success;
extern std::atomic<uint64_t> g_order_failed;
extern std::atomic<uint64_t> g_query_count;

// 分交易所统计
// OKX: Ticker + Trades + K线 (无深度)
extern std::atomic<uint64_t> g_okx_ticker_count;
extern std::atomic<uint64_t> g_okx_trade_count;
extern std::atomic<uint64_t> g_okx_kline_count;

// Binance: Ticker + MarkPrice(资金费率) + K线 (无Trades/深度)
extern std::atomic<uint64_t> g_binance_ticker_count;
extern std::atomic<uint64_t> g_binance_markprice_count;
extern std::atomic<uint64_t> g_binance_kline_count;

// 订阅管理
extern std::mutex g_sub_mutex;
extern std::set<std::string> g_subscribed_trades;
extern std::map<std::string, std::set<std::string>> g_subscribed_klines;
extern std::map<std::string, std::set<std::string>> g_subscribed_orderbooks;
extern std::set<std::string> g_subscribed_funding_rates;

// WebSocket 客户端指针 - OKX
extern std::unique_ptr<okx::OKXWebSocket> g_ws_public;
extern std::unique_ptr<okx::OKXWebSocket> g_ws_business;
extern std::unique_ptr<okx::OKXWebSocket> g_ws_private;

// WebSocket 客户端指针 - Binance
extern std::unique_ptr<binance::BinanceWebSocket> g_binance_ws_market;
extern std::unique_ptr<binance::BinanceWebSocket> g_binance_ws_depth;   // 深度数据专用连接
extern std::unique_ptr<binance::BinanceWebSocket> g_binance_ws_user;
extern std::vector<std::unique_ptr<binance::BinanceWebSocket>> g_binance_ws_klines;  // K线专用连接组（多个）
extern std::unique_ptr<binance::BinanceRestAPI> g_binance_rest_api;


// 前端 WebSocket 服务器
extern std::unique_ptr<core::WebSocketServer> g_frontend_server;

// 认证管理器
extern auth::AuthManager g_auth_manager;
extern std::map<int, auth::TokenInfo> g_authenticated_clients;
extern std::mutex g_auth_mutex;

// 账户注册管理器 - 使用 trading::g_account_registry (定义在 account_registry.h)

// ============================================================
// 工具函数
// ============================================================

/**
 * @brief 加载配置 (向后兼容)
 *
 * 推荐使用: config::ConfigCenter::instance().init("server.json")
 */
void load_config();

/**
 * @brief 使用配置中心初始化
 *
 * @param config_file 配置文件路径 (默认 server.json)
 * @return 是否成功
 */
bool init_config_center(const std::string& config_file = "server.json");

/**
 * @brief 热重载配置
 *
 * @return 是否有变更
 */
bool reload_config();

/**
 * @brief 同步 ConfigCenter 到旧的 Config 命名空间 (向后兼容)
 */
void sync_config_from_center();

} // namespace server
} // namespace trading
