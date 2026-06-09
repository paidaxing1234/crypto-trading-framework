#pragma once

/**
 * @file risk_manager.h
 * @brief 风险管理器 - 保护账户安全的核心组件
 *
 * 功能：
 * - 订单前置风险检查
 * - 持仓限制监控
 * - 最大回撤保护
 * - 紧急止损（Kill Switch）
 * - 多渠道告警（电话/短信/邮件/钉钉）
 */

#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <deque>
#include <iostream>
#include <nlohmann/json.hpp>
#include "data.h"
#include "order.h"
#include "logger.h"

namespace trading {

/**
 * @brief 告警级别
 */
enum class AlertLevel {
    INFO = 1,
    WARNING = 2,
    CRITICAL = 3
};

/**
 * @brief 告警配置
 */
struct AlertConfig {
    bool email_enabled = true;      // 邮件告警
    bool lark_enabled = true;       // 飞书告警

    std::string alerts_path = "";   // alerts 脚本路径，为空则自动检测
    std::string python_path = "python3";  // Python 解释器路径
    std::string email_config_file = "";   // 邮件配置文件路径
    std::string lark_config_file = "";    // 飞书配置文件路径
};

/**
 * @brief 告警服务 - 调用 Python 告警脚本
 */
class AlertService {
public:
    AlertService(const AlertConfig& config = AlertConfig()) : config_(config) {
        // 自动检测 alerts 路径
        if (config_.alerts_path.empty()) {
            // 尝试相对于当前文件的路径
            std::filesystem::path current_file(__FILE__);
            config_.alerts_path = current_file.parent_path().string() + "/alerts";
        }
    }

    /**
     * @brief 发送邮件告警
     */
    void send_email_alert(const std::string& message, AlertLevel level = AlertLevel::WARNING,
                          const std::string& subject = "", const std::string& to_emails = "",
                          const std::string& alert_type = "default", bool async = true) {
        if (!config_.email_enabled) {
            alert_log("[邮件通知] 邮件告警已禁用，跳过发送");
            return;
        }
        alert_log("[邮件通知] 发送邮件告警: level=" + level_to_string(level) +
                  ", subject=" + (subject.empty() ? "(默认)" : subject) +
                  ", to=" + (to_emails.empty() ? "(配置文件)" : to_emails) +
                  ", message=" + safe_truncate(message, 100));
        std::string cmd = build_email_command(message, level_to_string(level), subject, to_emails, alert_type);
        execute_command(cmd, async);
    }

    /**
     * @brief 发送飞书告警
     * @param to_emails 私信收件人飞书邮箱（逗号分隔），为空则只走 webhook 群通知
     */
    void send_lark_alert(const std::string& message, AlertLevel level = AlertLevel::WARNING,
                         const std::string& title = "", bool async = true,
                         const std::string& to_emails = "") {
        if (!config_.lark_enabled) {
            alert_log("[飞书通知] 飞书告警已禁用，跳过发送");
            return;
        }
        alert_log("[飞书通知] 发送飞书告警: level=" + level_to_string(level) +
                  ", title=" + (title.empty() ? "(默认)" : title) +
                  ", to_emails=" + (to_emails.empty() ? "(仅群通知)" : to_emails) +
                  ", message=" + safe_truncate(message, 100));
        std::string cmd = build_command("lark_alert.py", message, level_to_string(level));
        if (!title.empty()) {
            cmd += " --title \"" + escape_string(title) + "\"";
        }
        if (!config_.lark_config_file.empty()) {
            cmd += " -c \"" + config_.lark_config_file + "\"";
        }
        if (!to_emails.empty()) {
            cmd += " --to-emails \"" + escape_string(to_emails) + "\"";
        }
        execute_command(cmd, async);
    }

    /**
     * @brief 发送告警到所有渠道
     */
    void send_alert_all(const std::string& message, AlertLevel level = AlertLevel::WARNING,
                        const std::string& title = "") {
        send_lark_alert(message, level, title, true);
        send_email_alert(message, level, title, "", "default", true);
    }

    /**
     * @brief 设置配置
     */
    void set_config(const AlertConfig& config) {
        config_ = config;
    }

    /**
     * @brief 写入告警专用日志（source="alert"，自动按天生成 alert_YYYYMMDD.log）
     */
    void alert_log(const std::string& msg) {
        core::Logger::instance().info("alert", msg);
    }

    void alert_log_error(const std::string& msg) {
        core::Logger::instance().error("alert", msg);
    }

    /**
     * @brief UTF-8 安全截断：不会把多字节字符从中间截断
     */
    static std::string safe_truncate(const std::string& str, size_t max_bytes) {
        if (str.size() <= max_bytes) return str;
        size_t pos = max_bytes;
        // 回退到 UTF-8 字符边界
        while (pos > 0 && (static_cast<unsigned char>(str[pos]) & 0xC0) == 0x80) {
            --pos;
        }
        return str.substr(0, pos) + "...";
    }

private:
    AlertConfig config_;

    std::string level_to_string(AlertLevel level) {
        switch (level) {
            case AlertLevel::INFO: return "info";
            case AlertLevel::WARNING: return "warning";
            case AlertLevel::CRITICAL: return "critical";
            default: return "warning";
        }
    }

    std::string escape_string(const std::string& str) {
        std::string result;
        for (char c : str) {
            if (c == '"' || c == '\\' || c == '$' || c == '`') {
                result += '\\';
            }
            result += c;
        }
        return result;
    }

    std::string build_command(const std::string& script, const std::string& message,
                              const std::string& level) {
        return config_.python_path + " " +
               config_.alerts_path + "/" + script +
               " -m \"" + escape_string(message) + "\"" +
               " -l " + level;
    }

    std::string build_email_command(const std::string& message, const std::string& level,
                                    const std::string& subject, const std::string& to_emails,
                                    const std::string& alert_type = "default") {
        std::string cmd = config_.python_path + " " +
                          config_.alerts_path + "/email_alert.py" +
                          " -m \"" + escape_string(message) + "\"" +
                          " -l " + level;

        // 添加配置文件
        if (!config_.email_config_file.empty()) {
            cmd += " -c \"" + config_.email_config_file + "\"";
        }

        // 添加主题
        if (!subject.empty()) {
            cmd += " -s \"" + escape_string(subject) + "\"";
        }

        // 添加收件人（覆盖配置文件）
        if (!to_emails.empty()) {
            cmd += " --to \"" + escape_string(to_emails) + "\"";
        }

        // 添加告警类型
        if (!alert_type.empty() && alert_type != "default") {
            cmd += " -t \"" + escape_string(alert_type) + "\"";
        }

        return cmd;
    }

    void send_alert(const std::string& script, const std::string& message,
                    const std::string& level, bool async) {
        std::string cmd = build_command(script, message, level);
        execute_command(cmd, async);
    }

    void execute_command(const std::string& cmd, bool async) {
        if (async) {
            // 异步执行，不阻塞主线程
            std::thread([this, cmd]() {
                try {
                    std::string full_cmd = cmd + " 2>&1";
                    FILE* pipe = popen(full_cmd.c_str(), "r");
                    if (pipe) {
                        char buffer[256];
                        std::string output;
                        while (fgets(buffer, sizeof(buffer), pipe)) {
                            output += buffer;
                        }
                        int ret = pclose(pipe);
                        if (ret != 0) {
                            std::string err_msg = "[告警脚本] 执行失败(code=" + std::to_string(ret) + "): " + output;
                            core::Logger::instance().error(err_msg);
                            alert_log_error(err_msg);
                        } else {
                            alert_log("[告警脚本] 执行成功: " + (output.empty() ? "(无输出)" : safe_truncate(output, 200)));
                        }
                    } else {
                        std::string err_msg = "[告警脚本] popen 失败: " + safe_truncate(cmd, 100);
                        core::Logger::instance().error(err_msg);
                        alert_log_error(err_msg);
                    }
                } catch (const std::exception& e) {
                    core::Logger::instance().error(std::string("[告警脚本] 异步执行异常: ") + e.what());
                } catch (...) {
                    core::Logger::instance().error("[告警脚本] 异步执行未知异常");
                }
            }).detach();
        } else {
            // 同步执行
            std::string full_cmd = cmd + " 2>&1";
            FILE* pipe = popen(full_cmd.c_str(), "r");
            if (pipe) {
                char buffer[256];
                std::string output;
                while (fgets(buffer, sizeof(buffer), pipe)) {
                    output += buffer;
                }
                int ret = pclose(pipe);
                if (ret != 0) {
                    std::string err_msg = "[告警脚本] 执行失败(code=" + std::to_string(ret) + "): " + output;
                    core::Logger::instance().error(err_msg);
                    alert_log_error(err_msg);
                } else {
                    alert_log("[告警脚本] 执行成功: " + (output.empty() ? "(无输出)" : safe_truncate(output, 200)));
                }
            } else {
                std::string err_msg = "[告警脚本] popen 失败: " + safe_truncate(cmd, 100);
                core::Logger::instance().error(err_msg);
                alert_log_error(err_msg);
            }
        }
    }
};

/**
 * @brief 风险限制配置
 */
struct RiskLimits {
    // 单笔订单限制
    double max_order_value = 10000.0;        // 单笔最大金额 (USDT)
    double max_order_quantity = 100.0;       // 单笔最大数量

    // 持仓限制
    double max_position_value = 50000.0;     // 单品种最大持仓 (USDT)
    double max_total_exposure = 100000.0;    // 总敞口限制 (USDT)
    int max_open_orders = 50;                // 最大挂单数

    // 风险控制
    double max_drawdown_pct = 0.10;          // 最大回撤百分比 (10%)
    double daily_loss_limit = 5000.0;        // 每日最大亏损 (USDT)
    std::string drawdown_mode = "daily_peak"; // 回撤模式: daily_peak(当日峰值回撤) 或 daily_initial(当日初值回撤)

    // 频率限制
    int max_orders_per_second = 10;          // 每秒最大订单数
    int max_orders_per_minute = 100;         // 每分钟最大订单数

    /**
     * @brief 从 JSON 对象加载配置
     */
    static RiskLimits from_json(const nlohmann::json& j) {
        RiskLimits limits;
        if (j.contains("max_order_value")) limits.max_order_value = j["max_order_value"];
        if (j.contains("max_order_quantity")) limits.max_order_quantity = j["max_order_quantity"];
        if (j.contains("max_position_value")) limits.max_position_value = j["max_position_value"];
        if (j.contains("max_total_exposure")) limits.max_total_exposure = j["max_total_exposure"];
        if (j.contains("max_open_orders")) limits.max_open_orders = j["max_open_orders"];
        if (j.contains("max_drawdown_pct")) limits.max_drawdown_pct = j["max_drawdown_pct"];
        if (j.contains("daily_loss_limit")) limits.daily_loss_limit = j["daily_loss_limit"];
        if (j.contains("max_orders_per_second")) limits.max_orders_per_second = j["max_orders_per_second"];
        if (j.contains("max_orders_per_minute")) limits.max_orders_per_minute = j["max_orders_per_minute"];
        if (j.contains("drawdown_mode")) limits.drawdown_mode = j["drawdown_mode"];
        return limits;
    }

    /**
     * @brief 从 JSON 文件加载配置
     */
    static RiskLimits from_file(const std::string& config_file) {
        try {
            std::ifstream file(config_file);
            if (!file.is_open()) {
                std::cerr << "[风控] 无法打开配置文件: " << config_file << "，使用默认配置\n";
                return RiskLimits();
            }

            nlohmann::json config;
            file >> config;

            if (config.contains("risk_limits")) {
                auto limits = from_json(config["risk_limits"]);
                std::cout << "[风控] ✓ 已加载配置文件: " << config_file << "\n";
                std::cout << "[风控] 配置: max_order_value=" << limits.max_order_value
                          << ", max_position_value=" << limits.max_position_value
                          << ", daily_loss_limit=" << limits.daily_loss_limit
                          << ", drawdown_mode=" << limits.drawdown_mode << "\n";
                return limits;
            } else {
                std::cerr << "[风控] 配置文件格式错误，缺少 'risk_limits' 字段\n";
                return RiskLimits();
            }
        } catch (const std::exception& e) {
            std::cerr << "[风控] 加载配置文件失败: " << e.what() << "，使用默认配置\n";
            return RiskLimits();
        }
    }
};

/**
 * @brief 风险检查结果
 */
struct RiskCheckResult {
    bool passed = true;
    std::string reason;

    static RiskCheckResult ok() {
        return {true, ""};
    }

    static RiskCheckResult reject(const std::string& msg) {
        return {false, msg};
    }
};

/**
 * @brief 风险管理器
 */
class RiskManager {
public:
    RiskManager(const RiskLimits& limits = RiskLimits(),
                const AlertConfig& alert_config = AlertConfig())
        : limits_(limits), kill_switch_(false), alert_service_(alert_config) {}

    /**
     * @brief 订单前置风险检查
     */
    RiskCheckResult check_order(const std::string& symbol,
                                 OrderSide side,
                                 double price,
                                 double quantity) {
        // 计算订单金额
        double order_value = price * quantity;
        return check_order_with_value(symbol, side, price, quantity, order_value);
    }

    /**
     * @brief 订单前置风险检查（指定订单金额，避免OKX张数/Binance币数计算问题）
     * @param strategy_id 策略ID，用于风控告警时发送到对应策略邮箱
     */
    RiskCheckResult check_order_with_value(const std::string& symbol,
                                            OrderSide side,
                                            double price,
                                            double quantity,
                                            double order_value,
                                            const std::string& strategy_id = "") {
        std::lock_guard<std::mutex> lock(mutex_);

        // Kill Switch 检查
        if (kill_switch_) {
            return RiskCheckResult::reject("Kill switch activated");
        }

        // 订单金额检查（使用传入的 order_value）
        if (order_value > limits_.max_order_value) {
            std::string reason = "Order value " + std::to_string(order_value) +
                                " exceeds limit " + std::to_string(limits_.max_order_value);
            // 发送风控告警到策略邮箱
            send_risk_alert_to_strategy(strategy_id, reason, "订单金额超限");
            return RiskCheckResult::reject(reason);
        }

        // 订单数量检查
        if (quantity > limits_.max_order_quantity) {
            std::string reason = "Order quantity " + std::to_string(quantity) +
                                " exceeds limit " + std::to_string(limits_.max_order_quantity);
            send_risk_alert_to_strategy(strategy_id, reason, "订单数量超限");
            return RiskCheckResult::reject(reason);
        }

        // 挂单数量检查
        if (open_order_count_ >= limits_.max_open_orders) {
            std::string reason = "Open orders " + std::to_string(open_order_count_) +
                                " exceeds limit " + std::to_string(limits_.max_open_orders);
            send_risk_alert_to_strategy(strategy_id, reason, "挂单数量超限");
            return RiskCheckResult::reject(reason);
        }

        // 持仓限制检查
        double current_position = get_position_value(symbol);
        double new_position = current_position + (side == OrderSide::BUY ? order_value : -order_value);

        if (std::abs(new_position) > limits_.max_position_value) {
            std::string reason = "Position value would exceed limit for " + symbol;
            send_risk_alert_to_strategy(strategy_id, reason, "持仓限制超限");
            return RiskCheckResult::reject(reason);
        }

        // 总敞口检查
        double total_exposure = calculate_total_exposure();
        if (total_exposure + order_value > limits_.max_total_exposure) {
            std::string reason = "Total exposure would exceed limit";
            send_risk_alert_to_strategy(strategy_id, reason, "总敞口超限");
            return RiskCheckResult::reject(reason);
        }

        // 单日亏损检查
        if (daily_pnl_ < -limits_.daily_loss_limit) {
            std::string reason = "Daily loss limit reached: " + std::to_string(daily_pnl_);
            send_risk_alert_to_strategy(strategy_id, reason, "单日亏损超限");
            return RiskCheckResult::reject(reason);
        }

        // 频率限制检查
        if (!check_rate_limit()) {
            std::string reason = "Order rate limit exceeded";
            send_risk_alert_to_strategy(strategy_id, reason, "订单频率超限");
            return RiskCheckResult::reject(reason);
        }

        return RiskCheckResult::ok();
    }

    /**
     * @brief 更新持仓
     */
    void update_position(const std::string& symbol, double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        position_values_[symbol] = value;
    }

    /**
     * @brief 更新挂单数量
     */
    void set_open_order_count(int count) {
        std::lock_guard<std::mutex> lock(mutex_);
        open_order_count_ = count;
    }

    /**
     * @brief 更新每日盈亏（仅用于统计，不触发Kill Switch）
     */
    void update_daily_pnl(double pnl) {
        std::lock_guard<std::mutex> lock(mutex_);
        daily_pnl_ = pnl;
        // 注意：这里不检查回撤，因为 pnl 可能是未实现盈亏（负数）
        // 回撤检查应该使用账户总权益，见 update_account_equity()
    }

    /**
     * @brief 更新账户总权益（用于回撤检查）
     * @param equity 账户总权益（USDT）
     * @param strategy_id 策略ID，用于风控告警时发送到对应策略邮箱
     */
    void update_account_equity(double equity, const std::string& strategy_id = "") {
        std::lock_guard<std::mutex> lock(mutex_);

        if (strategy_id.empty()) {
            return;  // 必须提供策略ID
        }

        // 获取当前日期
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm;
        localtime_r(&now_time_t, &now_tm);
        int current_date = now_tm.tm_year * 10000 + (now_tm.tm_mon + 1) * 100 + now_tm.tm_mday;

        // 获取该策略的数据
        double& peak_pnl = strategy_peak_pnl_[strategy_id];
        double& initial_equity = strategy_initial_equity_[strategy_id];
        int& last_reset_date = strategy_last_reset_date_[strategy_id];

        // 检查是否需要每日重置
        if (last_reset_date != current_date) {
            // 新的一天，重置峰值和初值
            peak_pnl = equity;
            initial_equity = equity;
            last_reset_date = current_date;
            std::cout << "[风控] [" << strategy_id << "] 每日重置: 日期=" << current_date
                      << ", 初始权益=" << equity << " USDT"
                      << ", 回撤模式=" << limits_.drawdown_mode << "\n";
            return;
        }

        // 初始化（第一次调用时）
        if (peak_pnl == 0.0) {
            peak_pnl = equity;
            initial_equity = equity;
            last_reset_date = current_date;
            std::cout << "[风控] [" << strategy_id << "] 初始化账户权益: " << equity << " USDT"
                      << ", 回撤模式=" << limits_.drawdown_mode << "\n";
            return;
        }

        // 根据回撤模式计算回撤
        double drawdown_pct = 0.0;
        if (limits_.drawdown_mode == "daily_initial") {
            // 模式1: 当日初值回撤
            if (initial_equity > 0) {
                drawdown_pct = (initial_equity - equity) / initial_equity;
            }
        } else {
            // 模式2: 当日峰值回撤（默认）
            drawdown_pct = (peak_pnl - equity) / peak_pnl;
        }

        // 检查最大回撤（仅告警，不触发kill switch）
        if (drawdown_pct > limits_.max_drawdown_pct) {
            std::string reason = "[" + strategy_id + "][" + limits_.drawdown_mode + "] 峰值=" +
                                std::to_string(peak_pnl) + " USDT, 初值=" +
                                std::to_string(initial_equity) + " USDT, 当前=" +
                                std::to_string(equity) + " USDT, 回撤=" +
                                std::to_string(drawdown_pct * 100) + "% (限制=" +
                                std::to_string(limits_.max_drawdown_pct * 100) + "%)";

            std::cout << "[风控] ⚠️  回撤超限 " << reason << "\n";

            // 只发送告警邮件，不激活kill switch
            std::string alert_message = "回撤超限警告: " + reason;
            alert_service_.alert_log("[回撤超限] " + reason);
            send_risk_alert_to_strategy(strategy_id, alert_message, "回撤超限告警");

            // 同时发送到所有告警渠道（但不激活kill switch）
            alert_service_.send_alert_all(
                alert_message,
                AlertLevel::CRITICAL,
                "回撤超限告警"
            );
        }

        // 更新峰值（仅在 daily_peak 模式下有意义，但两种模式都更新以便切换）
        if (equity > peak_pnl) {
            peak_pnl = equity;
        }
    }

    /**
     * @brief 激活紧急止损
     */
    void activate_kill_switch(const std::string& reason) {
        kill_switch_ = true;
        std::cout << "[风控] KILL SWITCH ACTIVATED: " << reason << "\n";
        alert_service_.alert_log("[KILL SWITCH] 已激活: " + reason);

        // 发送严重告警到所有渠道
        alert_service_.send_alert_all(
            "KILL SWITCH 已激活: " + reason,
            AlertLevel::CRITICAL,
            "紧急止损触发"
        );
    }

    /**
     * @brief 解除紧急止损
     */
    void deactivate_kill_switch() {
        kill_switch_ = false;
        std::cout << "[风控] Kill switch deactivated\n";
        alert_service_.alert_log("[KILL SWITCH] 已解除");

        // 发送通知
        alert_service_.send_lark_alert(
            "Kill Switch 已解除",
            AlertLevel::INFO,
            "风控状态恢复"
        );
    }

    /**
     * @brief 检查是否被止损
     */
    bool is_kill_switch_active() const {
        return kill_switch_;
    }

    /**
     * @brief 获取风险统计
     */
    nlohmann::json get_risk_stats() const {
        std::lock_guard<std::mutex> lock(mutex_);

        // 汇总所有策略的峰值（用于统计）
        nlohmann::json strategy_stats = nlohmann::json::object();
        for (const auto& [strategy_id, peak] : strategy_peak_pnl_) {
            strategy_stats[strategy_id] = {
                {"peak_pnl", peak},
                {"initial_equity", strategy_initial_equity_.count(strategy_id) ? strategy_initial_equity_.at(strategy_id) : 0.0}
            };
        }

        return {
            {"kill_switch", kill_switch_.load()},
            {"open_orders", open_order_count_},
            {"daily_pnl", daily_pnl_},
            {"strategy_stats", strategy_stats},
            {"total_exposure", calculate_total_exposure()},
            {"position_count", position_values_.size()}
        };
    }

    /**
     * @brief 获取告警服务（用于手动发送告警）
     */
    AlertService& get_alert_service() {
        return alert_service_;
    }

    /**
     * @brief 发送自定义告警
     */
    void send_alert(const std::string& message, AlertLevel level = AlertLevel::WARNING,
                    const std::string& title = "") {
        alert_service_.send_alert_all(message, level, title);
    }

    /**
     * @brief 设置风控限制
     */
    void set_limits(const RiskLimits& limits) {
        std::lock_guard<std::mutex> lock(mutex_);
        limits_ = limits;
    }

    /**
     * @brief 获取风控限制
     */
    RiskLimits get_limits() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return limits_;
    }

    /**
     * @brief 注册策略邮箱（从策略配置文件读取）
     * @param strategy_id 策略ID
     * @param email 策略负责人邮箱
     */
    void register_strategy_email(const std::string& strategy_id, const std::string& email) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!email.empty() && email.find("@") != std::string::npos) {
            strategy_emails_[strategy_id] = email;
            core::Logger::instance().info(strategy_id, "[风控] 已注册策略邮箱: " + strategy_id + " -> " + email);
        }
    }

    /**
     * @brief 注册策略飞书邮箱（用于 Open API 私信）
     */
    void register_strategy_lark_email(const std::string& strategy_id, const std::string& lark_email) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!lark_email.empty() && lark_email.find("@") != std::string::npos) {
            strategy_lark_emails_[strategy_id] = lark_email;
            core::Logger::instance().info(strategy_id, "[风控] 已注册策略飞书邮箱: " + strategy_id + " -> " + lark_email);
        }
    }

    /**
     * @brief 从策略配置文件加载联系人（邮箱 + 飞书）
     * @param config_file 策略配置文件路径
     */
    void load_strategy_email_from_config(const std::string& config_file) {
        try {
            std::ifstream file(config_file);
            if (!file.is_open()) {
                return;
            }

            nlohmann::json config;
            file >> config;

            if (config.contains("strategy_id") && config.contains("contacts")) {
                std::string strategy_id = config["strategy_id"];
                auto contacts = config["contacts"];

                if (contacts.is_array() && !contacts.empty()) {
                    for (const auto& contact : contacts) {
                        if (contact.contains("email")) {
                            std::string email = contact["email"];
                            register_strategy_email(strategy_id, email);
                        }
                        if (contact.contains("lark_email")) {
                            std::string lark_email = contact["lark_email"];
                            register_strategy_lark_email(strategy_id, lark_email);
                        }
                        break;  // 只取第一个联系人
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[风控] 加载策略联系人失败: " << e.what() << "\n";
        }
    }

    /**
     * @brief 检查账户余额风险
     */
    RiskCheckResult check_account_balance(double balance, double min_balance = 100.0) {
        if (balance < min_balance) {
            return RiskCheckResult::reject(
                "Account balance " + std::to_string(balance) +
                " below minimum " + std::to_string(min_balance)
            );
        }
        return RiskCheckResult::ok();
    }

    /**
     * @brief 批量订单风控检查
     */
    std::vector<RiskCheckResult> check_batch_orders(
        const std::vector<std::tuple<std::string, OrderSide, double, double>>& orders) {
        std::vector<RiskCheckResult> results;
        for (const auto& [symbol, side, price, quantity] : orders) {
            results.push_back(check_order(symbol, side, price, quantity));
        }
        return results;
    }

    /**
     * @brief 重置每日统计（每天开盘时调用）
     */
    void reset_daily_stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        daily_pnl_ = 0.0;
        // 注意：不再重置 peak_pnl_，因为现在由 update_account_equity() 自动检测日期变化并重置
        std::cout << "[风控] 每日统计已重置\n";
    }

    /**
     * @brief 记录订单执行（用于频率统计）
     */
    void record_order_execution() {
        std::lock_guard<std::mutex> lock(mutex_);
        order_timestamps_.push_back(std::chrono::steady_clock::now());
    }

    /**
     * @brief 获取当前订单频率（每秒）
     */
    int get_current_order_rate() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        int count = 0;
        for (auto it = order_timestamps_.rbegin(); it != order_timestamps_.rend(); ++it) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - *it).count() < 1) {
                count++;
            } else {
                break;
            }
        }
        return count;
    }

public:
    /**
     * @brief 发送风控告警到策略邮箱
     * @param strategy_id 策略ID
     * @param message 告警消息
     * @param title 告警标题
     */
    void send_risk_alert_to_strategy(const std::string& strategy_id,
                                      const std::string& message,
                                      const std::string& title) {
        if (strategy_id.empty()) {
            return;
        }

        std::string full_message = "[策略: " + strategy_id + "] " + message;
        std::string full_title = "[风控告警] " + title;

        // 写入告警日志
        alert_service_.alert_log("[风控告警] 策略=" + strategy_id + ", 标题=" + title + ", 内容=" + AlertService::safe_truncate(message, 200));

        // 发送邮件告警
        auto it = strategy_emails_.find(strategy_id);
        if (it != strategy_emails_.end() && !it->second.empty()) {
            core::Logger::instance().info(strategy_id, "[邮件通知] 向 " + it->second + " 发送告警: " + title);
            alert_service_.alert_log("[邮件通知] 策略=" + strategy_id + ", 收件人=" + it->second + ", 标题=" + title);
            alert_service_.send_email_alert(
                full_message,
                AlertLevel::CRITICAL,
                full_title,
                it->second,
                "risk_" + strategy_id,
                true
            );
        } else {
            core::Logger::instance().warn(strategy_id, "[邮件通知] 策略 " + strategy_id + " 未注册邮箱，跳过邮件通知");
            alert_service_.alert_log("[邮件通知] 策略 " + strategy_id + " 未注册邮箱，跳过邮件通知");
        }

        // 发送飞书告警（群通知 + 私信）
        std::string lark_email;
        auto lark_it = strategy_lark_emails_.find(strategy_id);
        if (lark_it != strategy_lark_emails_.end()) {
            lark_email = lark_it->second;
            core::Logger::instance().info(strategy_id, "[飞书通知] 向 " + lark_email + " 发送告警: " + title);
            alert_service_.alert_log("[飞书通知] 策略=" + strategy_id + ", 收件人=" + lark_email + ", 标题=" + title);
        } else {
            core::Logger::instance().info(strategy_id, "[飞书通知] 发送群通知告警: " + title);
            alert_service_.alert_log("[飞书通知] 策略=" + strategy_id + ", 群通知, 标题=" + title);
        }
        alert_service_.send_lark_alert(
            full_message,
            AlertLevel::CRITICAL,
            full_title,
            true,
            lark_email
        );
    }

    /**
     * @brief 激活紧急止损（带策略ID）
     */
    void activate_kill_switch_with_strategy(const std::string& reason, const std::string& strategy_id) {
        kill_switch_ = true;
        core::Logger::instance().error(strategy_id, "[风控] KILL SWITCH ACTIVATED: " + reason);
        alert_service_.alert_log("[KILL SWITCH] 策略=" + strategy_id + ", 已激活: " + reason);

        // 发送到策略邮箱
        send_risk_alert_to_strategy(strategy_id, "KILL SWITCH 已激活: " + reason, "紧急止损触发");

        // 发送严重告警到所有渠道
        alert_service_.send_alert_all(
            "KILL SWITCH 已激活: " + reason,
            AlertLevel::CRITICAL,
            "紧急止损触发"
        );
    }


    double get_position_value(const std::string& symbol) const {
        auto it = position_values_.find(symbol);
        return it != position_values_.end() ? it->second : 0.0;
    }

    double calculate_total_exposure() const {
        double total = 0.0;
        for (const auto& [symbol, value] : position_values_) {
            total += std::abs(value);
        }
        return total;
    }

    bool check_rate_limit() {
        auto now = std::chrono::steady_clock::now();

        // 清理过期记录
        while (!order_timestamps_.empty() &&
               std::chrono::duration_cast<std::chrono::seconds>(
                   now - order_timestamps_.front()).count() > 60) {
            order_timestamps_.pop_front();
        }

        // 检查限制
        int count_last_second = 0;
        for (auto it = order_timestamps_.rbegin(); it != order_timestamps_.rend(); ++it) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - *it).count() < 1) {
                count_last_second++;
            }
        }

        if (count_last_second >= limits_.max_orders_per_second) {
            return false;
        }

        if (order_timestamps_.size() >= static_cast<size_t>(limits_.max_orders_per_minute)) {
            return false;
        }

        // 不在这里记录时间戳，由 record_order_execution() 负责
        return true;
    }

private:
    RiskLimits limits_;
    mutable std::mutex mutex_;

    std::atomic<bool> kill_switch_;
    std::map<std::string, double> position_values_;
    std::map<std::string, std::string> strategy_emails_;  // 策略ID -> 邮箱映射
    std::map<std::string, std::string> strategy_lark_emails_;  // 策略ID -> 飞书邮箱映射

    // 每个策略独立的回撤追踪数据
    std::map<std::string, double> strategy_peak_pnl_;           // 策略ID -> 峰值权益
    std::map<std::string, double> strategy_initial_equity_;     // 策略ID -> 当日初始权益
    std::map<std::string, int> strategy_last_reset_date_;       // 策略ID -> 上次重置日期(YYYYMMDD)

    int open_order_count_ = 0;
    double daily_pnl_ = 0.0;

    std::deque<std::chrono::steady_clock::time_point> order_timestamps_;

    AlertService alert_service_;  // 告警服务
};

} // namespace trading
