#pragma once

/**
 * @file account_registry.h
 * @brief 账户注册管理器 - 多账户/多策略/多市场支持
 *
 * 功能：
 * - 策略账户注册/注销/更新
 * - 默认账户管理
 * - 多市场支持（Binance SPOT/FUTURES/COIN_FUTURES）
 * - 线程安全的账户查询
 * - 账户健康检查
 * - 配置持久化
 *
 * @author Sequence Team
 * @date 2026-01
 */

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>
#include <functional>
#include <iostream>
#include <fstream>
#include <set>
#include <nlohmann/json.hpp>
#include "../adapters/okx/okx_rest_api.h"
#include "../adapters/binance/binance_rest_api.h"

namespace trading {

// ==================== 交易所类型 ====================

enum class ExchangeType {
    OKX,
    BINANCE
};

inline std::string exchange_type_to_string(ExchangeType type) {
    switch (type) {
        case ExchangeType::OKX: return "OKX";
        case ExchangeType::BINANCE: return "Binance";
        default: return "Unknown";
    }
}

inline ExchangeType string_to_exchange_type(const std::string& str) {
    if (str == "okx" || str == "OKX") return ExchangeType::OKX;
    if (str == "binance" || str == "BINANCE" || str == "Binance") return ExchangeType::BINANCE;
    return ExchangeType::OKX;  // 默认
}

// ==================== 账户状态 ====================

enum class AccountStatus {
    ACTIVE,      // 正常
    DISABLED,    // 禁用
    ERROR,       // 错误（API无效等）
    RATE_LIMITED // 被限流
};

inline std::string account_status_to_string(AccountStatus status) {
    switch (status) {
        case AccountStatus::ACTIVE: return "active";
        case AccountStatus::DISABLED: return "disabled";
        case AccountStatus::ERROR: return "error";
        case AccountStatus::RATE_LIMITED: return "rate_limited";
        default: return "unknown";
    }
}

// ==================== 账户信息 ====================

/**
 * @brief 账户信息基类
 */
struct AccountInfoBase {
    std::string strategy_id;
    std::string account_id;
    std::string api_key;
    std::string secret_key;
    std::string passphrase;  // OKX需要，Binance不需要
    bool is_testnet;
    ExchangeType exchange_type;
    AccountStatus status;
    int64_t register_time;
    int64_t last_health_check;
    std::string last_error;

    // 监控数据（由 account_monitor 定期更新）
    std::atomic<double> equity{0.0};
    std::atomic<double> unrealized_pnl{0.0};
    std::atomic<int64_t> monitor_update_time{0};

    // 持仓数据（由 account_monitor 定期更新）
    mutable std::mutex positions_mutex;
    nlohmann::json positions = nlohmann::json::array();

    void update_positions(const nlohmann::json& pos) {
        std::lock_guard<std::mutex> lk(positions_mutex);
        positions = pos;
    }

    nlohmann::json get_positions() const {
        std::lock_guard<std::mutex> lk(positions_mutex);
        return positions;
    }

    AccountInfoBase()
        : is_testnet(true)
        , exchange_type(ExchangeType::OKX)
        , status(AccountStatus::ACTIVE)
        , register_time(0)
        , last_health_check(0) {}

    virtual ~AccountInfoBase() = default;

    // 转换为JSON
    virtual nlohmann::json to_json() const {
        return {
            {"strategy_id", strategy_id},
            {"account_id", account_id},
            {"api_key", api_key.substr(0, 8) + "..."},  // 只显示前8位
            {"is_testnet", is_testnet},
            {"exchange", exchange_type_to_string(exchange_type)},
            {"status", account_status_to_string(status)},
            {"register_time", register_time},
            {"equity", equity.load()},
            {"unrealizedPnl", unrealized_pnl.load()},
            {"monitor_update_time", monitor_update_time.load()}
        };
    }
};

/**
 * @brief OKX账户信息
 */
struct OKXAccountInfo : public AccountInfoBase {
    std::unique_ptr<okx::OKXRestAPI> api;

    OKXAccountInfo(const std::string& id, const std::string& key, const std::string& secret,
                   const std::string& pass, bool testnet, const std::string& acct_id = "") {
        strategy_id = id;
        account_id = acct_id;
        api_key = key;
        secret_key = secret;
        passphrase = pass;
        is_testnet = testnet;
        exchange_type = ExchangeType::OKX;
        status = AccountStatus::ACTIVE;
        register_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        api = std::make_unique<okx::OKXRestAPI>(api_key, secret_key, passphrase, is_testnet);
    }

    // 更新账户配置
    void update(const std::string& key, const std::string& secret,
                const std::string& pass, bool testnet) {
        api_key = key;
        secret_key = secret;
        passphrase = pass;
        is_testnet = testnet;
        api = std::make_unique<okx::OKXRestAPI>(api_key, secret_key, passphrase, is_testnet);
        status = AccountStatus::ACTIVE;
        last_error.clear();
    }
};

/**
 * @brief Binance账户信息（支持多市场）
 */
struct BinanceAccountInfo : public AccountInfoBase {
    // 支持同时访问多个市场
    std::unique_ptr<binance::BinanceRestAPI> spot_api;
    std::unique_ptr<binance::BinanceRestAPI> futures_api;
    std::unique_ptr<binance::BinanceRestAPI> coin_futures_api;

    // 默认市场类型
    binance::MarketType default_market;

    BinanceAccountInfo(const std::string& id, const std::string& key, const std::string& secret,
                       bool testnet, binance::MarketType market = binance::MarketType::FUTURES,
                       const std::string& acct_id = "") {
        strategy_id = id;
        account_id = acct_id;
        api_key = key;
        secret_key = secret;
        is_testnet = testnet;
        exchange_type = ExchangeType::BINANCE;
        status = AccountStatus::ACTIVE;
        default_market = market;
        register_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // 根据默认市场创建API
        create_api(market);
    }

    // 创建指定市场的API
    void create_api(binance::MarketType market) {
        switch (market) {
            case binance::MarketType::SPOT:
                if (!spot_api) {
                    spot_api = std::make_unique<binance::BinanceRestAPI>(
                        api_key, secret_key, binance::MarketType::SPOT, is_testnet
                    );
                }
                break;
            case binance::MarketType::FUTURES:
                if (!futures_api) {
                    futures_api = std::make_unique<binance::BinanceRestAPI>(
                        api_key, secret_key, binance::MarketType::FUTURES, is_testnet
                    );
                }
                break;
            case binance::MarketType::COIN_FUTURES:
                if (!coin_futures_api) {
                    coin_futures_api = std::make_unique<binance::BinanceRestAPI>(
                        api_key, secret_key, binance::MarketType::COIN_FUTURES, is_testnet
                    );
                }
                break;
        }
    }

    // 获取指定市场的API
    binance::BinanceRestAPI* get_api(binance::MarketType market) {
        create_api(market);  // 确保已创建
        switch (market) {
            case binance::MarketType::SPOT:
                return spot_api.get();
            case binance::MarketType::FUTURES:
                return futures_api.get();
            case binance::MarketType::COIN_FUTURES:
                return coin_futures_api.get();
            default:
                return nullptr;
        }
    }

    // 获取默认市场的API
    binance::BinanceRestAPI* get_default_api() {
        return get_api(default_market);
    }

    // 更新账户配置
    void update(const std::string& key, const std::string& secret, bool testnet,
                binance::MarketType market = binance::MarketType::FUTURES) {
        api_key = key;
        secret_key = secret;
        is_testnet = testnet;
        default_market = market;

        // 清除旧的API实例
        spot_api.reset();
        futures_api.reset();
        coin_futures_api.reset();

        // 创建新的API
        create_api(market);
        status = AccountStatus::ACTIVE;
        last_error.clear();
    }

    nlohmann::json to_json() const override {
        auto json = AccountInfoBase::to_json();
        json["default_market"] = static_cast<int>(default_market);
        json["has_spot"] = (spot_api != nullptr);
        json["has_futures"] = (futures_api != nullptr);
        json["has_coin_futures"] = (coin_futures_api != nullptr);
        return json;
    }
};

// ==================== 账户注册管理器 ====================

/**
 * @brief 账户注册管理器
 *
 * 线程安全的多账户管理，支持：
 * - 策略独立账户
 * - 默认账户（未注册策略使用）
 * - 多市场支持（Binance）
 * - 账户更新
 * - 健康检查
 * - 配置持久化
 */
class AccountRegistry {
public:
    AccountRegistry() = default;
    ~AccountRegistry() = default;

    // 禁止拷贝
    AccountRegistry(const AccountRegistry&) = delete;
    AccountRegistry& operator=(const AccountRegistry&) = delete;

    // ==================== OKX账户管理 ====================

    /**
     * @brief 注册OKX策略账户
     */
    bool register_okx_account(const std::string& strategy_id,
                              const std::string& api_key,
                              const std::string& secret_key,
                              const std::string& passphrase,
                              bool is_testnet,
                              const std::string& account_id = "") {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto account = std::make_shared<OKXAccountInfo>(
                strategy_id, api_key, secret_key, passphrase, is_testnet, account_id
            );
            okx_accounts_[strategy_id] = account;
        }

        auto_save();  // 自动保存
        return true;
    }

    /**
     * @brief 更新OKX策略账户
     */
    bool update_okx_account(const std::string& strategy_id,
                            const std::string& api_key,
                            const std::string& secret_key,
                            const std::string& passphrase,
                            bool is_testnet,
                            const std::string& account_id = "") {
        bool result = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = okx_accounts_.find(strategy_id);
            if (it != okx_accounts_.end() && it->second) {
                it->second->update(api_key, secret_key, passphrase, is_testnet);
                if (!account_id.empty()) it->second->account_id = account_id;
                result = true;
            }
        }

        if (result) {
            auto_save();  // 自动保存
            return true;
        }

        // 不存在则创建
        return register_okx_account(strategy_id, api_key, secret_key, passphrase, is_testnet, account_id);
    }

    /**
     * @brief 设置OKX默认账户
     */
    void set_default_okx_account(const std::string& api_key,
                                 const std::string& secret_key,
                                 const std::string& passphrase,
                                 bool is_testnet,
                                 const std::string& account_id = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        default_okx_account_ = std::make_shared<OKXAccountInfo>(
            "_default_", api_key, secret_key, passphrase, is_testnet, account_id
        );
    }

    /**
     * @brief 获取OKX策略账户API
     */
    okx::OKXRestAPI* get_okx_api(const std::string& strategy_id) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 查找策略账户
        auto it = okx_accounts_.find(strategy_id);
        if (it != okx_accounts_.end() && it->second && it->second->api) {
            if (it->second->status == AccountStatus::ACTIVE) {
                return it->second->api.get();
            }
        }

        // 使用默认账户
        if (default_okx_account_ && default_okx_account_->api) {
            return default_okx_account_->api.get();
        }

        return nullptr;
    }

    // ==================== Binance账户管理 ====================

    /**
     * @brief 注册Binance策略账户
     */
    bool register_binance_account(const std::string& strategy_id,
                                  const std::string& api_key,
                                  const std::string& secret_key,
                                  bool is_testnet,
                                  binance::MarketType market = binance::MarketType::FUTURES,
                                  const std::string& account_id = "") {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto account = std::make_shared<BinanceAccountInfo>(
                strategy_id, api_key, secret_key, is_testnet, market, account_id
            );
            binance_accounts_[strategy_id] = account;
        }

        auto_save();  // 自动保存
        return true;
    }

    /**
     * @brief 更新Binance策略账户
     */
    bool update_binance_account(const std::string& strategy_id,
                                const std::string& api_key,
                                const std::string& secret_key,
                                bool is_testnet,
                                binance::MarketType market = binance::MarketType::FUTURES,
                                const std::string& account_id = "") {
        bool result = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = binance_accounts_.find(strategy_id);
            if (it != binance_accounts_.end() && it->second) {
                it->second->update(api_key, secret_key, is_testnet, market);
                if (!account_id.empty()) it->second->account_id = account_id;
                result = true;
            }
        }

        if (result) {
            auto_save();  // 自动保存
            return true;
        }

        // 不存在则创建
        return register_binance_account(strategy_id, api_key, secret_key, is_testnet, market, account_id);
    }

    /**
     * @brief 设置Binance默认账户
     */
    void set_default_binance_account(const std::string& api_key,
                                     const std::string& secret_key,
                                     bool is_testnet,
                                     binance::MarketType market = binance::MarketType::FUTURES,
                                     const std::string& account_id = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        default_binance_account_ = std::make_shared<BinanceAccountInfo>(
            "_default_", api_key, secret_key, is_testnet, market, account_id
        );
    }

    /**
     * @brief 获取Binance策略账户API（指定市场）
     */
    binance::BinanceRestAPI* get_binance_api(const std::string& strategy_id,
                                              binance::MarketType market) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 查找策略账户
        auto it = binance_accounts_.find(strategy_id);
        if (it != binance_accounts_.end() && it->second) {
            if (it->second->status == AccountStatus::ACTIVE) {
                return it->second->get_api(market);
            }
        }

        // 使用默认账户
        if (default_binance_account_) {
            return default_binance_account_->get_api(market);
        }

        return nullptr;
    }

    /**
     * @brief 获取Binance策略账户API（使用默认市场）
     */
    binance::BinanceRestAPI* get_binance_api(const std::string& strategy_id) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 查找策略账户
        auto it = binance_accounts_.find(strategy_id);
        if (it != binance_accounts_.end() && it->second) {
            if (it->second->status == AccountStatus::ACTIVE) {
                return it->second->get_default_api();
            }
        }

        // 使用默认账户
        if (default_binance_account_) {
            return default_binance_account_->get_default_api();
        }

        return nullptr;
    }

    /**
     * @brief 为账户启用额外市场
     */
    bool enable_binance_market(const std::string& strategy_id, binance::MarketType market) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = binance_accounts_.find(strategy_id);
        if (it != binance_accounts_.end() && it->second) {
            it->second->create_api(market);
            return true;
        }

        return false;
    }

    // ==================== 通用接口 ====================

    /**
     * @brief 注册账户（自动识别交易所）
     */
    bool register_account(const std::string& strategy_id,
                         ExchangeType exchange,
                         const std::string& api_key,
                         const std::string& secret_key,
                         const std::string& passphrase,
                         bool is_testnet,
                         const std::string& account_id = "") {
        if (exchange == ExchangeType::OKX) {
            return register_okx_account(strategy_id, api_key, secret_key, passphrase, is_testnet, account_id);
        } else if (exchange == ExchangeType::BINANCE) {
            return register_binance_account(strategy_id, api_key, secret_key, is_testnet, binance::MarketType::FUTURES, account_id);
        }
        return false;
    }

    /**
     * @brief 注销账户
     */
    bool unregister_account(const std::string& strategy_id, ExchangeType exchange) {
        bool result = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 同一账户可能挂在多个键下: account_id 主键 + 若干 strategy_id 别名(add_account_alias
            // 只是把同一对象指针复制到别的键)。若只 erase(strategy_id) 这一个键, 残留的别名仍带着
            // 相同 account_id, get_all_accounts_info 会再次枚举到它 -> 前端注销后账户仍显示。
            // 这里删除【所有指向同一账户(同指针或同 account_id)的键】。
            if (exchange == ExchangeType::OKX) {
                auto it = okx_accounts_.find(strategy_id);
                if (it != okx_accounts_.end()) {
                    auto target = it->second;
                    std::string acct_id = target ? target->account_id : std::string();
                    for (auto i = okx_accounts_.begin(); i != okx_accounts_.end(); ) {
                        if (i->second == target ||
                            (!acct_id.empty() && i->second && i->second->account_id == acct_id)) {
                            i = okx_accounts_.erase(i);
                            result = true;
                        } else {
                            ++i;
                        }
                    }
                }
            } else if (exchange == ExchangeType::BINANCE) {
                auto it = binance_accounts_.find(strategy_id);
                if (it != binance_accounts_.end()) {
                    auto target = it->second;
                    std::string acct_id = target ? target->account_id : std::string();
                    for (auto i = binance_accounts_.begin(); i != binance_accounts_.end(); ) {
                        if (i->second == target ||
                            (!acct_id.empty() && i->second && i->second->account_id == acct_id)) {
                            i = binance_accounts_.erase(i);
                            result = true;
                        } else {
                            ++i;
                        }
                    }
                }
            }
        }

        if (result) {
            auto_save();  // 自动保存
        }
        return result;
    }

    /**
     * @brief 设置账户状态
     */
    bool set_account_status(const std::string& strategy_id, ExchangeType exchange,
                            AccountStatus status, const std::string& error_msg = "") {
        std::lock_guard<std::mutex> lock(mutex_);

        if (exchange == ExchangeType::OKX) {
            auto it = okx_accounts_.find(strategy_id);
            if (it != okx_accounts_.end() && it->second) {
                it->second->status = status;
                it->second->last_error = error_msg;
                return true;
            }
        } else if (exchange == ExchangeType::BINANCE) {
            auto it = binance_accounts_.find(strategy_id);
            if (it != binance_accounts_.end() && it->second) {
                it->second->status = status;
                it->second->last_error = error_msg;
                return true;
            }
        }

        return false;
    }

    /**
     * @brief 健康检查（验证API有效性）
     */
    bool health_check(const std::string& strategy_id, ExchangeType exchange) {
        if (exchange == ExchangeType::OKX) {
            auto* api = get_okx_api(strategy_id);
            if (!api) return false;

            try {
                auto result = api->get_account_balance();
                if (result.contains("code") && result["code"].get<std::string>() == "0") {
                    set_account_status(strategy_id, exchange, AccountStatus::ACTIVE);
                    return true;
                } else {
                    std::string err = result.contains("msg") ? result["msg"].get<std::string>() : "Unknown error";
                    set_account_status(strategy_id, exchange, AccountStatus::ERROR, err);
                    return false;
                }
            } catch (const std::exception& e) {
                set_account_status(strategy_id, exchange, AccountStatus::ERROR, e.what());
                return false;
            }
        } else if (exchange == ExchangeType::BINANCE) {
            auto* api = get_binance_api(strategy_id);
            if (!api) return false;

            try {
                // 使用 get_server_time 来验证连接 (返回时间戳，>0表示成功)
                int64_t server_time = api->get_server_time();
                if (server_time > 0) {
                    set_account_status(strategy_id, exchange, AccountStatus::ACTIVE);
                    return true;
                } else {
                    set_account_status(strategy_id, exchange, AccountStatus::ERROR, "Invalid server time");
                    return false;
                }
            } catch (const std::exception& e) {
                set_account_status(strategy_id, exchange, AccountStatus::ERROR, e.what());
                return false;
            }
        }

        return false;
    }

    /**
     * @brief 获取已注册账户数量（去重，共享同一 shared_ptr 的算一个）
     */
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::set<const void*> unique;
        for (const auto& [_, v] : okx_accounts_) unique.insert(v.get());
        for (const auto& [_, v] : binance_accounts_) unique.insert(v.get());
        return unique.size();
    }

    /**
     * @brief 添加账户别名（让 alias_id 共享 source_id 的 API 实例，不创建新连接）
     * 用于 Python 策略以 strategy_id 注册，但 account_id 已在 registry 中的场景
     */
    bool add_account_alias(const std::string& alias_id, const std::string& source_id, ExchangeType exchange) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (exchange == ExchangeType::OKX) {
            auto it = okx_accounts_.find(source_id);
            if (it != okx_accounts_.end()) {
                okx_accounts_[alias_id] = it->second;
                return true;
            }
        } else if (exchange == ExchangeType::BINANCE) {
            auto it = binance_accounts_.find(source_id);
            if (it != binance_accounts_.end()) {
                binance_accounts_[alias_id] = it->second;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 获取OKX账户数量
     */
    size_t okx_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return okx_accounts_.size();
    }

    /**
     * @brief 获取Binance账户数量
     */
    size_t binance_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return binance_accounts_.size();
    }

    /**
     * @brief 检查策略是否已注册
     */
    bool is_registered(const std::string& strategy_id, ExchangeType exchange) const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (exchange == ExchangeType::OKX) {
            return okx_accounts_.find(strategy_id) != okx_accounts_.end();
        } else if (exchange == ExchangeType::BINANCE) {
            return binance_accounts_.find(strategy_id) != binance_accounts_.end();
        }

        return false;
    }

    /**
     * @brief 更新账户监控数据（由 account_monitor 调用）
     */
    void update_monitor_data(const std::string& account_id, double eq, double pnl) {
        std::lock_guard<std::mutex> lock(mutex_);
        // 在所有交易所中查找该 account_id
        auto it_okx = okx_accounts_.find(account_id);
        if (it_okx != okx_accounts_.end() && it_okx->second) {
            it_okx->second->equity.store(eq);
            it_okx->second->unrealized_pnl.store(pnl);
            it_okx->second->monitor_update_time.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            return;
        }
        auto it_bn = binance_accounts_.find(account_id);
        if (it_bn != binance_accounts_.end() && it_bn->second) {
            it_bn->second->equity.store(eq);
            it_bn->second->unrealized_pnl.store(pnl);
            it_bn->second->monitor_update_time.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }
    }

    /**
     * @brief 更新账户持仓数据（由 account_monitor 调用）
     */
    void update_account_positions(const std::string& account_id, const nlohmann::json& positions) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it_okx = okx_accounts_.find(account_id);
        if (it_okx != okx_accounts_.end() && it_okx->second) {
            it_okx->second->update_positions(positions);
            return;
        }
        auto it_bn = binance_accounts_.find(account_id);
        if (it_bn != binance_accounts_.end() && it_bn->second) {
            it_bn->second->update_positions(positions);
        }
    }

    /**
     * @brief 获取账户持仓数据
     */
    nlohmann::json get_account_positions(const std::string& account_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it_okx = okx_accounts_.find(account_id);
        if (it_okx != okx_accounts_.end() && it_okx->second) {
            return it_okx->second->get_positions();
        }
        auto it_bn = binance_accounts_.find(account_id);
        if (it_bn != binance_accounts_.end() && it_bn->second) {
            return it_bn->second->get_positions();
        }
        return nlohmann::json::array();
    }

    /**
     * @brief 获取所有账户信息（用于显示）
     *
     * 双层去重:
     * 1. 指针去重 (alias 模式: 多 strategy_id 指向同一 shared_ptr)
     * 2. (exchange, api_key) 去重 (历史遗留: 不同 shared_ptr 但同一 api_key, 比如旧策略以 strategy_id 为主键 + 新策略以 account_id 为主键)
     */
    nlohmann::json get_all_accounts_info() const {
        std::lock_guard<std::mutex> lock(mutex_);

        nlohmann::json result = {
            {"okx", nlohmann::json::array()},
            {"binance", nlohmann::json::array()}
        };

        std::set<const void*> seen_ptrs;
        std::set<std::string> seen_keys;  // exchange + ":" + api_key

        for (const auto& [id, account] : okx_accounts_) {
            if (!account) continue;
            if (!seen_ptrs.insert(account.get()).second) continue;
            std::string ak = "okx:" + account->api_key;
            if (!seen_keys.insert(ak).second) continue;
            result["okx"].push_back(account->to_json());
        }

        for (const auto& [id, account] : binance_accounts_) {
            if (!account) continue;
            if (!seen_ptrs.insert(account.get()).second) continue;
            std::string ak = "binance:" + account->api_key;
            if (!seen_keys.insert(ak).second) continue;
            result["binance"].push_back(account->to_json());
        }

        return result;
    }

    /**
     * @brief 获取所有 OKX 账户的 API 指针（用于账户监控）
     */
    std::map<std::string, okx::OKXRestAPI*> get_all_okx_accounts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, okx::OKXRestAPI*> result;
        std::set<const void*> seen;

        for (const auto& [id, account] : okx_accounts_) {
            if (account && account->api && account->status == AccountStatus::ACTIVE
                && seen.insert(account.get()).second) {
                result[id] = account->api.get();
            }
        }

        return result;
    }

    /**
     * @brief 获取所有 Binance 账户的 API 指针（用于账户监控）
     */
    std::map<std::string, binance::BinanceRestAPI*> get_all_binance_accounts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, binance::BinanceRestAPI*> result;
        std::set<const void*> seen;

        for (const auto& [id, account] : binance_accounts_) {
            if (account && account->status == AccountStatus::ACTIVE
                && seen.insert(account.get()).second) {
                // 返回默认市场的 API
                auto* api = account->get_default_api();
                if (api) {
                    result[id] = api;
                }
            }
        }

        return result;
    }

    /**
     * @brief 从JSON加载账户配置
     */
    bool load_from_json(const nlohmann::json& config) {
        try {
            // 加载默认账户
            if (config.contains("default")) {
                const auto& def = config["default"];
                std::string exchange = def.value("exchange", "okx");
                std::string api_key = def.value("api_key", "");
                std::string secret_key = def.value("secret_key", "");
                std::string passphrase = def.value("passphrase", "");
                bool is_testnet = def.value("is_testnet", true);

                if (!api_key.empty() && api_key != "your_default_api_key") {
                    if (exchange == "okx") {
                        set_default_okx_account(api_key, secret_key, passphrase, is_testnet);
                    } else if (exchange == "binance") {
                        std::string market_str = def.value("market", "futures");
                        binance::MarketType market = binance::MarketType::FUTURES;
                        if (market_str == "spot") market = binance::MarketType::SPOT;
                        else if (market_str == "coin_futures") market = binance::MarketType::COIN_FUTURES;
                        set_default_binance_account(api_key, secret_key, is_testnet, market);
                    }
                }
            }

            // 加载策略账户
            if (config.contains("strategies")) {
                for (auto& [strategy_id, account_config] : config["strategies"].items()) {
                    std::string exchange = account_config.value("exchange", "okx");
                    std::string api_key = account_config.value("api_key", "");
                    std::string secret_key = account_config.value("secret_key", "");
                    std::string passphrase = account_config.value("passphrase", "");
                    bool is_testnet = account_config.value("is_testnet", true);
                    std::string acct_id = account_config.value("account_id", "");

                    if (api_key.empty() || api_key.find("your_") != std::string::npos) {
                        continue;  // 跳过占位符
                    }

                    if (exchange == "okx") {
                        register_okx_account(strategy_id, api_key, secret_key, passphrase, is_testnet, acct_id);
                    } else if (exchange == "binance") {
                        std::string market_str = account_config.value("market", "futures");
                        binance::MarketType market = binance::MarketType::FUTURES;
                        if (market_str == "spot") market = binance::MarketType::SPOT;
                        else if (market_str == "coin_futures") market = binance::MarketType::COIN_FUTURES;
                        register_binance_account(strategy_id, api_key, secret_key, is_testnet, market, acct_id);
                    }
                }
            }

            return true;
        } catch (const std::exception& e) {
            std::cerr << "[AccountRegistry] 加载配置失败: " << e.what() << std::endl;
            return false;
        }
    }

    /**
     * @brief 清空所有账户
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        okx_accounts_.clear();
        binance_accounts_.clear();
        default_okx_account_.reset();
        default_binance_account_.reset();
    }

    // ==================== 持久化功能 ====================

    /**
     * @brief 设置配置文件路径（启用自动持久化）
     */
    void set_config_path(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_path_ = path;
        auto_save_enabled_ = !path.empty();
        std::cout << "[AccountRegistry] 配置文件路径: " << path
                  << " | 自动保存: " << (auto_save_enabled_ ? "启用" : "禁用") << std::endl;
    }

    /**
     * @brief 从文件加载账户配置
     */
    bool load_from_file(const std::string& path = "") {
        std::string file_path = path.empty() ? config_path_ : path;
        if (file_path.empty()) {
            std::cerr << "[AccountRegistry] 未设置配置文件路径" << std::endl;
            return false;
        }

        try {
            std::ifstream file(file_path);
            if (!file.is_open()) {
                std::cerr << "[AccountRegistry] 无法打开配置文件: " << file_path << std::endl;
                return false;
            }

            nlohmann::json config;
            file >> config;
            file.close();

            bool result = load_from_json(config);
            if (result) {
                std::cout << "[AccountRegistry] ✓ 从 " << file_path << " 加载了 "
                          << count() << " 个账户" << std::endl;
            }
            return result;
        } catch (const std::exception& e) {
            std::cerr << "[AccountRegistry] 加载配置文件失败: " << e.what() << std::endl;
            return false;
        }
    }

    /**
     * @brief 保存账户配置到文件
     */
    bool save_to_file(const std::string& path = "") {
        std::string file_path = path.empty() ? config_path_ : path;
        if (file_path.empty()) {
            std::cerr << "[AccountRegistry] 未设置配置文件路径" << std::endl;
            return false;
        }

        try {
            nlohmann::json config = export_to_json();

            std::ofstream file(file_path);
            if (!file.is_open()) {
                std::cerr << "[AccountRegistry] 无法写入配置文件: " << file_path << std::endl;
                return false;
            }

            file << config.dump(2);  // 格式化输出，缩进2空格
            file.close();

            std::cout << "[AccountRegistry] ✓ 配置已保存到 " << file_path << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[AccountRegistry] 保存配置文件失败: " << e.what() << std::endl;
            return false;
        }
    }

    /**
     * @brief 导出账户配置为JSON（用于持久化）
     */
    nlohmann::json export_to_json() const {
        std::lock_guard<std::mutex> lock(mutex_);

        nlohmann::json config;
        config["description"] = "账户配置文件 - 支持多策略多账户多市场 (自动生成)";

        // 导出默认账户
        if (default_okx_account_) {
            config["default"] = {
                {"exchange", "okx"},
                {"api_key", default_okx_account_->api_key},
                {"secret_key", default_okx_account_->secret_key},
                {"passphrase", default_okx_account_->passphrase},
                {"is_testnet", default_okx_account_->is_testnet}
            };
        } else if (default_binance_account_) {
            std::string market_str = "futures";
            if (default_binance_account_->default_market == binance::MarketType::SPOT) {
                market_str = "spot";
            } else if (default_binance_account_->default_market == binance::MarketType::COIN_FUTURES) {
                market_str = "coin_futures";
            }
            config["default"] = {
                {"exchange", "binance"},
                {"api_key", default_binance_account_->api_key},
                {"secret_key", default_binance_account_->secret_key},
                {"is_testnet", default_binance_account_->is_testnet},
                {"market", market_str}
            };
        }

        // 导出策略账户
        config["strategies"] = nlohmann::json::object();

        for (const auto& [id, account] : okx_accounts_) {
            if (account && id != "_default_") {
                config["strategies"][id] = {
                    {"exchange", "okx"},
                    {"account_id", account->account_id},
                    {"api_key", account->api_key},
                    {"secret_key", account->secret_key},
                    {"passphrase", account->passphrase},
                    {"is_testnet", account->is_testnet}
                };
            }
        }

        for (const auto& [id, account] : binance_accounts_) {
            if (account && id != "_default_") {
                std::string market_str = "futures";
                if (account->default_market == binance::MarketType::SPOT) {
                    market_str = "spot";
                } else if (account->default_market == binance::MarketType::COIN_FUTURES) {
                    market_str = "coin_futures";
                }
                config["strategies"][id] = {
                    {"exchange", "binance"},
                    {"account_id", account->account_id},
                    {"api_key", account->api_key},
                    {"secret_key", account->secret_key},
                    {"is_testnet", account->is_testnet},
                    {"market", market_str}
                };
            }
        }

        return config;
    }

    /**
     * @brief 是否启用了自动保存
     */
    bool is_auto_save_enabled() const {
        return auto_save_enabled_;
    }

private:
    mutable std::mutex mutex_;

    // OKX账户
    std::map<std::string, std::shared_ptr<OKXAccountInfo>> okx_accounts_;
    std::shared_ptr<OKXAccountInfo> default_okx_account_;

    // Binance账户
    std::map<std::string, std::shared_ptr<BinanceAccountInfo>> binance_accounts_;
    std::shared_ptr<BinanceAccountInfo> default_binance_account_;

    // 持久化配置
    std::string config_path_;
    bool auto_save_enabled_ = false;

    /**
     * @brief 自动保存（如果启用）
     * @note 不加锁，调用方需要确保已持有锁或在锁外调用
     */
    void auto_save() {
        if (auto_save_enabled_ && !config_path_.empty()) {
            save_to_file();
        }
    }
};

// ==================== 全局账户注册表 ====================

/**
 * @brief 全局账户注册表实例
 *
 * 在整个程序中使用同一个实例管理所有账户
 */
inline AccountRegistry g_account_registry;

} // namespace trading
