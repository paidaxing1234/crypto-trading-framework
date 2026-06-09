#pragma once
/**
 * @file symbol_delist_monitor.h
 * @brief 合约下架监控 - 定时轮询 Binance exchangeInfo，检测即将下架/已下架的合约
 *
 * 功能：
 * 1. 每 30 秒轮询 GET /fapi/v1/exchangeInfo
 * 2. 检测 PERPETUAL 合约 status 从 TRADING 变为其他状态（SETTLING 等）
 * 3. 检测 deliveryDate 从默认值(2100年)变更为实际下架日期
 * 4. 发送邮件 + 飞书通知给超级管理员和策略管理员
 */

#include <string>
#include <set>
#include <map>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <fstream>
#include <nlohmann/json.hpp>

// 简单的 HTTP GET（使用 curl 命令，避免引入额外 HTTP 库）
namespace detail {
inline std::string http_get(const std::string& url, int timeout_sec = 60) {
    std::string cmd = "curl -s --connect-timeout 10"
                    " --max-time " + std::to_string(timeout_sec)
                    + " \"" + url + "\" 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    std::string result;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}
} // namespace detail

class SymbolDelistMonitor {
public:
    struct Config {
        std::string base_url = "https://fapi.binance.com";
        int poll_interval_sec = 30;
        std::string email_config_file;  // email_alert_network.json 路径
        std::string lark_config_file;   // lark_config.json 路径
        std::string alerts_script_dir;  // email_alert.py / lark_alert.py 所在目录
        std::string user_config_dir;    // user_configs 目录
        std::string state_file;         // 持久化已通知记录的 JSON 文件路径
        std::vector<std::string> to_emails;  // 默认收件人邮箱
        int64_t default_delivery_date = 4133404800000;  // 2100-12-25，永续合约默认值
    };

    explicit SymbolDelistMonitor(const Config& config)
        : config_(config), running_(false) {}

    ~SymbolDelistMonitor() {
        stop();
    }

    void start() {
        if (running_.load()) return;
        load_state();
        running_.store(true);
        thread_ = std::thread(&SymbolDelistMonitor::monitor_loop, this);
    }

    void stop() {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    Config config_;
    std::atomic<bool> running_;
    std::thread thread_;

    std::set<std::string> known_trading_symbols_;
    std::map<std::string, int64_t> known_delivery_dates_;
    std::set<std::string> notified_delisted_;
    std::set<std::string> notified_upcoming_;
    std::mutex mutex_;
    bool first_poll_ = true;

    void monitor_loop() {
        std::cout << "[下架监控] 监控线程已启动，轮询间隔: "
                  << config_.poll_interval_sec << "秒\n";

        while (running_.load()) {
            try {
                poll_exchange_info();
            } catch (const std::exception& e) {
                std::cerr << "[下架监控] 轮询异常: " << e.what() << "\n";
            }

            // 分段 sleep，方便快速退出
            for (int i = 0; i < config_.poll_interval_sec * 10 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        std::cout << "[下架监控] 监控线程已退出\n";
    }

    void poll_exchange_info() {
        std::string url = config_.base_url + "/fapi/v1/exchangeInfo";
        std::string response = detail::http_get(url);

        if (response.empty()) {
            std::cerr << "[下架监控] 请求 exchangeInfo 失败（空响应）\n";
            return;
        }

        nlohmann::json data;
        try {
            data = nlohmann::json::parse(response);
        } catch (...) {
            std::cerr << "[下架监控] 解析 exchangeInfo JSON 失败\n";
            return;
        }

        if (!data.contains("symbols") || !data["symbols"].is_array()) {
            return;
        }

        std::set<std::string> current_trading;
        std::map<std::string, int64_t> current_delivery_dates;
        std::map<std::string, std::string> current_status_map;

        for (const auto& sym : data["symbols"]) {
            std::string symbol = sym.value("symbol", "");
            std::string status = sym.value("status", "");
            std::string contract_type = sym.value("contractType", "");
            int64_t delivery_date = sym.value("deliveryDate", (int64_t)0);

            if (contract_type != "PERPETUAL" || symbol.empty()) continue;

            current_status_map[symbol] = status;
            current_delivery_dates[symbol] = delivery_date;

            if (status == "TRADING") {
                current_trading.insert(symbol);
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (first_poll_) {
            known_trading_symbols_ = current_trading;
            known_delivery_dates_ = current_delivery_dates;
            first_poll_ = false;
            std::cout << "[下架监控] 初始化完成: " << current_trading.size()
                      << " 个 TRADING 永续合约\n";

            // 首次初始化时就检查：是否有"未通知过"的合约 deliveryDate 不是默认值
            // 关键: 跳过 notified_upcoming_ 中已存在的 (来自 load_state 的持久化记录)
            // 避免每次重启 trading_server 都重复推送相同的下架预告
            std::vector<std::string> already_upcoming;
            for (const auto& symbol : current_trading) {
                int64_t dd = current_delivery_dates.count(symbol)
                    ? current_delivery_dates[symbol] : 0;
                if (dd != config_.default_delivery_date && dd > 0
                    && notified_upcoming_.find(symbol) == notified_upcoming_.end()) {
                    char time_buf[64];
                    time_t t = dd / 1000;
                    std::strftime(time_buf, sizeof(time_buf),
                                  "%Y-%m-%d %H:%M:%S", std::localtime(&t));
                    already_upcoming.push_back(
                        symbol + " (预计下架: " + std::string(time_buf) + ")");
                    notified_upcoming_.insert(symbol);
                }
            }
            if (!already_upcoming.empty()) {
                std::stringstream msg;
                msg << "合约下架预告（启动时检测）！\\n\\n"
                    << "以下 " << already_upcoming.size()
                    << " 个永续合约 deliveryDate 已设置（预告下架）：\\n\\n";
                for (const auto& s : already_upcoming) {
                    msg << "  - " << s << "\\n";
                }
                msg << "\\n请提前做好仓位调整准备。\\n"
                    << "时间: " << get_current_time_str();

                std::string subject = "[下架预告] " + std::to_string(already_upcoming.size())
                                    + " 个合约即将下架（启动检测）";

                std::cout << "[下架监控] ⚠️  启动时检测到 " << already_upcoming.size()
                          << " 个合约预告下架:\n";
                for (const auto& s : already_upcoming) {
                    std::cout << "[下架监控]   - " << s << "\n";
                }
                save_state();
                send_notification(msg.str(), "warning", subject);
            }
            return;
        }

        // 检测 1：status 从 TRADING 变为非 TRADING（已下架）
        std::vector<std::string> newly_delisted;
        for (const auto& symbol : known_trading_symbols_) {
            if (current_trading.find(symbol) == current_trading.end()
                && notified_delisted_.find(symbol) == notified_delisted_.end()) {
                std::string new_status = current_status_map.count(symbol)
                    ? current_status_map[symbol] : "UNKNOWN";
                newly_delisted.push_back(symbol + " (" + new_status + ")");
                notified_delisted_.insert(symbol);
            }
        }

        // 检测 2：deliveryDate 从默认值变为实际日期（预告下架，仍在 TRADING）
        std::vector<std::string> upcoming_delist;
        for (const auto& symbol : current_trading) {
            int64_t dd = current_delivery_dates.count(symbol)
                ? current_delivery_dates[symbol] : 0;
            int64_t old_dd = known_delivery_dates_.count(symbol)
                ? known_delivery_dates_[symbol] : 0;

            if (dd != config_.default_delivery_date && dd > 0
                && old_dd == config_.default_delivery_date
                && notified_upcoming_.find(symbol) == notified_upcoming_.end()) {

                char time_buf[64];
                time_t t = dd / 1000;
                std::strftime(time_buf, sizeof(time_buf),
                              "%Y-%m-%d %H:%M:%S", std::localtime(&t));

                upcoming_delist.push_back(
                    symbol + " (预计下架: " + time_buf + ")");
                notified_upcoming_.insert(symbol);
            }
        }

        // 更新基线
        known_trading_symbols_ = current_trading;
        known_delivery_dates_ = current_delivery_dates;

        // 有新通知时持久化状态
        if (!newly_delisted.empty() || !upcoming_delist.empty()) {
            save_state();
        }

        // 发送通知
        if (!newly_delisted.empty()) {
            std::stringstream msg;
            msg << "合约下架告警！\\n\\n"
                << "以下 " << newly_delisted.size()
                << " 个永续合约已变更为非TRADING状态：\\n\\n";
            for (const auto& s : newly_delisted) {
                msg << "  - " << s << "\\n";
            }
            msg << "\\n请检查策略持仓，确认是否有受影响的仓位需要处理。\\n"
                << "时间: " << get_current_time_str();

            std::string subject = "[下架告警] " + std::to_string(newly_delisted.size())
                                + " 个合约已下架";

            std::cout << "[下架监控] ⚠️  检测到 " << newly_delisted.size()
                      << " 个合约下架:\n";
            for (const auto& s : newly_delisted) {
                std::cout << "[下架监控]   - " << s << "\n";
            }

            send_notification(msg.str(), "critical", subject);
        }

        if (!upcoming_delist.empty()) {
            std::stringstream msg;
            msg << "合约下架预告！\\n\\n"
                << "以下 " << upcoming_delist.size()
                << " 个永续合约 deliveryDate 已更新（预告下架）：\\n\\n";
            for (const auto& s : upcoming_delist) {
                msg << "  - " << s << "\\n";
            }
            msg << "\\n请提前做好仓位调整准备。\\n"
                << "时间: " << get_current_time_str();

            std::string subject = "[下架预告] " + std::to_string(upcoming_delist.size())
                                + " 个合约即将下架";

            std::cout << "[下架监控] ⚠️  检测到 " << upcoming_delist.size()
                      << " 个合约预告下架:\n";
            for (const auto& s : upcoming_delist) {
                std::cout << "[下架监控]   - " << s << "\n";
            }

            send_notification(msg.str(), "warning", subject);
        }
    }

    void send_notification(const std::string& message, const std::string& level,
                           const std::string& subject) {
        auto emails = collect_notification_emails();
        std::string to_str;
        for (const auto& email : emails) {
            if (!to_str.empty()) to_str += ",";
            to_str += email;
        }

        // 发送邮件
        if (!config_.email_config_file.empty() && !to_str.empty()) {
            std::string cmd = "python3 " + config_.alerts_script_dir + "/email_alert.py"
                            + " -m \"" + message + "\""
                            + " -l " + level
                            + " -s \"" + escape_shell(subject) + "\""
                            + " -c \"" + config_.email_config_file + "\""
                            + " --to \"" + to_str + "\"";

            std::thread([cmd]() {
                int ret = std::system(cmd.c_str());
                if (ret != 0) {
                    std::cerr << "[下架监控] 邮件发送失败, 返回码: " << ret << "\n";
                } else {
                    std::cout << "[下架监控] ✓ 邮件已发送\n";
                }
            }).detach();
        }

        // 发送飞书通知
        if (!config_.lark_config_file.empty()) {
            std::string cmd = "python3 " + config_.alerts_script_dir + "/lark_alert.py"
                            + " -m \"" + message + "\""
                            + " -l " + level
                            + " --title \"" + escape_shell(subject) + "\""
                            + " -c \"" + config_.lark_config_file + "\""
                            + " --text";

            std::thread([cmd]() {
                int ret = std::system(cmd.c_str());
                if (ret != 0) {
                    std::cerr << "[下架监控] 飞书通知失败, 返回码: " << ret << "\n";
                } else {
                    std::cout << "[下架监控] ✓ 飞书通知已发送\n";
                }
            }).detach();
        }
    }

    /**
     * @brief 从 user_configs 读取超管/策略管理员邮箱，无 email 字段则用默认收件人兜底
     */
    std::vector<std::string> collect_notification_emails() {
        std::set<std::string> emails;

        for (const auto& e : config_.to_emails) {
            emails.insert(e);
        }

        if (!config_.user_config_dir.empty()
            && std::filesystem::exists(config_.user_config_dir)) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(config_.user_config_dir)) {
                if (entry.path().extension() != ".json") continue;
                try {
                    std::ifstream f(entry.path());
                    nlohmann::json user = nlohmann::json::parse(f);
                    std::string role = user.value("role", "");
                    bool active = user.value("active", false);
                    std::string email = user.value("email", "");

                    if (active && !email.empty()
                        && (role == "SUPER_ADMIN" || role == "STRATEGY_MANAGER")) {
                        emails.insert(email);
                    }
                } catch (...) {}
            }
        }

        return std::vector<std::string>(emails.begin(), emails.end());
    }

    static std::string escape_shell(const std::string& input) {
        std::string result;
        result.reserve(input.size());
        for (char c : input) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '`':  result += "\\`"; break;
                case '$':  result += "\\$"; break;
                default:   result += c; break;
            }
        }
        return result;
    }

    void load_state() {
        if (config_.state_file.empty() || !std::filesystem::exists(config_.state_file))
            return;
        try {
            std::ifstream f(config_.state_file);
            nlohmann::json j = nlohmann::json::parse(f);
            if (j.contains("notified_delisted") && j["notified_delisted"].is_array()) {
                for (const auto& s : j["notified_delisted"])
                    notified_delisted_.insert(s.get<std::string>());
            }
            if (j.contains("notified_upcoming") && j["notified_upcoming"].is_array()) {
                for (const auto& s : j["notified_upcoming"])
                    notified_upcoming_.insert(s.get<std::string>());
            }
            std::cout << "[下架监控] 已加载持久化状态: "
                      << notified_delisted_.size() << " 个已下架记录, "
                      << notified_upcoming_.size() << " 个预告记录\n";
        } catch (...) {
            std::cerr << "[下架监控] 加载持久化状态失败，忽略\n";
        }
    }

    void save_state() {
        if (config_.state_file.empty()) return;
        try {
            nlohmann::json j;
            j["notified_delisted"] = nlohmann::json::array();
            for (const auto& s : notified_delisted_)
                j["notified_delisted"].push_back(s);
            j["notified_upcoming"] = nlohmann::json::array();
            for (const auto& s : notified_upcoming_)
                j["notified_upcoming"].push_back(s);
            j["updated_at"] = get_current_time_str();

            std::ofstream f(config_.state_file);
            f << j.dump(2);
        } catch (...) {
            std::cerr << "[下架监控] 保存持久化状态失败\n";
        }
    }

    static std::string get_current_time_str() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
        return buf;
    }
};
