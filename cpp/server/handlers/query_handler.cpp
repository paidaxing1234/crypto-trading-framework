/**
 * @file query_handler.cpp
 * @brief 查询处理模块实现
 */

#include "query_handler.h"
#include "../config/server_config.h"
#include "../managers/account_manager.h"
#include "../../adapters/okx/okx_rest_api.h"
#include "../../trading/strategy_config_loader.h"
#include <iostream>

namespace trading {
namespace server {

nlohmann::json handle_query(const nlohmann::json& request) {
    g_query_count++;

    std::string strategy_id = request.value("strategy_id", "unknown");
    std::string query_type = request.value("query_type", "");
    auto params = request.value("params", nlohmann::json::object());

    std::cout << "[查询] 策略: " << strategy_id << " | 类型: " << query_type << "\n";

    // 策略配置查询（风控端使用）
    if (query_type == "get_strategy_config") {
        std::string query_strategy_id = params.value("strategy_id", "");
        auto config = StrategyConfigManager::instance().get_config(query_strategy_id);
        if (config) {
            return {{"code", 0}, {"query_type", query_type}, {"data", config->to_json()}};
        } else {
            return {{"code", -1}, {"error", "策略配置未找到: " + query_strategy_id}};
        }
    }
    else if (query_type == "get_all_strategy_configs") {
        auto configs_json = StrategyConfigManager::instance().get_all_configs_json();
        return {{"code", 0}, {"query_type", query_type}, {"data", configs_json}};
    }
    else if (query_type == "get_strategy_contacts") {
        std::string query_strategy_id = params.value("strategy_id", "");
        auto contacts = StrategyConfigManager::instance().get_contacts(query_strategy_id);
        nlohmann::json contacts_json = nlohmann::json::array();
        for (const auto& contact : contacts) {
            contacts_json.push_back(contact.to_json());
        }
        return {{"code", 0}, {"query_type", query_type}, {"data", contacts_json}};
    }
    else if (query_type == "get_strategy_risk_control") {
        std::string query_strategy_id = params.value("strategy_id", "");
        auto risk_control = StrategyConfigManager::instance().get_risk_control(query_strategy_id);
        return {{"code", 0}, {"query_type", query_type}, {"data", risk_control.to_json()}};
    }

    // 获取该策略对应的 API 客户端
    okx::OKXRestAPI* api = get_api_for_strategy(strategy_id);
    if (!api) {
        return {{"code", -1}, {"error", "策略 " + strategy_id + " 未注册账户"}};
    }

    try {
        if (query_type == "account" || query_type == "balance") {
            std::string ccy = params.value("currency", "");
            auto result = api->get_account_balance(ccy);
            return {{"code", 0}, {"query_type", query_type}, {"data", result}};
        }
        else if (query_type == "positions") {
            std::string inst_type = params.value("inst_type", "SWAP");
            std::string symbol = params.value("symbol", "");
            auto result = api->get_positions(inst_type, symbol);
            return {{"code", 0}, {"query_type", query_type}, {"data", result}};
        }
        else if (query_type == "pending_orders" || query_type == "orders") {
            std::string inst_type = params.value("inst_type", "SPOT");
            std::string symbol = params.value("symbol", "");
            auto result = api->get_pending_orders(inst_type, symbol);
            return {{"code", 0}, {"query_type", query_type}, {"data", result}};
        }
        else if (query_type == "order") {
            std::string symbol = params.value("symbol", "");
            std::string order_id = params.value("order_id", "");
            std::string client_order_id = params.value("client_order_id", "");
            auto result = api->get_order(symbol, order_id, client_order_id);
            return {{"code", 0}, {"query_type", query_type}, {"data", result}};
        }
        else if (query_type == "instruments") {
            std::string inst_type = params.value("inst_type", "SPOT");
            auto result = api->get_account_instruments(inst_type);
            return {{"code", 0}, {"query_type", query_type}, {"data", result}};
        }
        else if (query_type == "registered_accounts") {
            return {{"code", 0}, {"query_type", query_type},
                    {"count", get_registered_strategy_count()}};
        }
        else {
            return {{"code", -1}, {"error", "未知查询类型: " + query_type}};
        }
    } catch (const std::exception& e) {
        return {{"code", -1}, {"error", std::string("查询异常: ") + e.what()}};
    }
}

} // namespace server
} // namespace trading
