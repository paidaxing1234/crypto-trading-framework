/**
 * @file account_manager.cpp
 * @brief 账户注册管理实现
 */

#include "account_manager.h"
#include "../config/server_config.h"
#include "../../trading/account_registry.h"
#include <iostream>

namespace trading {
namespace server {

okx::OKXRestAPI* get_api_for_strategy(const std::string& strategy_id) {
    okx::OKXRestAPI* api = g_account_registry.get_okx_api(strategy_id);
    if (!api) {
        std::cout << "[账户] 策略 " << strategy_id << " 未注册账户，且无默认账户\n";
    }
    return api;
}

okx::OKXRestAPI* get_okx_api_for_strategy(const std::string& strategy_id) {
    return get_api_for_strategy(strategy_id);
}

binance::BinanceRestAPI* get_binance_api_for_strategy(const std::string& strategy_id) {
    binance::BinanceRestAPI* api = g_account_registry.get_binance_api(strategy_id);
    if (!api) {
        std::cout << "[账户] 策略 " << strategy_id << " 未注册Binance账户\n";
    }
    return api;
}

bool register_strategy_account(const std::string& strategy_id,
                               const std::string& api_key,
                               const std::string& secret_key,
                               const std::string& passphrase,
                               bool is_testnet,
                               const std::string& account_id) {
    bool success = g_account_registry.register_okx_account(
        strategy_id, api_key, secret_key, passphrase, is_testnet, account_id
    );

    if (success) {
        std::cout << "[账户] ✓ 策略 " << strategy_id << " 已注册 ("
                  << (is_testnet ? "模拟盘" : "实盘") << ")\n";
    } else {
        std::cout << "[账户] ✗ 策略 " << strategy_id << " 注册失败\n";
    }

    return success;
}

bool unregister_strategy_account(const std::string& strategy_id) {
    // TODO: 实现 unregister_okx_account 方法
    // bool success = g_account_registry.unregister_okx_account(strategy_id);
    bool success = false;  // 暂时返回 false

    if (success) {
        std::cout << "[账户] ✓ 策略 " << strategy_id << " 已注销\n";
    } else {
        std::cout << "[账户] 策略 " << strategy_id << " 未找到（功能暂未实现）\n";
    }

    return success;
}

size_t get_registered_strategy_count() {
    return g_account_registry.count();
}

} // namespace server
} // namespace trading
