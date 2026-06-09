#pragma once

/**
 * @file vpn_network_monitor.h
 * @brief VPN/代理 网络连接监控模块
 *
 * 功能：
 * - 定期通过代理探测交易所地址，检测代理是否可用
 * - 连续失败超过阈值后发送邮件告警
 * - 代理恢复后发送恢复通知
 *
 * 使用方式：
 *   VpnNetworkMonitor monitor(g_risk_manager);
 *   monitor.load_config("totalconfig/network_monitor_config.json");
 *   monitor.start();
 *   // ...
 *   monitor.stop();
 *
 * @author Sequence Team
 * @date 2026-03
 */

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "../trading/risk_manager.h"

namespace trading {

/**
 * @brief VPN 网络监控配置
 */
struct VpnMonitorConfig {
    bool enabled = true;
    std::string proxy_host = "127.0.0.1";
    uint16_t proxy_port = 7890;
    std::vector<std::string> check_targets = {
        "https://www.okx.com",
        "https://fapi.binance.com"
    };
    int check_interval_seconds = 30;
    int check_timeout_seconds = 10;
    int fail_threshold = 3;
    int alert_cooldown_seconds = 300;
    bool notify_on_recovery = true;

    // 邮件告警配置文件路径（从 totalconfig 中提取）
    std::string email_config_file;
    // 直接指定收件人（从 totalconfig 的 email_alert.to_emails 读取）
    std::vector<std::string> to_emails;
};

/**
 * @brief VPN/代理 网络连接监控器
 */
class VpnNetworkMonitor {
public:
    explicit VpnNetworkMonitor(RiskManager& risk_manager)
        : risk_manager_(risk_manager), running_(false), consecutive_failures_(0),
          proxy_down_(false) {}

    ~VpnNetworkMonitor() {
        stop();
    }

    /**
     * @brief 从 JSON 配置文件加载配置
     * @param config_file totalconfig/network_monitor_config.json 路径
     * @return 是否加载成功
     */
    bool load_config(const std::string& config_file) {
        try {
            std::ifstream file(config_file);
            if (!file.is_open()) {
                std::cerr << "[VPN监控] 无法打开配置文件: " << config_file << "\n";
                return false;
            }

            nlohmann::json j;
            file >> j;

            // 解析 network_monitor 部分
            if (j.contains("network_monitor")) {
                auto& nm = j["network_monitor"];
                if (nm.contains("enabled")) config_.enabled = nm["enabled"];
                if (nm.contains("proxy_host")) config_.proxy_host = nm["proxy_host"];
                if (nm.contains("proxy_port")) config_.proxy_port = nm["proxy_port"];
                if (nm.contains("check_interval_seconds")) config_.check_interval_seconds = nm["check_interval_seconds"];
                if (nm.contains("check_timeout_seconds")) config_.check_timeout_seconds = nm["check_timeout_seconds"];
                if (nm.contains("fail_threshold")) config_.fail_threshold = nm["fail_threshold"];
                if (nm.contains("alert_cooldown_seconds")) config_.alert_cooldown_seconds = nm["alert_cooldown_seconds"];
                if (nm.contains("notify_on_recovery")) config_.notify_on_recovery = nm["notify_on_recovery"];

                if (nm.contains("check_targets") && nm["check_targets"].is_array()) {
                    config_.check_targets.clear();
                    for (const auto& t : nm["check_targets"]) {
                        config_.check_targets.push_back(t.get<std::string>());
                    }
                }
            }

            // 解析 email_alert 部分 - 提取收件人
            if (j.contains("email_alert")) {
                auto& ea = j["email_alert"];
                if (ea.contains("to_emails") && ea["to_emails"].is_array()) {
                    config_.to_emails.clear();
                    for (const auto& e : ea["to_emails"]) {
                        config_.to_emails.push_back(e.get<std::string>());
                    }
                }
            }

            // 将 email_alert 部分写入临时配置文件供 email_alert.py 使用
            if (j.contains("email_alert")) {
                // 使用与 totalconfig 同目录的临时邮件配置
                std::filesystem::path config_path(config_file);
                email_config_path_ = config_path.parent_path().string() + "/email_alert_network.json";

                std::ofstream email_file(email_config_path_);
                if (email_file.is_open()) {
                    email_file << j["email_alert"].dump(2);
                    email_file.close();
                    config_.email_config_file = email_config_path_;
                }
            }

            config_file_ = config_file;

            std::cout << "[VPN监控] ✓ 配置已加载: " << config_file << "\n";
            std::cout << "[VPN监控]   代理: " << config_.proxy_host << ":" << config_.proxy_port << "\n";
            std::cout << "[VPN监控]   探测目标: " << config_.check_targets.size() << " 个\n";
            std::cout << "[VPN监控]   检测间隔: " << config_.check_interval_seconds << " 秒\n";
            std::cout << "[VPN监控]   失败阈值: " << config_.fail_threshold << " 次\n";
            std::cout << "[VPN监控]   告警冷却: " << config_.alert_cooldown_seconds << " 秒\n";
            if (!config_.to_emails.empty()) {
                std::cout << "[VPN监控]   告警邮箱: " << config_.to_emails[0];
                if (config_.to_emails.size() > 1) std::cout << " (+" << (config_.to_emails.size() - 1) << ")";
                std::cout << "\n";
            }

            return true;
        } catch (const std::exception& e) {
            std::cerr << "[VPN监控] 加载配置失败: " << e.what() << "\n";
            return false;
        }
    }

    /**
     * @brief 启动监控线程
     */
    void start() {
        if (!config_.enabled) {
            std::cout << "[VPN监控] 已禁用，跳过启动\n";
            return;
        }

        if (running_.load()) {
            std::cout << "[VPN监控] 已在运行中\n";
            return;
        }

        running_.store(true);
        monitor_thread_ = std::thread(&VpnNetworkMonitor::monitor_loop, this);
        std::cout << "[VPN监控] ✓ 监控线程已启动\n";
    }

    /**
     * @brief 停止监控线程
     */
    void stop() {
        running_.store(false);
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
        std::cout << "[VPN监控] 监控线程已停止\n";
    }

    /**
     * @brief 获取当前状态
     */
    nlohmann::json get_status() const {
        return {
            {"enabled", config_.enabled},
            {"running", running_.load()},
            {"proxy_down", proxy_down_.load()},
            {"consecutive_failures", consecutive_failures_.load()},
            {"total_checks", total_checks_.load()},
            {"total_failures", total_failures_.load()},
            {"alerts_sent", alerts_sent_.load()}
        };
    }

private:
    /**
     * @brief 监控主循环
     */
    void monitor_loop() {
        std::cout << "[VPN监控] 监控循环开始\n";

        // 首次启动延迟 5 秒，等待其他组件初始化
        for (int i = 0; i < 50 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        while (running_.load()) {
            // 执行探测
            bool proxy_ok = check_proxy();
            total_checks_++;

            if (!proxy_ok) {
                int failures = ++consecutive_failures_;
                total_failures_++;

                std::cerr << "[VPN监控] 代理探测失败 (" << failures << "/" << config_.fail_threshold << ")\n";

                // 达到失败阈值，发送告警
                if (failures >= config_.fail_threshold && !proxy_down_.load()) {
                    proxy_down_.store(true);
                    send_proxy_down_alert(failures);
                }
                // 已经处于中断状态，检查是否需要重复告警
                else if (proxy_down_.load()) {
                    maybe_resend_alert(failures);
                }
            } else {
                // 探测成功
                if (proxy_down_.load()) {
                    // 从中断恢复
                    proxy_down_.store(false);
                    int was_failures = consecutive_failures_.load();
                    consecutive_failures_.store(0);

                    if (config_.notify_on_recovery) {
                        send_recovery_alert(was_failures);
                    }

                    std::cout << "[VPN监控] ✓ 代理已恢复（之前连续失败 " << was_failures << " 次）\n";
                } else {
                    consecutive_failures_.store(0);
                }
            }

            // 等待下一次检测
            for (int i = 0; i < config_.check_interval_seconds * 10 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        std::cout << "[VPN监控] 监控循环退出\n";
    }

    /**
     * @brief 通过代理探测目标地址
     * @return true=代理可用, false=代理不可用
     */
    bool check_proxy() {
        std::string proxy_url = "http://" + config_.proxy_host + ":" + std::to_string(config_.proxy_port);

        for (const auto& target : config_.check_targets) {
            if (probe_url_via_proxy(target, proxy_url)) {
                return true;  // 任一目标可达即认为代理正常
            }
        }
        return false;  // 所有目标都不可达
    }

    /**
     * @brief 通过代理访问 URL，检测连通性
     */
    bool probe_url_via_proxy(const std::string& url, const std::string& proxy_url) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        // 只需要 HEAD 请求，不需要下载内容
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);  // HEAD 请求
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(config_.check_timeout_seconds));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(config_.check_timeout_seconds));

        // 设置代理
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
        curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);

        // SSL 设置
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#if LIBCURL_VERSION_NUM >= 0x073400
        curl_easy_setopt(curl, CURLOPT_PROXY_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_PROXY_SSL_VERIFYHOST, 0L);
#endif

        // 禁止跟随重定向（只检测连通性）
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        }

        curl_easy_cleanup(curl);

        // 只要能连上（不管 HTTP 状态码），就说明代理是通的
        return (res == CURLE_OK);
    }

    /**
     * @brief 发送代理中断告警
     */
    void send_proxy_down_alert(int fail_count) {
        auto now = std::chrono::steady_clock::now();
        last_alert_time_ = now;
        alerts_sent_++;

        std::string targets_str;
        for (const auto& t : config_.check_targets) {
            if (!targets_str.empty()) targets_str += ", ";
            targets_str += t;
        }

        std::string message =
            "VPN/代理连接中断！\n\n"
            "代理地址: " + config_.proxy_host + ":" + std::to_string(config_.proxy_port) + "\n"
            "连续失败: " + std::to_string(fail_count) + " 次\n"
            "探测目标: " + targets_str + "\n"
            "检测间隔: " + std::to_string(config_.check_interval_seconds) + " 秒\n\n"
            "请立即检查：\n"
            "1. VPN/代理软件（mihomo/Clash）是否正常运行\n"
            "2. 代理端口 " + std::to_string(config_.proxy_port) + " 是否可访问\n"
            "3. 网络连接是否正常\n\n"
            "代理中断期间，所有交易所 REST API 和 WebSocket 连接将受影响！";

        std::cerr << "[VPN监控] ⚠️  代理中断，发送告警邮件\n";

        // 通过风控系统发送告警（走所有渠道）
        risk_manager_.send_alert(message, AlertLevel::CRITICAL, "VPN/代理连接中断");

        // 额外发送到 totalconfig 配置的邮箱
        send_email_to_config_recipients(message, "critical", "VPN/代理连接中断");
    }

    /**
     * @brief 检查是否需要重复发送告警（冷却时间后）
     */
    void maybe_resend_alert(int fail_count) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_alert_time_).count();

        if (elapsed >= config_.alert_cooldown_seconds) {
            send_proxy_down_alert(fail_count);
        }
    }

    /**
     * @brief 发送代理恢复通知
     */
    void send_recovery_alert(int was_failures) {
        std::string message =
            "VPN/代理连接已恢复！\n\n"
            "代理地址: " + config_.proxy_host + ":" + std::to_string(config_.proxy_port) + "\n"
            "之前连续失败: " + std::to_string(was_failures) + " 次\n\n"
            "交易所连接已恢复正常。";

        std::cout << "[VPN监控] ✓ 代理恢复，发送恢复通知\n";

        risk_manager_.send_alert(message, AlertLevel::INFO, "VPN/代理连接恢复");
        send_email_to_config_recipients(message, "info", "VPN/代理连接恢复");
    }

    /**
     * @brief 调用 email_alert.py 发送邮件到 totalconfig 配置的收件人
     */
    void send_email_to_config_recipients(const std::string& message,
                                          const std::string& level,
                                          const std::string& subject) {
        if (config_.to_emails.empty() || config_.email_config_file.empty()) {
            return;
        }

        // 构建收件人列表
        std::string to_str;
        for (const auto& email : config_.to_emails) {
            if (!to_str.empty()) to_str += ",";
            to_str += email;
        }

        // 查找 email_alert.py 路径
        // __FILE__ = .../cpp/network/vpn_network_monitor.h
        // parent_path().parent_path() = .../cpp/
        std::filesystem::path current_file(__FILE__);
        std::string alerts_path = current_file.parent_path().parent_path().string() + "/trading/alerts";

        std::string cmd = "python3 " + alerts_path + "/email_alert.py"
                          " -m \"" + escape_string(message) + "\""
                          " -l " + level +
                          " -s \"" + escape_string(subject) + "\""
                          " -c \"" + config_.email_config_file + "\""
                          " --to \"" + to_str + "\"";

        std::cout << "[VPN监控] 执行邮件命令: " << cmd << "\n";

        // 异步执行
        std::thread([cmd]() {
            int ret = std::system(cmd.c_str());
            if (ret != 0) {
                std::cerr << "[VPN监控] 邮件发送失败, 返回码: " << ret << "\n";
            } else {
                std::cout << "[VPN监控] 邮件发送成功\n";
            }
        }).detach();
    }

    /**
     * @brief 转义 shell 特殊字符
     */
    static std::string escape_string(const std::string& str) {
        std::string result;
        for (char c : str) {
            if (c == '"' || c == '\\' || c == '$' || c == '`') {
                result += '\\';
            }
            result += c;
        }
        return result;
    }

    RiskManager& risk_manager_;
    VpnMonitorConfig config_;
    std::string config_file_;
    std::string email_config_path_;  // 生成的邮件配置文件路径

    std::thread monitor_thread_;
    std::atomic<bool> running_;
    std::atomic<int> consecutive_failures_;
    std::atomic<bool> proxy_down_;

    // 统计
    std::atomic<uint64_t> total_checks_{0};
    std::atomic<uint64_t> total_failures_{0};
    std::atomic<uint64_t> alerts_sent_{0};

    // 告警冷却
    std::chrono::steady_clock::time_point last_alert_time_;
};

} // namespace trading
