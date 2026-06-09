/**
 * @file account_manager.h
 * @brief 账户注册管理
 */

#pragma once

#include <string>
#include "../../adapters/okx/okx_rest_api.h"
#include "../../adapters/binance/binance_rest_api.h"

namespace trading {
namespace server {

/**
 * @brief 获取策略对应的 API 客户端
 */
okx::OKXRestAPI* get_api_for_strategy(const std::string& strategy_id);

/**
 * @brief 获取OKX策略对应的 API 客户端
 */
okx::OKXRestAPI* get_okx_api_for_strategy(const std::string& strategy_id);

/**
 * @brief 获取Binance策略对应的 API 客户端
 */
binance::BinanceRestAPI* get_binance_api_for_strategy(const std::string& strategy_id);

/**
 * @brief 注册策略账户
 */
bool register_strategy_account(const std::string& strategy_id,
                               const std::string& api_key,
                               const std::string& secret_key,
                               const std::string& passphrase,
                               bool is_testnet,
                               const std::string& account_id = "");
/**
 * @brief 注销策略账户
 */
bool unregister_strategy_account(const std::string& strategy_id);

/**
 * @brief 获取已注册的策略数量
 */
size_t get_registered_strategy_count();

} // namespace server
} // namespace trading
