/**
 * @file server_config.cpp
 * @brief 交易服务器全局配置和状态实现
 *
 * 注意: 推荐使用 config_center.h 中的 ConfigCenter 进行配置管理
 * 本文件保留向后兼容，新代码请使用 ConfigCenter
 */

#include "server_config.h"
#include "../../adapters/okx/okx_websocket.h"
#include "../../adapters/binance/binance_websocket.h"
#include "../../adapters/binance/binance_rest_api.h"
#include "../../trading/account_registry.h"
#include "../../network/websocket_server.h"
#include "../../network/auth_manager.h"
#include <cstdlib>
#include <iostream>

namespace trading {
namespace server {

// ============================================================
// 全局配置
// ============================================================

namespace Config {
    // OKX 配置
    std::string api_key;
    std::string secret_key;
    std::string passphrase;
    bool is_testnet = false;  // 默认主网
    std::vector<std::string> default_symbols = {};  // 空则动态获取全市场
    std::vector<std::string> spot_symbols = {};     // 空则动态获取全市场
    std::vector<std::string> swap_symbols = {};     // 空则动态获取全市场

    // Binance 配置
    std::string binance_api_key;
    std::string binance_secret_key;
    bool binance_is_testnet = false;  // 使用主网获取行情数据
    std::vector<std::string> binance_symbols = {};  // 空则动态获取全部永续合约
}

// ============================================================
// 全局状态
// ============================================================

std::atomic<bool> g_running{true};

// 统计
std::atomic<uint64_t> g_trade_count{0};
std::atomic<uint64_t> g_kline_count{0};
std::atomic<uint64_t> g_orderbook_count{0};
std::atomic<uint64_t> g_funding_rate_count{0};
std::atomic<uint64_t> g_order_count{0};
std::atomic<uint64_t> g_order_success{0};
std::atomic<uint64_t> g_order_failed{0};
std::atomic<uint64_t> g_query_count{0};

// 分交易所统计
// OKX: Ticker + Trades + K线
std::atomic<uint64_t> g_okx_ticker_count{0};
std::atomic<uint64_t> g_okx_trade_count{0};
std::atomic<uint64_t> g_okx_kline_count{0};

// Binance: Ticker + MarkPrice + K线
std::atomic<uint64_t> g_binance_ticker_count{0};
std::atomic<uint64_t> g_binance_markprice_count{0};
std::atomic<uint64_t> g_binance_kline_count{0};

// 订阅管理
std::mutex g_sub_mutex;
std::set<std::string> g_subscribed_trades;
std::map<std::string, std::set<std::string>> g_subscribed_klines;
std::map<std::string, std::set<std::string>> g_subscribed_orderbooks;
std::set<std::string> g_subscribed_funding_rates;

// WebSocket 客户端指针 - OKX
std::unique_ptr<okx::OKXWebSocket> g_ws_public;
std::unique_ptr<okx::OKXWebSocket> g_ws_business;
std::unique_ptr<okx::OKXWebSocket> g_ws_private;

// WebSocket 客户端指针 - Binance
std::unique_ptr<binance::BinanceWebSocket> g_binance_ws_market;
std::unique_ptr<binance::BinanceWebSocket> g_binance_ws_depth;   // 深度数据专用连接
std::unique_ptr<binance::BinanceWebSocket> g_binance_ws_user;
std::vector<std::unique_ptr<binance::BinanceWebSocket>> g_binance_ws_klines;  // K线专用连接组
std::unique_ptr<binance::BinanceRestAPI> g_binance_rest_api;


// 前端 WebSocket 服务器
std::unique_ptr<core::WebSocketServer> g_frontend_server;

// 认证管理器
auth::AuthManager g_auth_manager;
std::map<int, auth::TokenInfo> g_authenticated_clients;
std::mutex g_auth_mutex;

// 账户注册管理器 - 使用 trading::g_account_registry (定义在 account_registry.h)

// ============================================================
// 工具函数
// ============================================================

void load_config() {
    // OKX 配置
    Config::api_key = std::getenv("OKX_API_KEY")
        ? std::getenv("OKX_API_KEY")
        : "YOUR_OKX_API_KEY";

    Config::secret_key = std::getenv("OKX_SECRET_KEY")
        ? std::getenv("OKX_SECRET_KEY")
        : "YOUR_OKX_SECRET_KEY";

    Config::passphrase = std::getenv("OKX_PASSPHRASE")
        ? std::getenv("OKX_PASSPHRASE")
        : "YOUR_OKX_PASSPHRASE";

    const char* testnet_env = std::getenv("OKX_TESTNET");
    Config::is_testnet = testnet_env ? (std::string(testnet_env) == "1") : false;  // 默认主网

    // Binance 配置
    Config::binance_api_key = std::getenv("BINANCE_API_KEY")
        ? std::getenv("BINANCE_API_KEY")
        : "";

    Config::binance_secret_key = std::getenv("BINANCE_SECRET_KEY")
        ? std::getenv("BINANCE_SECRET_KEY")
        : "";

    const char* binance_testnet_env = std::getenv("BINANCE_TESTNET");
    Config::binance_is_testnet = binance_testnet_env ? (std::string(binance_testnet_env) == "1") : false;  // 默认主网
}

// ============================================================
// 配置中心集成
// ============================================================

bool init_config_center(const std::string& config_file) {
    std::cout << "[Config] 初始化配置中心: " << config_file << std::endl;

    // 初始化配置中心
    bool success = config::ConfigCenter::instance().init(config_file, true);

    if (success) {
        // 同步到旧的 Config 命名空间（向后兼容）
        sync_config_from_center();

        // 注册配置变更监听器，自动同步
        config::ConfigCenter::instance().on_change("", [](const std::string& key,
                                                          const nlohmann::json& /*old_val*/,
                                                          const nlohmann::json& /*new_val*/) {
            std::cout << "[Config] 配置变更: " << key << std::endl;
            sync_config_from_center();
        });

        std::cout << "[Config] ✓ 配置中心初始化完成" << std::endl;
    } else {
        std::cerr << "[Config] ✗ 配置中心初始化失败，使用环境变量" << std::endl;
        load_config();  // 回退到旧方式
    }

    return success;
}

bool reload_config() {
    std::cout << "[Config] 热重载配置..." << std::endl;

    bool success = config::ConfigCenter::instance().reload();

    if (success) {
        sync_config_from_center();
        std::cout << "[Config] ✓ 配置热重载完成" << std::endl;
    } else {
        std::cerr << "[Config] ✗ 配置热重载失败" << std::endl;
    }

    return success;
}

void sync_config_from_center() {
    auto& center = config::ConfigCenter::instance();

    // 同步 OKX 配置
    Config::api_key = center.okx().api_key;
    Config::secret_key = center.okx().secret_key;
    Config::passphrase = center.okx().passphrase;
    Config::is_testnet = center.okx().is_testnet;
    Config::spot_symbols = center.okx().spot_symbols;
    Config::swap_symbols = center.okx().swap_symbols;

    // 同步 Binance 配置
    Config::binance_api_key = center.binance().api_key;
    Config::binance_secret_key = center.binance().secret_key;
    Config::binance_is_testnet = center.binance().is_testnet;
    Config::binance_symbols = center.binance().futures_symbols;

    std::cout << "[Config] ✓ 配置已同步到 Config 命名空间" << std::endl;
    std::cout << "[Config]   OKX: " << (Config::is_testnet ? "测试网" : "主网")
              << " | API Key: " << (Config::api_key.empty() ? "(空)" : Config::api_key.substr(0, 8) + "...")
              << std::endl;
    std::cout << "[Config]   Binance: " << (Config::binance_is_testnet ? "测试网" : "主网")
              << " | API Key: " << (Config::binance_api_key.empty() ? "(空)" : Config::binance_api_key.substr(0, 8) + "...")
              << std::endl;
}

} // namespace server
} // namespace trading
