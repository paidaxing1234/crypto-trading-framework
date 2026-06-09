#pragma once

/**
 * @file account_monitor.h
 * @brief 账户实时监控模块 - 定期查询账户状态并更新风控管理器
 *
 * 功能：
 * - 定期查询账户余额和持仓
 * - 计算实时盈亏
 * - 更新风控管理器状态
 * - 触发风控告警
 */

#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <sstream>
#include <mutex>
#include "../../trading/risk_manager.h"
#include "../../trading/account_registry.h"
#include "../../adapters/okx/okx_rest_api.h"
#include "../../adapters/okx/okx_websocket.h"
#include "../../adapters/binance/binance_rest_api.h"
#include "../../adapters/binance/binance_websocket.h"
#include "../../core/logger.h"

// 账户监控日志宏已废弃，改用 strat_log / acct_log 方法

namespace trading {
namespace server {

/**
 * @brief 账户信息结构（用于WebSocket连接）
 */
struct AccountCredentials {
    std::string api_key;
    std::string secret_key;
    std::string passphrase;  // OKX专用
    bool is_testnet;

    AccountCredentials() : is_testnet(false) {}
    AccountCredentials(const std::string& key, const std::string& secret,
                      const std::string& pass = "", bool testnet = false)
        : api_key(key), secret_key(secret), passphrase(pass), is_testnet(testnet) {}
};

/**
 * @brief 账户监控器 - 使用WebSocket推送代替REST API轮询
 *
 * 改进：
 * - 使用WebSocket订阅账户余额和持仓推送
 * - 大幅减少API请求次数（避免触发限流）
 * - 实时性更好（推送延迟<100ms）
 * - 保持所有原有功能（余额检查、持仓监控、风控告警等）
 */
class AccountMonitor {
public:
    AccountMonitor(RiskManager& risk_manager)
        : risk_manager_(risk_manager), running_(false), use_websocket_(true) {}

    // 获取策略级日志源: 独立账户用 account_id，策略用 {account_id}_{strategy_id}
    std::string strat_log_src(const std::string& strategy_id) const {
        auto it = account_id_map_.find(strategy_id);
        std::string acct_id = (it != account_id_map_.end()) ? it->second : strategy_id;
        // 避免重复：当 account_id == strategy_id 时只用 account_id
        if (acct_id == strategy_id) {
            return acct_id;
        }
        return acct_id + "_" + strategy_id;
    }

    // 写策略级日志到 {account_id}_{strategy_id}_{date}.log
    void strat_log(const std::string& strategy_id, const std::string& msg) {
        trading::core::Logger::instance().info(strat_log_src(strategy_id), msg);
        std::cout << msg << std::endl;
    }

    ~AccountMonitor() {
        stop();
    }

    /**
     * @brief 设置是否使用WebSocket模式（默认true）
     */
    void set_use_websocket(bool enabled) {
        use_websocket_ = enabled;
    }

    /**
     * @brief 启动监控线程
     * @param interval_seconds 监控间隔（秒），仅在REST API模式下使用
     */
    void start(int interval_seconds = 5) {
        if (running_) {
            return;
        }

        running_ = true;

        if (use_websocket_) {
            // WebSocket模式：连接所有WebSocket并订阅推送
            std::cout << "[账户监控] 启动 WebSocket 推送模式" << std::endl;
            start_websocket_connections();
        } else {
            // REST API轮询模式（旧版本，保留兼容）
            std::cout << "[账户监控] 启动 REST API 轮询模式，间隔: " << interval_seconds << "秒" << std::endl;
            monitor_thread_ = std::thread([this, interval_seconds]() {
                while (running_) {
                    try {
                        update_all_accounts();
                    } catch (const std::exception& e) {
                        trading::core::Logger::instance().error("system", "[账户监控] 错误: " + std::string(e.what()));
                    }

                    // 等待指定间隔
                    for (int i = 0; i < interval_seconds && running_; i++) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                }
                trading::core::Logger::instance().info("system", "[账户监控] 已停止");
            });
        }
    }

    /**
     * @brief 停止监控线程
     */
    void stop() {
        if (!running_) {
            return;
        }

        running_ = false;

        if (use_websocket_) {
            // 断开所有WebSocket连接
            stop_websocket_connections();
        } else {
            // 停止REST API轮询线程
            if (monitor_thread_.joinable()) {
                monitor_thread_.join();
            }
        }
    }

    /**
     * @brief 注册需要监控的 OKX 账户
     * @param strategy_id 策略ID
     * @param api REST API指针（REST模式使用）
     * @param credentials 账户凭证（WebSocket模式使用）
     */
    void register_okx_account(const std::string& strategy_id, okx::OKXRestAPI* api,
                             const AccountCredentials* credentials = nullptr,
                             const std::string& account_id = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        okx_accounts_[strategy_id] = api;
        if (!account_id.empty()) {
            account_id_map_[strategy_id] = account_id;
        }

        if (use_websocket_ && credentials && running_) {
            // 创建WebSocket客户端
            auto ws = std::make_shared<okx::OKXWebSocket>(
                credentials->api_key,
                credentials->secret_key,
                credentials->passphrase,
                credentials->is_testnet,
                okx::WsEndpointType::PRIVATE
            );

            // 设置账户余额和持仓回调
            ws->set_balance_and_position_callback([this, strategy_id](const nlohmann::json& data) {
                this->on_okx_balance_and_position_update(strategy_id, data);
            });

            okx_ws_clients_[strategy_id] = ws;
            okx_credentials_[strategy_id] = *credentials;

            // 立即连接WebSocket
            strat_log(strategy_id, "[账户监控WS] 连接 OKX WebSocket: " + strategy_id);
            if (ws->connect()) {
                ws->login();
                if (ws->wait_for_login(5000)) {
                    ws->subscribe_balance_and_position();
                    strat_log(strategy_id, "[账户监控WS] ✓ OKX " + strategy_id + " 已订阅账户推送");
                } else {
                    strat_log(strategy_id, "[账户监控WS] ✗ OKX " + strategy_id + " 登录超时");
                }
            } else {
                strat_log(strategy_id, "[账户监控WS] ✗ OKX " + strategy_id + " 连接失败");
            }
        } else if (use_websocket_ && credentials) {
            // 如果监控还未���动，只创建客户端，等待start()时连接
            auto ws = std::make_shared<okx::OKXWebSocket>(
                credentials->api_key,
                credentials->secret_key,
                credentials->passphrase,
                credentials->is_testnet,
                okx::WsEndpointType::PRIVATE
            );

            ws->set_balance_and_position_callback([this, strategy_id](const nlohmann::json& data) {
                this->on_okx_balance_and_position_update(strategy_id, data);
            });

            okx_ws_clients_[strategy_id] = ws;
            okx_credentials_[strategy_id] = *credentials;
        }

        strat_log(strategy_id, "[账户监控] 注册 OKX 账户: " + strategy_id);
    }

    /**
     * @brief 注册需要监控的 Binance 账户
     * @param strategy_id 策略ID
     * @param api REST API指针（REST模式使用）
     * @param credentials 账户凭证（WebSocket模式使用）
     */
    void register_binance_account(const std::string& strategy_id, binance::BinanceRestAPI* api,
                                  const AccountCredentials* credentials = nullptr,
                                  const std::string& account_id = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        binance_accounts_[strategy_id] = api;
        if (!account_id.empty()) {
            account_id_map_[strategy_id] = account_id;
        }

        if (use_websocket_ && credentials && running_) {
            // 创建WebSocket客户端
            auto ws = std::make_shared<binance::BinanceWebSocket>(
                credentials->api_key,
                credentials->secret_key,
                binance::WsConnectionType::USER,
                binance::MarketType::FUTURES,
                credentials->is_testnet
            );

            // 设置账户更新回调
            ws->set_account_update_callback([this, strategy_id](const nlohmann::json& data) {
                this->on_binance_account_update(strategy_id, data);
            });

            binance_ws_clients_[strategy_id] = ws;
            binance_credentials_[strategy_id] = *credentials;

            // 立即连接WebSocket
            strat_log(strategy_id, "[账户监控WS] 连接 Binance WebSocket: " + strategy_id);

            auto listen_key_result = api->create_listen_key();
            if (listen_key_result.contains("listenKey")) {
                std::string listen_key = listen_key_result["listenKey"];
                strat_log(strategy_id, "[账户监控WS] Binance " + strategy_id + " listenKey: " + listen_key);

                if (ws->connect_user_stream(listen_key)) {
                    // 启动自动刷新listenKey（每50分钟）
                    ws->start_auto_refresh_listen_key(api, 3000);
                    strat_log(strategy_id, "[账户监控WS] ✓ Binance " + strategy_id + " 已连接用户数据流");
                } else {
                    strat_log(strategy_id, "[账户监控WS] ✗ Binance " + strategy_id + " 连接用户数据流失败");
                }
            } else {
                strat_log(strategy_id, "[账户监控WS] ✗ Binance " + strategy_id + " 获取listenKey失败");
            }
        } else if (use_websocket_ && credentials) {
            // 如果监控还未启动，只创建客户端，等待start()时连接
            auto ws = std::make_shared<binance::BinanceWebSocket>(
                credentials->api_key,
                credentials->secret_key,
                binance::WsConnectionType::USER,
                binance::MarketType::FUTURES,
                credentials->is_testnet
            );

            ws->set_account_update_callback([this, strategy_id](const nlohmann::json& data) {
                this->on_binance_account_update(strategy_id, data);
            });

            binance_ws_clients_[strategy_id] = ws;
            binance_credentials_[strategy_id] = *credentials;
        }

        strat_log(strategy_id, "[账户监控] 注册 Binance 账户: " + strategy_id);
    }

    /**
     * @brief 注销 OKX 账户监控
     */
    void unregister_okx_account(const std::string& strategy_id) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = okx_accounts_.find(strategy_id);
        if (it != okx_accounts_.end()) {
            okx_accounts_.erase(it);
        }

        if (use_websocket_) {
            auto ws_it = okx_ws_clients_.find(strategy_id);
            if (ws_it != okx_ws_clients_.end()) {
                if (ws_it->second && ws_it->second->is_connected()) {
                    ws_it->second->disconnect();
                }
                okx_ws_clients_.erase(ws_it);
            }
            okx_credentials_.erase(strategy_id);
        }

        strat_log(strategy_id, "[账户监控] 注销 OKX 账户: " + strategy_id);
    }

    /**
     * @brief 注销 Binance 账户监控
     */
    void unregister_binance_account(const std::string& strategy_id) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = binance_accounts_.find(strategy_id);
        if (it != binance_accounts_.end()) {
            binance_accounts_.erase(it);
        }

        if (use_websocket_) {
            auto ws_it = binance_ws_clients_.find(strategy_id);
            if (ws_it != binance_ws_clients_.end()) {
                if (ws_it->second && ws_it->second->is_connected()) {
                    ws_it->second->stop_auto_refresh_listen_key();
                    ws_it->second->disconnect();
                }
                binance_ws_clients_.erase(ws_it);
            }
            binance_credentials_.erase(strategy_id);
        }

        strat_log(strategy_id, "[账户监控] 注销 Binance 账户: " + strategy_id);
    }

    /**
     * @brief 手动触发一次账户更新
     */
    void update_all_accounts() {
        std::cout << "\n========== [账户监控] 开始更新所有账户 ==========" << std::endl;

        // 加锁拷贝账户快照，避免遍历时其他线程修改 map 导致数据竞争
        std::map<std::string, okx::OKXRestAPI*> okx_snapshot;
        std::map<std::string, binance::BinanceRestAPI*> binance_snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            okx_snapshot = okx_accounts_;
            binance_snapshot = binance_accounts_;
        }

        // 更新所有 OKX 账户
        for (const auto& [strategy_id, api] : okx_snapshot) {
            if (!api) {
                strat_log(strategy_id, "[账户监控] ⚠️  OKX 账户 " + strategy_id + " API 指针无效");
                continue;
            }

            bool success = update_okx_account(strategy_id, api);
            if (!success) {
                okx_fail_count_[strategy_id]++;
                strat_log(strategy_id, "[账户监控] ⚠️  OKX 账户 " + strategy_id + " 更新失败 (连续" + std::to_string(okx_fail_count_[strategy_id]) + "次)");
            } else {
                if (okx_fail_count_[strategy_id] > 0) {
                    strat_log(strategy_id, "[账户监控] ✓ OKX 账户 " + strategy_id + " 恢复正常");
                }
                okx_fail_count_[strategy_id] = 0;
            }
        }

        // 更新所有 Binance 账户
        for (const auto& [strategy_id, api] : binance_snapshot) {
            if (!api) {
                strat_log(strategy_id, "[账户监控] ⚠️  Binance 账户 " + strategy_id + " API 指针无效");
                continue;
            }

            bool success = update_binance_account(strategy_id, api);
            if (!success) {
                binance_fail_count_[strategy_id]++;
                strat_log(strategy_id, "[账户监控] ⚠️  Binance 账户 " + strategy_id + " 更新失败 (连续" + std::to_string(binance_fail_count_[strategy_id]) + "次)");
            } else {
                if (binance_fail_count_[strategy_id] > 0) {
                    strat_log(strategy_id, "[账户监控] ✓ Binance 账户 " + strategy_id + " 恢复正常");
                }
                binance_fail_count_[strategy_id] = 0;
            }
        }

        std::cout << "========== [账户监控] 更新完成 ==========" << std::endl;
    }

private:
    /**
     * @brief 更新单个 OKX 账户状态
     * @return true 更新成功，false 更新失败（账户可能已注销）
     */
    bool update_okx_account(const std::string& strategy_id, okx::OKXRestAPI* api) {
        if (!api) return false;
        auto& log = trading::core::Logger::instance();
        std::string acct_id = account_id_map_.count(strategy_id) ? account_id_map_[strategy_id] : strategy_id;
        std::string acct_src = acct_id;
        std::string strat_src = (acct_id == strategy_id) ? acct_id : (acct_id + "_" + strategy_id);

        try {
            log.info(strat_src, "[账户监控] 正在查询 OKX 账户: " + strategy_id);

            // 1. 查询账户余额
            auto balance_result = api->get_account_balance("");
            if (balance_result["code"] == "0" && balance_result.contains("data")) {
                double total_equity = 0.0;
                double unrealized_pnl = 0.0;

                for (const auto& detail : balance_result["data"][0]["details"]) {
                    double eq = std::stod(detail.value("eq", "0"));
                    double upl = std::stod(detail.value("upl", "0"));
                    total_equity += eq;
                    unrealized_pnl += upl;
                }

                log.info(acct_src, "[账户监控] " + strategy_id + " - 总权益: " +
                    std::to_string(total_equity) + " USDT, 未实现盈亏: " + std::to_string(unrealized_pnl) + " USDT");

                // 检查账户余额
                auto balance_check = risk_manager_.check_account_balance(total_equity);
                if (!balance_check.passed) {
                    log.warn(acct_src, "[账户监控] ⚠️  " + strategy_id + " - 余额告警: " + balance_check.reason);
                    risk_manager_.send_alert(
                        "[" + strategy_id + "] " + balance_check.reason,
                        AlertLevel::WARNING,
                        "账户余额不足"
                    );
                } else {
                    log.info(acct_src, "[账户监控] ✓ " + strategy_id + " - 余额正常");
                }

                // 更新账户总权益（用于回撤检查）
                risk_manager_.update_account_equity(total_equity, strategy_id);

                // 更新每日盈亏（仅用于统计）
                risk_manager_.update_daily_pnl(unrealized_pnl);

                // 更新 registry 中的监控数据（供前端查询）
                g_account_registry.update_monitor_data(acct_id, total_equity, unrealized_pnl);
            }

            // 2. 查询持仓
            auto positions_result = api->get_positions("SWAP", "");
            if (positions_result["code"] == "0" && positions_result.contains("data")) {
                int position_count = 0;
                nlohmann::json active_positions = nlohmann::json::array();
                for (const auto& pos : positions_result["data"]) {
                    std::string symbol = pos.value("instId", "");
                    double notional = std::stod(pos.value("notionalUsd", "0"));

                    if (std::abs(notional) > 0.01) {  // 只显示有效持仓
                        log.info(acct_src, "[账户监控] " + strategy_id + " - 持仓: " +
                            symbol + " = " + std::to_string(notional) + " USDT");
                        position_count++;
                        active_positions.push_back(pos);
                    }

                    // 更新持仓到风控管理器
                    risk_manager_.update_position(symbol, std::abs(notional));
                }

                // 存储持仓到 registry（供前端查询）
                g_account_registry.update_account_positions(acct_id, active_positions);

                if (position_count == 0) {
                    log.info(acct_src, "[账户监控] " + strategy_id + " - 无持仓");
                }
            }

            // 3. 查询挂单数量
            auto orders_result = api->get_pending_orders("SWAP", "");
            if (orders_result["code"] == "0" && orders_result.contains("data")) {
                int open_orders = orders_result["data"].size();
                log.info(acct_src, "[账户监控] " + strategy_id + " - 挂单数量: " + std::to_string(open_orders));
                risk_manager_.set_open_order_count(open_orders);
            }

            log.info(strat_src, "[账户监控] ✓ " + strategy_id + " 更新完成");
            return true;

        } catch (const std::exception& e) {
            log.error(strat_src, "[账户监控] ✗ OKX账户 " + strategy_id + " 更新失败: " + std::string(e.what()));
            return false;
        }
    }

    /**
     * @brief 更新单个 Binance 账户状态
     * @return true 更新成功，false 更新失败（账户可能已注销）
     */
    bool update_binance_account(const std::string& strategy_id, binance::BinanceRestAPI* api) {
        if (!api) return false;
        auto& log = trading::core::Logger::instance();
        std::string acct_id = account_id_map_.count(strategy_id) ? account_id_map_[strategy_id] : strategy_id;
        std::string acct_src = acct_id;
        std::string strat_src = (acct_id == strategy_id) ? acct_id : (acct_id + "_" + strategy_id);

        try {
            log.info(strat_src, "[账户监控] 正在查询 Binance 账户: " + strategy_id);

            // 1. 查询账户余额
            auto balance_result = api->get_account_balance();

            double total_balance = 0.0;
            double unrealized_pnl = 0.0;

            // 检查返回结果是否是数组（直接返回数组）
            if (balance_result.is_array()) {
                for (const auto& asset : balance_result) {
                    if (asset.contains("balance")) {
                        double balance = std::stod(asset.value("balance", "0"));
                        total_balance += balance;
                    }
                    if (asset.contains("crossUnPnl")) {
                        double pnl = std::stod(asset.value("crossUnPnl", "0"));
                        unrealized_pnl += pnl;
                    }
                }
            }
            // 检查返回结果是否是对象（包含 code 和 data）
            else if (balance_result.is_object() &&
                     balance_result.contains("code") &&
                     balance_result["code"] == 200 &&
                     balance_result.contains("data") &&
                     balance_result["data"].is_array()) {
                for (const auto& asset : balance_result["data"]) {
                    if (asset.contains("balance")) {
                        double balance = std::stod(asset.value("balance", "0"));
                        total_balance += balance;
                    }
                    if (asset.contains("crossUnPnl")) {
                        double pnl = std::stod(asset.value("crossUnPnl", "0"));
                        unrealized_pnl += pnl;
                    }
                }
            } else {
                log.warn(acct_src, "[账户监控] " + strategy_id + " - 余额查询返回格式未知，跳过");
            }

            if (total_balance > 0 || unrealized_pnl != 0) {
                log.info(acct_src, "[账户监控] " + strategy_id + " - 总余额: " +
                    std::to_string(total_balance) + " USDT, 未实现盈亏: " + std::to_string(unrealized_pnl) + " USDT");

                // 检查账户余额
                auto balance_check = risk_manager_.check_account_balance(total_balance);
                if (!balance_check.passed) {
                    log.warn(acct_src, "[账户监控] ⚠️  " + strategy_id + " - 余额告警: " + balance_check.reason);
                    risk_manager_.send_alert(
                        "[" + strategy_id + "] " + balance_check.reason,
                        AlertLevel::WARNING,
                        "账户余额不足"
                    );
                } else {
                    log.info(acct_src, "[账户监控] ✓ " + strategy_id + " - 余额正常");
                }

                // 更新账户总权益（用于回撤检查）
                risk_manager_.update_account_equity(total_balance, strategy_id);

                // 更新每日盈亏（仅用于统计）
                risk_manager_.update_daily_pnl(unrealized_pnl);

                // 更新 registry 中的监控数据（供前端查询）
                g_account_registry.update_monitor_data(acct_id, total_balance, unrealized_pnl);
            }

            // 2. 查询持仓
            auto positions_result = api->get_positions();

            int position_count = 0;
            nlohmann::json active_positions = nlohmann::json::array();

            // 检查是否是数组（直接返回）
            if (positions_result.is_array()) {
                for (const auto& pos : positions_result) {
                    std::string symbol = pos.value("symbol", "");
                    double notional = std::stod(pos.value("notional", "0"));

                    if (std::abs(notional) > 0.01) {  // 只显示有效持仓
                        log.info(acct_src, "[账户监控] " + strategy_id + " - 持仓: " +
                            symbol + " = " + std::to_string(notional) + " USDT");
                        position_count++;
                        active_positions.push_back(pos);
                    }

                    // 更新持仓到风控管理器
                    risk_manager_.update_position(symbol, std::abs(notional));
                }
            }
            // 检查是否是对象（包含 code 和 data）
            else if (positions_result.is_object() &&
                     positions_result.contains("code") &&
                     positions_result["code"] == 200 &&
                     positions_result.contains("data") &&
                     positions_result["data"].is_array()) {
                for (const auto& pos : positions_result["data"]) {
                    std::string symbol = pos.value("symbol", "");
                    double notional = std::stod(pos.value("notional", "0"));

                    if (std::abs(notional) > 0.01) {  // 只显示有效持仓
                        log.info(acct_src, "[账户监控] " + strategy_id + " - 持仓: " +
                            symbol + " = " + std::to_string(notional) + " USDT");
                        position_count++;
                        active_positions.push_back(pos);
                    }

                    // 更新持仓到风控管理器
                    risk_manager_.update_position(symbol, std::abs(notional));
                }
            }

            // 存储持仓到 registry（供前端查询）
            g_account_registry.update_account_positions(acct_id, active_positions);

            if (position_count == 0) {
                log.info(acct_src, "[账户监控] " + strategy_id + " - 无持仓");
            }

            log.info(strat_src, "[账户监控] ✓ " + strategy_id + " 更新完成");
            return true;

        } catch (const std::exception& e) {
            log.error(strat_src, "[账户监控] ✗ Binance账户 " + strategy_id + " 更新失败: " + std::string(e.what()));
            return false;
        }
    }

    /**
     * @brief 启动所有WebSocket连接
     */
    void start_websocket_connections() {
        // 连接所有OKX WebSocket
        for (auto& [strategy_id, ws] : okx_ws_clients_) {
            if (ws && !ws->is_connected()) {
                strat_log(strategy_id, "[账户监控WS] 连接 OKX WebSocket: " + strategy_id);
                if (ws->connect()) {
                    ws->login();
                    if (ws->wait_for_login(5000)) {
                        ws->subscribe_balance_and_position();
                        strat_log(strategy_id, "[账户监控WS] ✓ OKX " + strategy_id + " 已订阅账户推送");
                    } else {
                        strat_log(strategy_id, "[账户监控WS] ✗ OKX " + strategy_id + " 登录超时");
                    }
                } else {
                    strat_log(strategy_id, "[账户监控WS] ✗ OKX " + strategy_id + " 连接失败");
                }
            }
        }

        // 连接所有Binance WebSocket
        for (auto& [strategy_id, ws] : binance_ws_clients_) {
            if (ws && !ws->is_connected()) {
                strat_log(strategy_id, "[账户监控WS] 连接 Binance WebSocket: " + strategy_id);

                // 获取listenKey
                auto api = binance_accounts_[strategy_id];
                if (!api) {
                    strat_log(strategy_id, "[账户监控WS] ✗ Binance " + strategy_id + " REST API未初始化");
                    continue;
                }

                auto listen_key_result = api->create_listen_key();
                if (listen_key_result.contains("listenKey")) {
                    std::string listen_key = listen_key_result["listenKey"];
                    strat_log(strategy_id, "[账户监控WS] Binance " + strategy_id + " listenKey: " + listen_key);

                    if (ws->connect_user_stream(listen_key)) {
                        ws->start_auto_refresh_listen_key(api, 3000);
                        strat_log(strategy_id, "[账户监控WS] ✓ Binance " + strategy_id + " 已连接用户数据流");
                    } else {
                        strat_log(strategy_id, "[账户监控WS] ✗ Binance " + strategy_id + " 连接用户数据流失败");
                    }
                } else {
                    strat_log(strategy_id, "[账户监控WS] ✗ Binance " + strategy_id + " 获取listenKey失败");
                }
            }
        }

        std::cout << "[账户监控WS] 所有WebSocket连接完成" << std::endl;
    }

    /**
     * @brief 停止所有WebSocket连接
     */
    void stop_websocket_connections() {
        std::cout << "[账户监控WS] 停止中..." << std::endl;

        // 断开所有OKX WebSocket
        for (auto& [strategy_id, ws] : okx_ws_clients_) {
            if (ws && ws->is_connected()) {
                ws->disconnect();
                strat_log(strategy_id, "[账户监控WS] 断开 OKX WebSocket: " + strategy_id);
            }
        }

        // 断开所有Binance WebSocket
        for (auto& [strategy_id, ws] : binance_ws_clients_) {
            if (ws && ws->is_connected()) {
                ws->stop_auto_refresh_listen_key();
                ws->disconnect();
                strat_log(strategy_id, "[账户监控WS] 断开 Binance WebSocket: " + strategy_id);
            }
        }

        std::cout << "[账户监控WS] 已停止" << std::endl;
    }

    /**
     * @brief OKX账户余额和持仓推送回调
     */
    void on_okx_balance_and_position_update(const std::string& strategy_id, const nlohmann::json& data) {
        try {
            std::string acct_id = account_id_map_.count(strategy_id) ? account_id_map_[strategy_id] : strategy_id;
            auto& log = trading::core::Logger::instance();
            std::string acct_src = acct_id;  // 账户级日志 → {account_id}_{date}.log
            std::string strat_src = (acct_id == strategy_id) ? acct_id : (acct_id + "_" + strategy_id);  // 策略级日志

            log.info(strat_src, "[账户监控WS] 收到 OKX " + strategy_id + " 账户推送");

            // 解析余额数据
            if (data.contains("balData") && data["balData"].is_array()) {
                double total_equity = 0.0;
                for (const auto& bal : data["balData"]) {
                    if (bal.contains("cashBal")) {
                        double cash_bal = std::stod(bal.value("cashBal", "0"));
                        total_equity += cash_bal;
                    }
                }

                if (total_equity > 0) {
                    log.info(acct_src, "[账户监控WS] " + strategy_id + " - 总权益: " + std::to_string(total_equity) + " USDT");

                    // 检查账户余额
                    auto balance_check = risk_manager_.check_account_balance(total_equity);
                    if (!balance_check.passed) {
                        log.warn(acct_src, "[账户监控WS] ⚠️  " + strategy_id + " - 余额告警: " + balance_check.reason);
                        risk_manager_.send_alert(
                            "[" + strategy_id + "] " + balance_check.reason,
                            AlertLevel::WARNING,
                            "账户余额不足"
                        );
                    } else {
                        log.info(acct_src, "[账户监控WS] ✓ " + strategy_id + " - 余额正常");
                    }

                    // 更新账户总权益（用于回撤检查）
                    risk_manager_.update_account_equity(total_equity, strategy_id);

                    // 更新 registry 中的监控数据（供前端查询）
                    g_account_registry.update_monitor_data(acct_id, total_equity, 0.0);
                }
            }

            // 解析持仓数据
            if (data.contains("posData") && data["posData"].is_array() && !data["posData"].empty()) {
                int position_count = 0;
                for (const auto& pos : data["posData"]) {
                    std::string inst_id = pos.value("instId", "");
                    double notional = 0.0;

                    // 优先使用 notionalUsd
                    if (pos.contains("notionalUsd")) {
                        notional = std::stod(pos.value("notionalUsd", "0"));
                    }

                    // 如果 notionalUsd 为0或不存在，尝试用持仓数量和均价计算
                    if (std::abs(notional) < 0.01 && pos.contains("pos") && pos.contains("avgPx")) {
                        double position_qty = std::stod(pos.value("pos", "0"));
                        double avg_price = std::stod(pos.value("avgPx", "0"));
                        notional = std::abs(position_qty * avg_price);
                    }

                    if (std::abs(notional) > 0.01) {
                        std::string pos_side = pos.value("posSide", "");
                        log.info(acct_src, "[账户监控WS] " + strategy_id + " - 持仓: " + inst_id
                                  + " (" + pos_side + ") = " + std::to_string(notional) + " USDT");
                        position_count++;
                        risk_manager_.update_position(inst_id, std::abs(notional));
                    }
                }

                // 只在收到持仓推送时才显示"无持仓"
                if (position_count == 0) {
                    log.info(acct_src, "[账户监控WS] " + strategy_id + " - 无持仓");
                }
            }

        } catch (const std::exception& e) {
            strat_log(strategy_id, "[账户监控WS] ✗ OKX " + strategy_id + " 数据解析失败: " + e.what());
        }
    }

    /**
     * @brief Binance账户更新推送回调
     */
    void on_binance_account_update(const std::string& strategy_id, const nlohmann::json& data) {
        try {
            std::string acct_id = account_id_map_.count(strategy_id) ? account_id_map_[strategy_id] : strategy_id;
            auto& log = trading::core::Logger::instance();
            std::string acct_src = acct_id;  // 账户级日志 → {account_id}_{date}.log
            std::string strat_src = (acct_id == strategy_id) ? acct_id : (acct_id + "_" + strategy_id);  // 策略级日志

            log.info(strat_src, "[账户监控WS] 收到 Binance " + strategy_id + " 账户推送");

            // Binance USER_DATA事件类型：ACCOUNT_UPDATE
            if (data.contains("e") && data["e"] == "ACCOUNT_UPDATE") {
                // 解析余额数据
                if (data.contains("a") && data["a"].contains("B")) {
                    double total_balance = 0.0;
                    double unrealized_pnl = 0.0;

                    for (const auto& bal : data["a"]["B"]) {
                        if (bal.contains("wb")) {  // wallet balance
                            double balance = std::stod(bal.value("wb", "0"));
                            total_balance += balance;
                        }
                        if (bal.contains("cw")) {  // cross wallet balance
                            double cross_bal = std::stod(bal.value("cw", "0"));
                            // 使用cross wallet balance作为总余额
                            total_balance = cross_bal;
                        }
                    }

                    if (total_balance > 0) {
                        log.info(acct_src, "[账户监控WS] " + strategy_id + " - 总余额: " + std::to_string(total_balance) + " USDT");

                        // 检查账户余额
                        auto balance_check = risk_manager_.check_account_balance(total_balance);
                        if (!balance_check.passed) {
                            log.warn(acct_src, "[账户监控WS] ⚠️  " + strategy_id + " - 余额告警: " + balance_check.reason);
                            risk_manager_.send_alert(
                                "[" + strategy_id + "] " + balance_check.reason,
                                AlertLevel::WARNING,
                                "账户余额不足"
                            );
                        } else {
                            log.info(acct_src, "[账户监控WS] ✓ " + strategy_id + " - 余额正常");
                        }

                        // 更新账户总权益
                        risk_manager_.update_account_equity(total_balance, strategy_id);

                        // 更新每日盈亏（仅用于统计）
                        risk_manager_.update_daily_pnl(unrealized_pnl);

                        // 更新 registry 中的监控数据（供前端查询）
                        g_account_registry.update_monitor_data(acct_id, total_balance, unrealized_pnl);
                    }
                }

                // 解析持仓数据
                if (data.contains("a") && data["a"].contains("P")) {
                    int position_count = 0;
                    for (const auto& pos : data["a"]["P"]) {
                        std::string symbol = pos.value("s", "");
                        double notional = 0.0;

                        if (pos.contains("pa")) {  // position amount
                            double amount = std::stod(pos.value("pa", "0"));
                            if (pos.contains("ep")) {  // entry price
                                double entry_price = std::stod(pos.value("ep", "0"));
                                notional = std::abs(amount * entry_price);
                            }
                        }

                        if (std::abs(notional) > 0.01) {
                            log.info(acct_src, "[账户监控WS] " + strategy_id + " - 持仓: " + symbol + " = " + std::to_string(notional) + " USDT");
                            position_count++;
                            risk_manager_.update_position(symbol, std::abs(notional));
                        }
                    }

                    if (position_count == 0) {
                        log.info(acct_src, "[账户监控WS] " + strategy_id + " - 无持仓");
                    }
                }
            }

        } catch (const std::exception& e) {
            strat_log(strategy_id, "[账户监控WS] ✗ Binance " + strategy_id + " 数据解析失败: " + e.what());
        }
    }

    RiskManager& risk_manager_;
    std::atomic<bool> running_;
    std::atomic<bool> use_websocket_;  // 是否使用WebSocket模式
    std::thread monitor_thread_;
    std::mutex mutex_;

    // REST API客户端（兼容旧版本）
    std::map<std::string, okx::OKXRestAPI*> okx_accounts_;
    std::map<std::string, binance::BinanceRestAPI*> binance_accounts_;

    // WebSocket客户端
    std::map<std::string, std::shared_ptr<okx::OKXWebSocket>> okx_ws_clients_;
    std::map<std::string, std::shared_ptr<binance::BinanceWebSocket>> binance_ws_clients_;

    // 账户凭证（用于WebSocket连接）
    std::map<std::string, AccountCredentials> okx_credentials_;
    std::map<std::string, AccountCredentials> binance_credentials_;

    // strategy_id → account_id 映射
    std::map<std::string, std::string> account_id_map_;

    // 失败计数器 - 连续失败3次才注销账户
    std::map<std::string, int> okx_fail_count_;
    std::map<std::string, int> binance_fail_count_;
    static constexpr int MAX_FAIL_COUNT = 3;
};

} // namespace server
} // namespace trading
