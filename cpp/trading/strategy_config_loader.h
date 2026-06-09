#pragma once

/**
 * @file strategy_config_loader.h
 * @brief 策略配置加载器
 *
 * 功能：
 * - 从 strategies/configs/ 目录加载每个策略的配置文件
 * - 支持策略账户配置、参数配置、联系人信息
 * - 自动注册策略账户到 AccountRegistry
 * - 提供风控端查询接口
 *
 * @author Sequence Team
 * @date 2026-01
 */

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "account_registry.h"
#include "../core/logger.h"
#include "../adapters/binance/binance_rest_api.h"

namespace trading {

/**
 * @brief 联系人信息结构
 */
struct ContactInfo {
    std::string name;           // 联系人姓名
    std::string phone;          // 电话号码
    std::string email;          // 邮箱（可选）
    std::string wechat;         // 微信号（可选）
    std::string department;     // 部门（可选）
    std::string role;           // 角色（如：策略负责人、风控负责人）

    ContactInfo() = default;

    // 转换为JSON
    nlohmann::json to_json() const {
        nlohmann::json j;
        j["name"] = name;
        j["phone"] = phone;
        if (!email.empty()) j["email"] = email;
        if (!wechat.empty()) j["wechat"] = wechat;
        if (!department.empty()) j["department"] = department;
        if (!role.empty()) j["role"] = role;
        return j;
    }

    // 从JSON加载
    static ContactInfo from_json(const nlohmann::json& j) {
        ContactInfo info;
        info.name = j.value("name", "");
        info.phone = j.value("phone", "");
        info.email = j.value("email", "");
        info.wechat = j.value("wechat", "");
        info.department = j.value("department", "");
        info.role = j.value("role", "");
        return info;
    }
};

/**
 * @brief 风控配置结构
 */
struct RiskControlConfig {
    double max_position_value;      // 最大持仓价值（USD）
    double max_daily_loss;          // 最大日亏损（USD）
    double max_order_amount;        // 单笔最大下单金额（USD）
    int max_orders_per_minute;      // 每分钟最大下单次数
    bool enabled;                   // 是否启用风控

    RiskControlConfig()
        : max_position_value(0)
        , max_daily_loss(0)
        , max_order_amount(0)
        , max_orders_per_minute(0)
        , enabled(false) {}

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["max_position_value"] = max_position_value;
        j["max_daily_loss"] = max_daily_loss;
        j["max_order_amount"] = max_order_amount;
        j["max_orders_per_minute"] = max_orders_per_minute;
        j["enabled"] = enabled;
        return j;
    }

    static RiskControlConfig from_json(const nlohmann::json& j) {
        RiskControlConfig config;
        config.max_position_value = j.value("max_position_value", 0.0);
        config.max_daily_loss = j.value("max_daily_loss", 0.0);
        config.max_order_amount = j.value("max_order_amount", 0.0);
        config.max_orders_per_minute = j.value("max_orders_per_minute", 0);
        config.enabled = j.value("enabled", false);
        return config;
    }
};

/**
 * @brief 策略配置结构
 */
struct StrategyConfig {
    // 基本信息
    std::string strategy_id;           // 策略ID
    std::string account_id;            // 账户ID（同一API Key的策略共享）
    std::string strategy_name;         // 策略名称（可读）
    std::string strategy_type;         // 策略类型（grid/arbitrage/gnn等）
    bool enabled;                      // 是否启用

    // 交易所账户信息
    std::string exchange;              // 交易所 (okx/binance)
    std::string api_key;               // API Key
    std::string secret_key;            // Secret Key
    std::string passphrase;            // Passphrase (OKX专用)
    bool is_testnet;                   // 是否测试网
    std::string market;                // 市场类型 (futures/spot/coin_futures)

    // 联系人信息（支持多个联系人）
    std::vector<ContactInfo> contacts; // 联系人列表

    // 风控配置
    RiskControlConfig risk_control;    // 风控配置

    // 策略参数
    nlohmann::json params;             // 策略自定义参数

    // 元数据
    std::string description;           // 策略描述
    std::string created_at;            // 创建时间
    std::string updated_at;            // 更新时间

    StrategyConfig()
        : enabled(true)
        , is_testnet(true)
        , market("futures") {}

    // 转换为JSON（用于查询接口）
    nlohmann::json to_json() const {
        nlohmann::json j;
        j["strategy_id"] = strategy_id;
        j["account_id"] = account_id;
        j["strategy_name"] = strategy_name;
        j["strategy_type"] = strategy_type;
        j["enabled"] = enabled;
        j["exchange"] = exchange;
        j["is_testnet"] = is_testnet;
        j["market"] = market;

        // 联系人信息
        nlohmann::json contacts_json = nlohmann::json::array();
        for (const auto& contact : contacts) {
            contacts_json.push_back(contact.to_json());
        }
        j["contacts"] = contacts_json;

        // 风控配置
        j["risk_control"] = risk_control.to_json();

        // 策略参数
        j["params"] = params;

        // 元数据
        if (!description.empty()) j["description"] = description;
        if (!created_at.empty()) j["created_at"] = created_at;
        if (!updated_at.empty()) j["updated_at"] = updated_at;

        return j;
    }
};

/**
 * @brief 从JSON加载单个策略配置
 */
inline StrategyConfig load_strategy_config_from_json(const std::string& strategy_id,
                                                      const nlohmann::json& config) {
    StrategyConfig sc;

    // 基本信息
    sc.strategy_id = strategy_id;
    sc.account_id = config.value("account_id", "");
    sc.strategy_name = config.value("strategy_name", strategy_id);
    sc.strategy_type = config.value("strategy_type", "unknown");
    sc.enabled = config.value("enabled", true);

    // 交易所账户信息
    sc.exchange = config.value("exchange", "okx");
    sc.api_key = config.value("api_key", "");
    sc.secret_key = config.value("secret_key", "");
    sc.passphrase = config.value("passphrase", "");
    sc.is_testnet = config.value("is_testnet", true);
    sc.market = config.value("market", "futures");

    // 联系人信息
    if (config.contains("contacts") && config["contacts"].is_array()) {
        for (const auto& contact_json : config["contacts"]) {
            sc.contacts.push_back(ContactInfo::from_json(contact_json));
        }
    }

    // 风控配置
    if (config.contains("risk_control") && config["risk_control"].is_object()) {
        sc.risk_control = RiskControlConfig::from_json(config["risk_control"]);
    }

    // 策略参数
    if (config.contains("params") && config["params"].is_object()) {
        sc.params = config["params"];
    } else {
        sc.params = nlohmann::json::object();
    }

    // 元数据
    sc.description = config.value("description", "");
    sc.created_at = config.value("created_at", "");
    sc.updated_at = config.value("updated_at", "");

    return sc;
}

/**
 * @brief 从文件加载单个策略配置
 */
inline StrategyConfig load_strategy_config_from_file(const std::string& config_file) {
    std::ifstream file(config_file);
    if (!file.is_open()) {
        throw std::runtime_error("无法打开策略配置文件: " + config_file);
    }

    nlohmann::json config;
    try {
        file >> config;
    } catch (const std::exception& e) {
        throw std::runtime_error("解析策略配置文件失败: " + std::string(e.what()));
    }

    // 从文件名提取策略ID（去掉.json后缀）
    std::string strategy_id = std::filesystem::path(config_file).stem().string();

    // 如果配置中有 strategy_id，使用配置中的
    if (config.contains("strategy_id") && !config["strategy_id"].get<std::string>().empty()) {
        strategy_id = config["strategy_id"].get<std::string>();
    }

    return load_strategy_config_from_json(strategy_id, config);
}

/**
 * @brief 从目录加载所有策略配置
 *
 * @param strategies_dir 策略配置目录路径
 * @return 策略配置列表
 */
inline std::vector<StrategyConfig> load_all_strategy_configs(const std::string& strategies_dir) {
    std::vector<StrategyConfig> configs;

    if (!std::filesystem::exists(strategies_dir)) {
        std::cerr << "[策略配置] 目录不存在: " << strategies_dir << "\n";
        return configs;
    }

    if (!std::filesystem::is_directory(strategies_dir)) {
        std::cerr << "[策略配置] 不是目录: " << strategies_dir << "\n";
        return configs;
    }

    std::cout << "[策略配置] 扫描目录: " << strategies_dir << "\n";

    for (const auto& entry : std::filesystem::directory_iterator(strategies_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            try {
                auto config = load_strategy_config_from_file(entry.path().string());
                configs.push_back(config);
                std::cout << "[策略配置] 加载: " << config.strategy_id
                          << " (" << config.strategy_name << ") ✓\n";
            } catch (const std::exception& e) {
                std::cerr << "[策略配置] 加载失败: " << entry.path().filename()
                          << " - " << e.what() << "\n";
            }
        }
    }

    std::cout << "[策略配置] 共加载 " << configs.size() << " 个策略配置\n";
    return configs;
}

/**
 * @brief 注册策略账户到 AccountRegistry
 *
 * @param registry 账户注册器
 * @param config 策略配置
 * @return 是否注册成功
 */
inline bool register_strategy_from_config(AccountRegistry& registry, const StrategyConfig& config) {
    if (!config.enabled) {
        std::cout << "[策略注册] 跳过禁用的策略: " << config.strategy_id << "\n";
        return false;
    }

    if (config.api_key.empty() || config.secret_key.empty()) {
        std::cerr << "[策略注册] 策略 " << config.strategy_id << " 配置不完整（缺少API密钥）\n";
        return false;
    }

    ExchangeType ex_type = string_to_exchange_type(config.exchange);

    bool success = false;

    if (ex_type == ExchangeType::OKX) {
        success = registry.register_okx_account(
            config.strategy_id,
            config.api_key,
            config.secret_key,
            config.passphrase,
            config.is_testnet,
            config.account_id
        );
    } else if (ex_type == ExchangeType::BINANCE) {
        binance::MarketType market_type = binance::MarketType::FUTURES;
        std::string market_lower = config.market;
        std::transform(market_lower.begin(), market_lower.end(),
                      market_lower.begin(), ::tolower);

        if (market_lower == "spot") {
            market_type = binance::MarketType::SPOT;
        } else if (market_lower == "coin_futures" || market_lower == "coin-futures") {
            market_type = binance::MarketType::COIN_FUTURES;
        }

        success = registry.register_binance_account(
            config.strategy_id,
            config.api_key,
            config.secret_key,
            config.is_testnet,
            market_type,
            config.account_id
        );
    } else {
        std::cerr << "[策略注册] 不支持的交易所: " << config.exchange << "\n";
        return false;
    }

    if (success) {
        std::cout << "[策略注册] ✓ " << config.strategy_id
                  << " (" << config.strategy_name << ")"
                  << " | " << config.exchange
                  << " | " << (config.is_testnet ? "测试网" : "实盘")
                  << " | API Key: " << config.api_key.substr(0, 8) << "...";

        // 显示联系人信息
        if (!config.contacts.empty()) {
            std::cout << " | 联系人: " << config.contacts[0].name
                      << " (" << config.contacts[0].phone << ")";
        }
        std::cout << "\n";

        // 记录审计日志
        using namespace trading::core;
        std::stringstream audit_msg;
        audit_msg << "策略: " << config.strategy_id
                  << " | 交易所: " << config.exchange
                  << " | 模式: " << (config.is_testnet ? "测试网" : "实盘");
        if (!config.contacts.empty()) {
            audit_msg << " | 联系人: " << config.contacts[0].name
                      << " (" << config.contacts[0].phone << ")";
        }
        LOG_AUDIT("策略账户注册", audit_msg.str());
    } else {
        std::cerr << "[策略注册] ✗ " << config.strategy_id << " 注册失败\n";
    }

    return success;
}

/**
 * @brief 批量注册所有策略账户
 *
 * @param registry 账户注册器
 * @param configs 策略配置列表
 * @return 成功注册的数量
 */
inline size_t register_all_strategies(AccountRegistry& registry,
                                      const std::vector<StrategyConfig>& configs) {
    size_t success_count = 0;

    for (const auto& config : configs) {
        if (register_strategy_from_config(registry, config)) {
            success_count++;
        }
    }

    std::cout << "[策略注册] 成功注册 " << success_count << "/" << configs.size() << " 个策略\n";
    return success_count;
}

/**
 * @brief 从目录加载并注册所有策略
 *
 * @param registry 账户注册器
 * @param strategies_dir 策略配置目录
 * @return 成功注册的数量
 */
inline size_t load_and_register_strategies(AccountRegistry& registry,
                                           const std::string& strategies_dir) {
    std::cout << "\n========================================\n";
    std::cout << "  加载策略配置\n";
    std::cout << "========================================\n";

    auto configs = load_all_strategy_configs(strategies_dir);

    if (configs.empty()) {
        std::cout << "[策略配置] 未找到任何策略配置文件\n";
        std::cout << "[策略配置] 请在 " << strategies_dir << " 目录下创建策略配置文件\n";
        return 0;
    }

    size_t count = register_all_strategies(registry, configs);

    std::cout << "========================================\n\n";
    return count;
}

/**
 * @brief 策略配置管理器（全局单例）
 */
class StrategyConfigManager {
public:
    static StrategyConfigManager& instance() {
        static StrategyConfigManager instance;
        return instance;
    }

    // 加载所有配置
    void load_configs(const std::string& config_dir) {
        configs_ = load_all_strategy_configs(config_dir);

        // 建立索引
        config_map_.clear();
        for (const auto& config : configs_) {
            config_map_[config.strategy_id] = config;
        }
    }

    // 获取策略配置
    const StrategyConfig* get_config(const std::string& strategy_id) const {
        auto it = config_map_.find(strategy_id);
        if (it != config_map_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // 获取所有配置
    const std::vector<StrategyConfig>& get_all_configs() const {
        return configs_;
    }

    // 获取所有配置的JSON（用于查询接口）
    nlohmann::json get_all_configs_json() const {
        nlohmann::json result = nlohmann::json::array();
        for (const auto& config : configs_) {
            result.push_back(config.to_json());
        }
        return result;
    }

    // 根据策略ID获取联系人信息
    std::vector<ContactInfo> get_contacts(const std::string& strategy_id) const {
        auto config = get_config(strategy_id);
        if (config) {
            return config->contacts;
        }
        return {};
    }

    // 根据策略ID获取风控配置
    RiskControlConfig get_risk_control(const std::string& strategy_id) const {
        auto config = get_config(strategy_id);
        if (config) {
            return config->risk_control;
        }
        return RiskControlConfig();
    }

private:
    StrategyConfigManager() = default;

    std::vector<StrategyConfig> configs_;
    std::map<std::string, StrategyConfig> config_map_;
};

} // namespace trading
