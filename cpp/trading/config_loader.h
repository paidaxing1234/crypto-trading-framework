#pragma once

/**
 * @file config_loader.h
 * @brief 配置文件加载器
 *
 * 功能：
 * - 加载JSON配置文件
 * - 加载账户配置
 * - 环境变量覆盖
 *
 * @author Sequence Team
 * @date 2026-01
 */

#include <string>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "account_registry.h"

namespace trading {

/**
 * @brief 加载JSON文件
 */
inline nlohmann::json load_json_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("无法打开配置文件: " + file_path);
    }

    nlohmann::json config;
    try {
        file >> config;
    } catch (const std::exception& e) {
        throw std::runtime_error("解析配置文件失败: " + std::string(e.what()));
    }

    return config;
}

/**
 * @brief 从环境变量获取值（如果存在）
 */
inline std::string get_env_or_default(const char* env_name, const std::string& default_value) {
    const char* env_value = std::getenv(env_name);
    return env_value ? std::string(env_value) : default_value;
}

/**
 * @brief 加载账户配置到注册器
 *
 * @param registry 账户注册器
 * @param config_file 配置文件路径
 * @param use_env_override 是否使用环境变量覆盖
 * @param enable_auto_save 是否启用自动保存（默认启用）
 */
inline void load_accounts_from_config(AccountRegistry& registry,
                                     const std::string& config_file,
                                     bool use_env_override = true,
                                     bool enable_auto_save = true) {
    std::cout << "[配置] 加载账户配置: " << config_file << "\n";

    // 设置配置文件路径（启用自动持久化）
    if (enable_auto_save) {
        registry.set_config_path(config_file);
    }

    nlohmann::json config;
    try {
        config = load_json_file(config_file);
    } catch (const std::exception& e) {
        std::cerr << "[配置] 加载失败: " << e.what() << "\n";
        std::cerr << "[配置] 将使用环境变量或默认值\n";
        config = nlohmann::json::object();
    }

    // ==================== 加载默认账户 ====================

    if (config.contains("default") && config["default"].is_object()) {
        auto& def = config["default"];

        std::string exchange = def.value("exchange", "okx");
        std::string api_key = def.value("api_key", "");
        std::string secret_key = def.value("secret_key", "");
        std::string passphrase = def.value("passphrase", "");
        bool is_testnet = def.value("is_testnet", true);

        // 环境变量覆盖
        if (use_env_override) {
            api_key = get_env_or_default("OKX_API_KEY", api_key);
            secret_key = get_env_or_default("OKX_SECRET_KEY", secret_key);
            passphrase = get_env_or_default("OKX_PASSPHRASE", passphrase);

            const char* testnet_env = std::getenv("OKX_TESTNET");
            if (testnet_env) {
                is_testnet = (std::string(testnet_env) == "1" || std::string(testnet_env) == "true");
            }
        }

        if (!api_key.empty() && !secret_key.empty()) {
            ExchangeType ex_type = string_to_exchange_type(exchange);

            if (ex_type == ExchangeType::OKX) {
                registry.set_default_okx_account(api_key, secret_key, passphrase, is_testnet);
                std::cout << "[配置] 默认OKX账户 ✓ (API Key: " << api_key.substr(0, 8) << "...)\n";
            } else if (ex_type == ExchangeType::BINANCE) {
                registry.set_default_binance_account(api_key, secret_key, is_testnet);
                std::cout << "[配置] 默认Binance账户 ✓ (API Key: " << api_key.substr(0, 8) << "...)\n";
            }

            std::cout << "[配置] 模式: " << (is_testnet ? "模拟盘" : "实盘") << "\n";
        } else {
            std::cerr << "[配置] 警告: 默认账户配置不完整\n";
        }
    }

    // ==================== 加载策略账户 ====================

    if (config.contains("strategies") && config["strategies"].is_object()) {
        int loaded_count = 0;

        for (auto& [strategy_id, acc] : config["strategies"].items()) {
            if (!acc.is_object()) continue;

            std::string exchange = acc.value("exchange", "okx");
            std::string api_key = acc.value("api_key", "");
            std::string secret_key = acc.value("secret_key", "");
            std::string passphrase = acc.value("passphrase", "");
            bool is_testnet = acc.value("is_testnet", true);

            if (api_key.empty() || secret_key.empty()) {
                std::cerr << "[配置] 跳过策略 " << strategy_id << ": 配置不完整\n";
                continue;
            }

            ExchangeType ex_type = string_to_exchange_type(exchange);

            bool success = registry.register_account(
                strategy_id, ex_type, api_key, secret_key, passphrase, is_testnet
            );

            if (success) {
                loaded_count++;
                std::cout << "[配置] 策略 " << strategy_id << " ✓ | "
                          << exchange_type_to_string(ex_type) << " | "
                          << (is_testnet ? "模拟盘" : "实盘") << " | "
                          << "API Key: " << api_key.substr(0, 8) << "...\n";
            } else {
                std::cerr << "[配置] 策略 " << strategy_id << " ✗ 注册失败\n";
            }
        }

        std::cout << "[配置] 已加载 " << loaded_count << " 个策略账户\n";
    }

    std::cout << "[配置] 总计: OKX=" << registry.okx_count()
              << ", Binance=" << registry.binance_count() << "\n";
}

} // namespace trading
