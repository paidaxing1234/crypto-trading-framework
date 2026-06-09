/**
 * @file frontend_command_handler.cpp
 * @brief 前端 WebSocket 命令处理模块实现（含认证）
 */

#include "frontend_command_handler.h"
#include "../config/server_config.h"

#include "order_processor.h"  // 包含 g_risk_manager, g_account_monitor 声明
#include "../managers/account_monitor.h"  // AccountCredentials
#include "../../network/websocket_server.h"
#include "../../network/auth_manager.h"
#include "../../core/logger.h"
#include "../../trading/account_registry.h"
#include "../../trading/strategy_config_loader.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <dirent.h>
#include <set>
#include <deque>
#include <algorithm>
#include <iomanip>
#include <sys/stat.h>

using namespace trading::core;

namespace trading {
namespace server {

// Debug 日志：写入 frontend.txt
static void frontend_debug(const std::string& msg) {
    static std::mutex dbg_mtx;
    std::lock_guard<std::mutex> lock(dbg_mtx);
    std::ofstream f("logs/frontend.txt", std::ios::app);
    if (f.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        char buf[32];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
        f << "[" << buf << "." << std::setfill('0') << std::setw(3) << ms.count() << "] " << msg << "\n";
        f.flush();
    }
}

// 获取可执行文件所在目录（cpp/build/），配置默认路径相对于此目录
static std::string get_exe_dir() {
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe");
    return exe_path.parent_path().string();  // cpp/build/
}

// ============================================================
// 账户配置文件 I/O 辅助函数
// ============================================================

static std::string get_account_config_dir() {
    return get_exe_dir() + "/../strategies/acount_configs";
}

// 前端生成的策略配置存放目录（区别于模板 configs/）
static std::string get_strategy_config_dir() {
    return get_exe_dir() + "/../strategies/strategy_configs";
}

static std::string get_account_config_path(const std::string& exchange, const std::string& account_id) {
    return get_account_config_dir() + "/" + exchange + "_" + account_id + ".json";
}

static bool save_account_config_file(const std::string& exchange, const std::string& account_id,
                                      const std::string& api_key, const std::string& secret_key,
                                      const std::string& passphrase, bool is_testnet) {
    std::string dir = get_account_config_dir();
    std::string path = get_account_config_path(exchange, account_id);
    try {
        // 确保目录存在
        std::filesystem::create_directories(dir);

        nlohmann::json config;
        config["account_id"] = account_id;
        config["exchange"] = exchange;
        config["api_key"] = api_key;
        config["secret_key"] = secret_key;
        if (exchange == "okx") {
            config["passphrase"] = passphrase;
        }
        config["is_testnet"] = is_testnet;

        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%FT%TZ", std::gmtime(&t));
        config["created_at"] = std::string(buf);

        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << config.dump(2);
        f.close();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("[账户配置] 保存失败: " + path + " - " + e.what());
        return false;
    }
}

static bool delete_account_config_file(const std::string& exchange, const std::string& account_id) {
    std::string path = get_account_config_path(exchange, account_id);
    try {
        return std::filesystem::remove(path);
    } catch (...) {
        return false;
    }
}

static nlohmann::json load_all_account_configs() {
    nlohmann::json result = {{"okx", nlohmann::json::array()}, {"binance", nlohmann::json::array()}};
    std::string dir = get_account_config_dir();
    if (!std::filesystem::exists(dir)) return result;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        try {
            std::ifstream f(entry.path().string());
            nlohmann::json config;
            f >> config;
            f.close();

            std::string exchange = config.value("exchange", "");
            // 脱敏显示
            nlohmann::json display = config;
            std::string ak = config.value("api_key", "");
            display["api_key"] = ak.size() > 8 ? ak.substr(0, 8) + "..." : ak;
            display.erase("secret_key");
            display.erase("passphrase");

            if (exchange == "okx") result["okx"].push_back(display);
            else if (exchange == "binance") result["binance"].push_back(display);
        } catch (...) { /* 跳过格式错误的文件 */ }
    }
    return result;
}

// 解析单行日志
nlohmann::json parse_log_line(const std::string& line) {
    // 格式: [2026-01-07 01:27:26.775] [INFO ] [source] message
    std::regex log_regex(R"(\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\] \[(\w+)\s*\] \[(\w+)\] (.*))");
    std::smatch match;

    if (std::regex_match(line, match, log_regex)) {
        std::string timestamp_str = match[1].str();
        std::string level = match[2].str();
        std::string source = match[3].str();
        std::string message = match[4].str();

        // 转换级别为小写
        for (auto& c : level) c = std::tolower(c);
        if (level == "warn") level = "warning";

        // 解析时间戳为毫秒
        std::tm tm = {};
        int ms = 0;
        sscanf(timestamp_str.c_str(), "%d-%d-%d %d:%d:%d.%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms);
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        auto time_point = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            time_point.time_since_epoch()).count() + ms;

        return {
            {"timestamp", timestamp},
            {"level", level},
            {"source", source},
            {"message", message}
        };
    }
    return nlohmann::json();
}

// 读取日志文件
nlohmann::json read_log_files(const std::string& date, const std::string& source_filter,
                              const std::string& level_filter, int limit, int offset) {
    nlohmann::json logs = nlohmann::json::array();
    std::string log_dir = "logs";

    // 获取日志文件列表
    std::vector<std::string> log_files;
    DIR* dir = opendir(log_dir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            if (filename.find(".log") != std::string::npos) {
                // 如果指定了日期，只读取该日期的文件
                if (!date.empty()) {
                    if (filename.find(date) != std::string::npos) {
                        log_files.push_back(log_dir + "/" + filename);
                    }
                } else {
                    log_files.push_back(log_dir + "/" + filename);
                }
            }
        }
        closedir(dir);
    }

    // 按文件名排序（日期排序）
    std::sort(log_files.begin(), log_files.end());

    int total_count = 0;
    int skipped = 0;

    for (const auto& filepath : log_files) {
        std::ifstream file(filepath);
        if (!file.is_open()) continue;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;

            auto log_entry = parse_log_line(line);
            if (log_entry.empty()) continue;

            // 来源过滤
            if (!source_filter.empty() && log_entry["source"] != source_filter) continue;

            // 级别过滤
            if (!level_filter.empty() && log_entry["level"] != level_filter) continue;

            total_count++;

            // 分页
            if (skipped < offset) {
                skipped++;
                continue;
            }

            if (limit > 0 && (int)logs.size() >= limit) continue;

            logs.push_back(log_entry);
        }
    }

    return {
        {"logs", logs},
        {"total", total_count},
        {"offset", offset},
        {"limit", limit}
    };
}

// 获取已认证客户端的角色和用户名
static bool get_client_auth(int client_id, auth::TokenInfo& out_info) {
    std::lock_guard<std::mutex> lock(g_auth_mutex);
    auto it = g_authenticated_clients.find(client_id);
    if (it == g_authenticated_clients.end()) return false;
    out_info = it->second;
    return true;
}

// 获取策略管理员的 allowed_strategies（超级管理员返回空 = 不过滤）
static std::vector<std::string> get_allowed_strategies(const std::string& username, auth::UserRole role) {
    if (role == auth::UserRole::SUPER_ADMIN) return {};
    const auto* user = g_auth_manager.get_user_info(username);
    if (!user) return {};
    return user->allowed_strategies;
}

// 检查策略管理员是否有权访问某个策略
static bool is_strategy_allowed(const std::vector<std::string>& allowed, const std::string& strategy_id) {
    if (allowed.empty()) return true;  // 超级管理员（空列表 = 全部允许）
    for (const auto& s : allowed) {
        if (strategy_id.find(s) != std::string::npos || s.find(strategy_id) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void handle_frontend_command(int client_id, const nlohmann::json& message) {
    try {
        std::string msg_type = message.value("type", "");
        std::string action = message.value("action", "");
        nlohmann::json data = message.value("data", nlohmann::json::object());
        std::string request_id = data.value("requestId", "");

        frontend_debug(">>> 收到消息 client=" + std::to_string(client_id) + " type=" + msg_type + " action=" + action + " requestId=" + request_id);

        // ==================== 认证相关（无需登录） ====================
        if (msg_type == "login") {
            std::string username = message.value("username", "");
            std::string password = message.value("password", "");
            std::string token = g_auth_manager.login(username, password);

            if (token.empty()) {
                nlohmann::json response = {
                    {"type", "login_response"},
                    {"success", false},
                    {"message", "用户名或密码错误"}
                };
                g_frontend_server->send_response(client_id, false, "用户名或密码错误", response);
                LOG_INFO("登录失败: " + username);
            } else {
                auth::TokenInfo info;
                g_auth_manager.verify_token(token, info);

                {
                    std::lock_guard<std::mutex> lock(g_auth_mutex);
                    g_authenticated_clients[client_id] = info;
                }

                nlohmann::json user_obj = {
                    {"username", username},
                    {"role", auth::AuthManager::role_to_string(info.role)}
                };
                // 策略管理员返回 allowed_strategies
                if (info.role == auth::UserRole::STRATEGY_MANAGER) {
                    const auto* user_info = g_auth_manager.get_user_info(username);
                    if (user_info) {
                        user_obj["allowed_strategies"] = user_info->allowed_strategies;
                    }
                }
                nlohmann::json response = {
                    {"type", "login_response"},
                    {"success", true},
                    {"token", token},
                    {"user", user_obj}
                };
                g_frontend_server->send_response(client_id, true, "登录成功", response);
                LOG_INFO("登录成功: " + username + " (角色: " + auth::AuthManager::role_to_string(info.role) + ")");
            }
            return;
        }

        if (msg_type == "logout" || action == "logout") {
            std::string token = message.value("token", "");
            g_auth_manager.logout(token);
            {
                std::lock_guard<std::mutex> lock(g_auth_mutex);
                g_authenticated_clients.erase(client_id);
            }
            nlohmann::json response = {
                {"type", "logout_response"},
                {"success", true},
                {"message", "已登出"}
            };
            g_frontend_server->send_response(client_id, true, "已登出", response);
            LOG_INFO("客户端 " + std::to_string(client_id) + " 已登出");
            return;
        }

        if (msg_type == "get_user_info") {
            std::lock_guard<std::mutex> lock(g_auth_mutex);
            auto it = g_authenticated_clients.find(client_id);
            if (it != g_authenticated_clients.end()) {
                nlohmann::json response = {
                    {"type", "user_info"},
                    {"success", true},
                    {"user", {
                        {"username", it->second.username},
                        {"role", auth::AuthManager::role_to_string(it->second.role)}
                    }}
                };
                g_frontend_server->send_response(client_id, true, "", response);
            } else {
                g_frontend_server->send_response(client_id, false, "未登录", {{"type", "user_info"}});
            }
            return;
        }

        // ==================== 以下命令需要认证 ====================
        Logger::instance().info("system", "收到命令: " + action + " (客户端: " + std::to_string(client_id) + ")");

        nlohmann::json response;

        if (action == "set_log_config") {
            // 处理日志配置命令
            std::string level = data.value("level", "info");
            LogLevel log_level = LogLevel::INFO;
            if (level == "debug") log_level = LogLevel::DEBUG;
            else if (level == "info") log_level = LogLevel::INFO;
            else if (level == "warning") log_level = LogLevel::WARN;
            else if (level == "error") log_level = LogLevel::ERROR;

            Logger::instance().set_level(log_level);
            response = {{"success", true}, {"message", "日志级别已设置为: " + level}};
        }
        else if (action == "frontend_log") {
            // 前端发送的日志，记录到后端
            std::string level = data.value("level", "info");
            std::string message = data.value("message", "");
            Logger::instance().info("frontend", message);
            response = {{"success", true}, {"message", "日志已记录"}};
        }
        else if (action == "get_logs") {
            // 读取本地日志文件
            std::string date = data.value("date", "");  // 格式: 20260107
            std::string source = data.value("source", "");
            std::string level = data.value("level", "");
            int limit = data.value("limit", 500);
            int offset = data.value("offset", 0);

            auto logs_data = read_log_files(date, source, level, limit, offset);
            response = {
                {"success", true},
                {"type", "logs_data"},
                {"data", logs_data}
            };
        }
        else if (action == "get_log_dates") {
            // 获取可用的日志日期列表
            nlohmann::json dates = nlohmann::json::array();
            std::string log_dir = "logs";

            DIR* dir = opendir(log_dir.c_str());
            if (dir) {
                struct dirent* entry;
                std::regex date_regex(R"(.*_(\d{8})\.log)");
                std::set<std::string> date_set;

                while ((entry = readdir(dir)) != nullptr) {
                    std::string filename = entry->d_name;
                    std::smatch match;
                    if (std::regex_match(filename, match, date_regex)) {
                        date_set.insert(match[1].str());
                    }
                }
                closedir(dir);

                for (const auto& d : date_set) {
                    dates.push_back(d);
                }
            }

            response = {
                {"success", true},
                {"type", "log_dates"},
                {"dates", dates}
            };
        }
        else if (action == "get_account_positions") {
            // 获取账户持仓数据
            std::string account_id = data.value("accountId", "");
            if (account_id.empty()) {
                response = {{"success", false}, {"message", "缺少 accountId 参数"}};
            } else {
                nlohmann::json positions = g_account_registry.get_account_positions(account_id);
                response = {
                    {"success", true},
                    {"type", "account_positions"},
                    {"data", positions}
                };
            }
        }
        else if (action == "get_recent_orders") {
            // 从策略日志文件中解析最近的订单记录
            std::string log_dir = get_exe_dir() + "/../logs";
            try { log_dir = std::filesystem::canonical(log_dir).string(); } catch (...) {}
            int limit = data.value("limit", 100);

            // 获取今天的日期字符串
            auto now = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(now);
            struct tm tm_buf;
            localtime_r(&tt, &tm_buf);
            char date_str[16];
            strftime(date_str, sizeof(date_str), "%Y%m%d", &tm_buf);
            std::string today(date_str);

            struct OrderEntry {
                std::string timestamp;
                std::string level;
                std::string strategy;
                std::string order_id;
                std::string status;
                std::string symbol;
                std::string side;
                std::string quantity;
                std::string detail;
            };
            std::vector<OrderEntry> all_orders;

            auto extract_val = [](const std::string& text, const std::string& key) -> std::string {
                auto kpos = text.find(key + "=");
                if (kpos == std::string::npos) return "";
                auto vstart = kpos + key.size() + 1;
                auto vend = text.find(' ', vstart);
                if (vend == std::string::npos) vend = text.size();
                return text.substr(vstart, vend - vstart);
            };

            DIR* dir = opendir(log_dir.c_str());
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    std::string filename = entry->d_name;
                    if (filename.size() < 4) continue;
                    if (filename.substr(filename.size() - 4) != ".log") continue;
                    // 跳过系统日志
                    if (filename.find("main_") == 0 || filename.find("market_") == 0 ||
                        filename.find("alert_") == 0 || filename.find("frontend") != std::string::npos) continue;
                    // 只读今天的日志
                    if (filename.find(today) == std::string::npos) continue;

                    std::string filepath = log_dir + "/" + filename;
                    std::ifstream file(filepath);
                    std::string line;
                    while (std::getline(file, line)) {
                        // 只处理 [ORDER: 开头的订单记录
                        if (line.find("[ORDER:") == std::string::npos) continue;

                        OrderEntry oe;
                        // 解析时间戳: [2026-03-12 10:36:00.177]
                        auto ts_start = line.find('[');
                        auto ts_end = line.find(']');
                        if (ts_start == std::string::npos || ts_end == std::string::npos) continue;
                        oe.timestamp = line.substr(ts_start + 1, ts_end - ts_start - 1);

                        // 解析日志级别
                        auto lv_start = line.find('[', ts_end + 1);
                        auto lv_end = line.find(']', lv_start + 1);
                        if (lv_start == std::string::npos || lv_end == std::string::npos) continue;
                        oe.level = line.substr(lv_start + 1, lv_end - lv_start - 1);
                        // trim spaces
                        while (!oe.level.empty() && oe.level.back() == ' ') oe.level.pop_back();
                        while (!oe.level.empty() && oe.level.front() == ' ') oe.level.erase(oe.level.begin());

                        // 解析策略来源
                        auto src_start = line.find('[', lv_end + 1);
                        auto src_end = line.find(']', src_start + 1);
                        if (src_start == std::string::npos || src_end == std::string::npos) continue;
                        oe.strategy = line.substr(src_start + 1, src_end - src_start - 1);

                        // 解析 [ORDER:id]
                        auto ord_start = line.find("[ORDER:", src_end + 1);
                        auto ord_end = line.find(']', ord_start + 7);
                        if (ord_start == std::string::npos || ord_end == std::string::npos) continue;
                        oe.order_id = line.substr(ord_start + 7, ord_end - ord_start - 7);

                        // 解析 STATUS | detail
                        size_t rest_start = ord_end + 1;
                        while (rest_start < line.size() && line[rest_start] == ' ') rest_start++;
                        std::string rest = line.substr(rest_start);
                        auto pipe_pos = rest.find(" | ");
                        if (pipe_pos != std::string::npos) {
                            oe.status = rest.substr(0, pipe_pos);
                            oe.detail = rest.substr(pipe_pos + 3);
                        } else {
                            oe.status = rest;
                        }

                        // 从 detail 提取 symbol/side/qty
                        oe.symbol = extract_val(oe.detail, "symbol");
                        oe.side = extract_val(oe.detail, "side");
                        oe.quantity = extract_val(oe.detail, "qty");

                        all_orders.push_back(oe);
                    }
                }
                closedir(dir);
            }

            // 按时间戳降序排序
            std::sort(all_orders.begin(), all_orders.end(), [](const OrderEntry& a, const OrderEntry& b) {
                return a.timestamp > b.timestamp;
            });

            if ((int)all_orders.size() > limit) {
                all_orders.resize(limit);
            }

            nlohmann::json orders_json = nlohmann::json::array();
            for (const auto& oe : all_orders) {
                orders_json.push_back({
                    {"timestamp", oe.timestamp},
                    {"level", oe.level},
                    {"strategy", oe.strategy},
                    {"order_id", oe.order_id},
                    {"status", oe.status},
                    {"symbol", oe.symbol},
                    {"side", oe.side},
                    {"quantity", oe.quantity},
                    {"detail", oe.detail}
                });
            }

            response = {
                {"success", true},
                {"type", "recent_orders"},
                {"data", orders_json}
            };
        }
        else if (action == "get_system_log_files") {
            // 获取系统日志文件列表（cpp/logs/ 目录）
            std::string log_dir = get_exe_dir() + "/../logs";
            try { log_dir = std::filesystem::canonical(log_dir).string(); } catch (...) {}
            nlohmann::json files = nlohmann::json::array();

            // 获取策略管理员的 allowed_strategies 用于过滤
            auth::TokenInfo caller;
            std::vector<std::string> allowed;
            if (get_client_auth(client_id, caller)) {
                allowed = get_allowed_strategies(caller.username, caller.role);
            }

            DIR* dir = opendir(log_dir.c_str());
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    std::string filename = entry->d_name;
                    // 包含 .log 和 .txt 文件
                    if (filename.size() < 4) continue;
                    std::string ext = filename.substr(filename.size() - 4);
                    if (ext != ".log" && ext != ".txt") continue;

                    // 策略管理员过滤：只显示与其策略相关的日志文件
                    if (!allowed.empty() && !is_strategy_allowed(allowed, filename)) continue;

                    std::string filepath = log_dir + "/" + filename;
                    struct stat st;
                    long file_size = 0;
                    if (stat(filepath.c_str(), &st) == 0) {
                        file_size = st.st_size;
                    }
                    files.push_back({{"filename", filename}, {"size", file_size}});
                }
                closedir(dir);
            }

            response = {
                {"success", true},
                {"type", "system_log_files"},
                {"data", files}
            };
        }
        else if (action == "get_system_logs") {
            // 读取系统日志内容
            std::string filename = data.value("filename", "");
            int tail_lines = data.value("tailLines", 200);
            std::string log_dir = get_exe_dir() + "/../logs";
            try { log_dir = std::filesystem::canonical(log_dir).string(); } catch (...) {}
            std::string filepath = log_dir + "/" + filename;

            // 安全检查：防止路径遍历
            if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos) {
                response = {{"success", false}, {"message", "非法文件名"}};
            } else {
                std::ifstream file(filepath);
                if (!file.is_open()) {
                    response = {{"success", false}, {"message", "日志文件不存在: " + filename}};
                } else {
                    std::deque<std::string> lines;
                    std::string line;
                    while (std::getline(file, line)) {
                        lines.push_back(line);
                        if ((int)lines.size() > tail_lines) {
                            lines.pop_front();
                        }
                    }
                    file.close();

                    nlohmann::json log_lines = nlohmann::json::array();
                    for (const auto& l : lines) {
                        log_lines.push_back(l);
                    }

                    response = {
                        {"success", true},
                        {"type", "system_logs"},
                        {"data", {{"filename", filename}, {"lines", log_lines}, {"totalLines", (int)log_lines.size()}}}
                    };
                }
            }
        }
        else if (action == "get_strategy_log_files") {
            // 获取策略日志文件列表
            std::string strategy_id = data.value("strategyId", "");
            std::string strategy_log_dir = get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_log_dir;
            nlohmann::json files = nlohmann::json::array();

            // 获取策略管理员的 allowed_strategies 用于过滤
            auth::TokenInfo caller;
            std::vector<std::string> allowed;
            if (get_client_auth(client_id, caller)) {
                allowed = get_allowed_strategies(caller.username, caller.role);
            }

            DIR* dir = opendir(strategy_log_dir.c_str());
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    std::string filename = entry->d_name;
                    if (filename.size() < 4 || filename.substr(filename.size() - 4) != ".log") continue;
                    if (!strategy_id.empty() && filename.find(strategy_id) == std::string::npos) continue;

                    // 策略管理员过滤
                    if (!allowed.empty() && !is_strategy_allowed(allowed, filename)) continue;

                    // 获取文件大小
                    std::string filepath = strategy_log_dir + "/" + filename;
                    struct stat st;
                    long file_size = 0;
                    if (stat(filepath.c_str(), &st) == 0) {
                        file_size = st.st_size;
                    }
                    files.push_back({{"filename", filename}, {"size", file_size}});
                }
                closedir(dir);
            }

            response = {
                {"success", true},
                {"type", "strategy_log_files"},
                {"data", files}
            };
        }
        else if (action == "get_strategy_logs") {
            // 读取策略日志内容
            std::string filename = data.value("filename", "");
            int tail_lines = data.value("tailLines", 200);
            std::string strategy_log_dir = get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_log_dir;
            std::string filepath = strategy_log_dir + "/" + filename;

            // 安全检查：防止路径遍历
            if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos) {
                response = {{"success", false}, {"message", "非法文件名"}};
            } else {
                std::ifstream file(filepath);
                if (!file.is_open()) {
                    response = {{"success", false}, {"message", "日志文件不存在: " + filename}};
                } else {
                    // 读取最后 tail_lines 行
                    std::deque<std::string> lines;
                    std::string line;
                    while (std::getline(file, line)) {
                        lines.push_back(line);
                        if ((int)lines.size() > tail_lines) {
                            lines.pop_front();
                        }
                    }
                    file.close();

                    nlohmann::json log_lines = nlohmann::json::array();
                    for (const auto& l : lines) {
                        log_lines.push_back(l);
                    }

                    response = {
                        {"success", true},
                        {"type", "strategy_logs"},
                        {"data", {{"filename", filename}, {"lines", log_lines}, {"totalLines", (int)log_lines.size()}}}
                    };
                }
            }
        }
        else if (action == "download_log_file") {
            // 下载完整日志文件（系统日志或策略日志）
            std::string filename = data.value("filename", "");
            std::string log_type = data.value("logType", "system");  // "system" 或 "strategy"

            // 安全检查
            if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos) {
                response = {{"success", false}, {"message", "非法文件名"}};
            } else {
                std::string filepath;
                if (log_type == "strategy") {
                    filepath = get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_log_dir + "/" + filename;
                } else {
                    std::string log_dir = get_exe_dir() + "/../logs";
                    try { log_dir = std::filesystem::canonical(log_dir).string(); } catch (...) {}
                    filepath = log_dir + "/" + filename;
                }

                // 检查文件大小（限制 50MB）
                struct stat st;
                if (stat(filepath.c_str(), &st) != 0) {
                    response = {{"success", false}, {"message", "文件不存在: " + filename}};
                } else if (st.st_size > 50 * 1024 * 1024) {
                    response = {{"success", false}, {"message", "文件过大（超过50MB），请使用其他方式下载"}};
                } else {
                    std::ifstream file(filepath, std::ios::binary);
                    if (!file.is_open()) {
                        response = {{"success", false}, {"message", "无法打开文件: " + filename}};
                    } else {
                        std::string content((std::istreambuf_iterator<char>(file)),
                                             std::istreambuf_iterator<char>());
                        file.close();

                        response = {
                            {"success", true},
                            {"type", "download_log_file"},
                            {"data", {
                                {"filename", filename},
                                {"content", content},
                                {"size", (long)content.size()}
                            }}
                        };
                    }
                }
            }
        }
        else if (action == "list_strategy_files") {
            // 列出策略源代码文件
            std::string strategy_dir = get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_source_dir;
            nlohmann::json files = nlohmann::json::array();

            // 获取策略管理员的 allowed_strategies 用于过滤
            auth::TokenInfo caller;
            std::vector<std::string> allowed;
            if (get_client_auth(client_id, caller)) {
                allowed = get_allowed_strategies(caller.username, caller.role);
            }

            std::function<void(const std::string&, const std::string&)> scan_dir;
            scan_dir = [&](const std::string& dir_path, const std::string& prefix) {
                DIR* dir = opendir(dir_path.c_str());
                if (!dir) return;
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    std::string name = entry->d_name;
                    if (name == "." || name == ".." || name == "__pycache__" || name == "logs") continue;
                    std::string full_path = dir_path + "/" + name;
                    std::string rel_path = prefix.empty() ? name : prefix + "/" + name;
                    struct stat st;
                    if (stat(full_path.c_str(), &st) == 0) {
                        if (S_ISDIR(st.st_mode)) {
                            scan_dir(full_path, rel_path);
                        } else if (name.size() > 3 && name.substr(name.size() - 3) == ".py") {
                            // 策略管理员过滤
                            if (!allowed.empty() && !is_strategy_allowed(allowed, rel_path)) continue;
                            files.push_back({{"filename", rel_path}, {"size", st.st_size}});
                        }
                    }
                }
                closedir(dir);
            };
            scan_dir(strategy_dir, "");

            response = {
                {"success", true},
                {"type", "strategy_files"},
                {"data", files}
            };
        }
        else if (action == "get_strategy_source") {
            // 读取策略源代码
            std::string filename = data.value("filename", "");
            std::string strategy_dir = get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_source_dir;
            std::string filepath = strategy_dir + "/" + filename;

            // 安全检查
            if (filename.find("..") != std::string::npos) {
                response = {{"success", false}, {"message", "非法文件名"}};
            } else if (filename.substr(filename.size() - 3) != ".py") {
                response = {{"success", false}, {"message", "只能查看Python策略文件"}};
            } else {
                std::ifstream file(filepath);
                if (!file.is_open()) {
                    response = {{"success", false}, {"message", "文件不存在: " + filename}};
                } else {
                    std::stringstream ss;
                    ss << file.rdbuf();
                    file.close();
                    response = {
                        {"success", true},
                        {"type", "strategy_source"},
                        {"data", {{"filename", filename}, {"content", ss.str()}}}
                    };
                }
            }
        }
        else if (action == "save_strategy_source") {
            // 策略管理员禁止保存代码
            auth::TokenInfo caller;
            if (get_client_auth(client_id, caller) && caller.role == auth::UserRole::STRATEGY_MANAGER) {
                response = {{"success", false}, {"message", "策略管理员无权编辑策略代码"}};
                if (!request_id.empty()) response["requestId"] = request_id;
                g_frontend_server->send_response(client_id, false, "策略管理员无权编辑策略代码", response);
                return;
            }
            // 保存策略源代码
            std::string filename = data.value("filename", "");
            std::string content = data.value("content", "");
            std::string strategy_dir = get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_source_dir;
            std::string filepath = strategy_dir + "/" + filename;

            // 安全检查
            if (filename.find("..") != std::string::npos) {
                response = {{"success", false}, {"message", "非法文件名"}};
            } else if (filename.substr(filename.size() - 3) != ".py") {
                response = {{"success", false}, {"message", "只能保存Python策略文件"}};
            } else {
                // 备份原文件
                std::string backup_path = filepath + ".bak";
                std::ifstream src(filepath, std::ios::binary);
                if (src.is_open()) {
                    std::ofstream dst(backup_path, std::ios::binary);
                    dst << src.rdbuf();
                    src.close();
                    dst.close();
                }

                // 保存新内容
                std::ofstream file(filepath);
                if (!file.is_open()) {
                    response = {{"success", false}, {"message", "无法写入文件: " + filename}};
                } else {
                    file << content;
                    file.close();
                    response = {
                        {"success", true},
                        {"type", "save_strategy_source"},
                        {"message", "保存成功"}
                    };
                }
            }
        }
        else if (action == "get_risk_status") {
            // 获取风控状态
            auto stats = g_risk_manager.get_risk_stats();
            response["success"] = true;
            response["type"] = "risk_status";
            response["data"] = stats;

            bool kill_switch_active = stats["kill_switch"].get<bool>();
            LOG_INFO("查询风控状态: kill_switch=" + std::to_string(kill_switch_active));
        }
        else if (action == "deactivate_kill_switch") {
            // 解除kill switch
            if (g_risk_manager.is_kill_switch_active()) {
                g_risk_manager.deactivate_kill_switch();
                response["success"] = true;
                response["message"] = "Kill switch已解除，交易已恢复";
                LOG_INFO("Kill switch已通过前端命令解除");
            } else {
                response["success"] = true;
                response["message"] = "Kill switch当前未激活";
            }
        }
        else if (action == "register_account") {
            // 注册账户（添加交易所 API 凭证）
            std::string strategy_id = data.value("strategy_id", "");
            std::string account_id = data.value("account_id", "");
            std::string exchange = data.value("exchange", "okx");
            // 统一转小写，确保文件名一致
            std::transform(exchange.begin(), exchange.end(), exchange.begin(), ::tolower);
            std::string api_key = data.value("api_key", "");
            std::string secret_key = data.value("secret_key", "");
            std::string passphrase = data.value("passphrase", "");
            bool is_testnet = data.value("is_testnet", true);

            LOG_INFO("[账户注册] 策略: " + strategy_id + " | 账户: " + account_id + " | 交易所: " + exchange);

            if (api_key.empty() || secret_key.empty()) {
                response = {{"success", false}, {"message", "缺少必要参数 (api_key, secret_key)"}};
            } else {
                ExchangeType ex_type = string_to_exchange_type(exchange);
                bool success = false;

                if (strategy_id.empty() && !account_id.empty()) {
                    // 前端直接注册账户（无策略），用 account_id 作为 registry key
                    strategy_id = account_id;
                    success = g_account_registry.register_account(
                        strategy_id, ex_type, api_key, secret_key, passphrase, is_testnet, account_id
                    );
                } else if (strategy_id.empty()) {
                    // 无 strategy_id 也无 account_id，注册为默认账户
                    if (ex_type == ExchangeType::OKX) {
                        g_account_registry.set_default_okx_account(api_key, secret_key, passphrase, is_testnet, account_id);
                        success = true;
                    } else if (ex_type == ExchangeType::BINANCE) {
                        g_account_registry.set_default_binance_account(api_key, secret_key, is_testnet, binance::MarketType::FUTURES, account_id);
                        success = true;
                    }
                } else {
                    success = g_account_registry.register_account(
                        strategy_id, ex_type, api_key, secret_key, passphrase, is_testnet, account_id
                    );
                }

                if (success) {
                    // 验证 API 有效性
                    std::string check_id = strategy_id.empty() ? "" : strategy_id;
                    bool api_valid = false;
                    std::string error_msg;

                    if (!check_id.empty()) {
                        try {
                            api_valid = g_account_registry.health_check(check_id, ex_type);
                            if (!api_valid) {
                                error_msg = "API 验证失败，请检查 API Key 和权限设置";
                            }
                        } catch (const std::exception& e) {
                            error_msg = std::string("API 验证异常: ") + e.what();
                        }
                    } else {
                        // 默认账户也验证
                        try {
                            if (ex_type == ExchangeType::OKX) {
                                auto* api = g_account_registry.get_okx_api("");
                                if (api) {
                                    auto result = api->get_account_balance();
                                    api_valid = result.contains("code") && result["code"].get<std::string>() == "0";
                                    if (!api_valid) {
                                        error_msg = result.contains("msg") ? result["msg"].get<std::string>() : "API 验证失败";
                                    }
                                }
                            } else if (ex_type == ExchangeType::BINANCE) {
                                auto* api = g_account_registry.get_binance_api("");
                                if (api) {
                                    int64_t t = api->get_server_time();
                                    api_valid = (t > 0);
                                    if (!api_valid) error_msg = "无法连接 Binance 服务器";
                                }
                            }
                        } catch (const std::exception& e) {
                            error_msg = std::string("API 验证异常: ") + e.what();
                        }
                    }

                    if (!api_valid) {
                        // API 无效，回滚注册
                        if (!strategy_id.empty()) {
                            g_account_registry.unregister_account(strategy_id, ex_type);
                        }
                        response = {{"success", false}, {"message", error_msg}};
                        LOG_INFO("[账户注册] ✗ API 验证失败: " + error_msg);
                    } else {
                        // API 有效，添加到账户监控器
                        if (g_account_monitor) {
                            if (ex_type == ExchangeType::OKX) {
                                auto* api = g_account_registry.get_okx_api(strategy_id);
                                if (api) {
                                    trading::server::AccountCredentials credentials(api_key, secret_key, passphrase, is_testnet);
                                    g_account_monitor->register_okx_account(strategy_id, api, &credentials, account_id);
                                }
                            } else if (ex_type == ExchangeType::BINANCE) {
                                auto* api = g_account_registry.get_binance_api(strategy_id);
                                if (api) {
                                    trading::server::AccountCredentials credentials(api_key, secret_key, "", is_testnet);
                                    g_account_monitor->register_binance_account(strategy_id, api, &credentials, account_id);
                                }
                            }
                        }

                        // 保存账户配置文件到磁盘
                        if (!account_id.empty()) {
                            if (save_account_config_file(exchange, account_id, api_key, secret_key, passphrase, is_testnet)) {
                                LOG_INFO("[账户注册] 配置文件已保存: " + exchange + "_" + account_id + ".json");
                            } else {
                                LOG_ERROR("[账户注册] 配置文件保存失败: " + exchange + "_" + account_id + ".json");
                            }
                        }

                        response = {{"success", true}, {"message", "账户注册成功，API 验证通过"}};
                        LOG_INFO("[账户注册] ✓ " + (strategy_id.empty() ? "默认账户" : strategy_id) + " 注册成功，API 验证通过");
                    }
                } else {
                    response = {{"success", false}, {"message", "注册失败"}};
                    LOG_INFO("[账户注册] ✗ " + strategy_id + " 注册失败");
                }
            }
        }
        else if (action == "unregister_account") {
            // 注销账户
            std::string strategy_id = data.value("strategy_id", "");
            std::string account_id = data.value("account_id", "");
            std::string exchange = data.value("exchange", "okx");
            // 统一转小写，避免 "Binance" vs "binance" 导致文件名不匹配
            std::transform(exchange.begin(), exchange.end(), exchange.begin(), ::tolower);

            if (strategy_id.empty() && account_id.empty()) {
                response = {{"success", false}, {"message", "缺少 strategy_id 或 account_id"}};
            } else {
                // 获取 account_id: 优先从请求中取，否则从 strategy_manager 查，最后用 strategy_id 本身
                if (account_id.empty()) {
                    account_id = g_strategy_manager.get_account_id(strategy_id);
                }
                if (account_id.empty()) {
                    account_id = strategy_id;
                }
                // 如果只有 account_id 没有 strategy_id，用 account_id 作为 registry key
                if (strategy_id.empty()) {
                    strategy_id = account_id;
                }

                ExchangeType ex_type = string_to_exchange_type(exchange);
                bool success = g_account_registry.unregister_account(strategy_id, ex_type);

                if (success && g_account_monitor) {
                    if (ex_type == ExchangeType::OKX) {
                        g_account_monitor->unregister_okx_account(strategy_id);
                    } else if (ex_type == ExchangeType::BINANCE) {
                        g_account_monitor->unregister_binance_account(strategy_id);
                    }
                }

                // 停止并删除该账户下的所有策略进程
                if (!account_id.empty()) {
                    auto removed = g_strategy_manager.stop_and_remove_by_account(account_id);
                    for (const auto& sid : removed) {
                        LOG_INFO("[账户注销] 联动停止策略: " + sid + " (account: " + account_id + ")");
                    }
                }

                // 删除账户配置文件
                if (!account_id.empty()) {
                    std::string del_path = get_account_config_path(exchange, account_id);
                    LOG_INFO("[账��注销] 尝试删除配置文件: " + del_path + " (exchange=" + exchange + ", account_id=" + account_id + ", exists=" + (std::filesystem::exists(del_path) ? "true" : "false") + ")");
                    if (delete_account_config_file(exchange, account_id)) {
                        LOG_INFO("[账户注销] 账户配置文件已删除: " + exchange + "_" + account_id + ".json");
                    } else {
                        LOG_ERROR("[账户注销] 账户配置文件删除失败: " + del_path);
                    }
                } else {
                    LOG_INFO("[账户注销] account_id 为空，跳过配置文件删除");
                }

                // 级联删除该账户下的所有策略配置文件（扫描 configs/ 和 strategy_configs/）
                if (!account_id.empty()) {
                    std::vector<std::string> cascade_dirs = {
                        get_strategy_config_dir(),
                        get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_config_dir
                    };
                    for (const auto& dir : cascade_dirs) {
                        try {
                            if (std::filesystem::exists(dir)) {
                                for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                                    if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                                    try {
                                        std::ifstream f(entry.path().string());
                                        nlohmann::json cfg;
                                        f >> cfg;
                                        f.close();
                                        if (cfg.value("account_id", "") == account_id) {
                                            std::filesystem::remove(entry.path());
                                            LOG_INFO("[账户注销] 联动删除策略配置: " + entry.path().string());
                                        }
                                    } catch (...) {}
                                }
                            }
                        } catch (...) {}
                    }
                }

                response["success"] = success;
                response["message"] = success ? "账户注销成功" : "账户未找到";
                LOG_INFO("[账户注销] " + strategy_id + " " + (success ? "成功" : "失败"));
            }
        }
        else if (action == "list_accounts") {
            // 列出所有已注册账户
            auto accounts_info = g_account_registry.get_all_accounts_info();

            // 如果内存中无账户，回退读取磁盘配置文件
            size_t total = 0;
            if (accounts_info.contains("okx")) total += accounts_info["okx"].size();
            if (accounts_info.contains("binance")) total += accounts_info["binance"].size();
            if (total == 0) {
                accounts_info = load_all_account_configs();
                LOG_INFO("[list_accounts] 内存无账户，从磁盘加载 " + std::to_string(
                    accounts_info["okx"].size() + accounts_info["binance"].size()) + " 个账户");
            }

            LOG_INFO("[list_accounts] 返回账户数据: " + accounts_info.dump());
            response = {
                {"success", true},
                {"type", "accounts_list"},
                {"data", accounts_info}
            };
        }
        else if (action == "list_strategy_configs") {
            frontend_debug("[list_strategy_configs] 进入处理");
            // 列出所有策略配置文件
            std::string config_dir = get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_config_dir;
            frontend_debug("[list_strategy_configs] 原始路径: " + config_dir);
            // 规范化路径，解析 .. 等
            try {
                config_dir = std::filesystem::canonical(config_dir).string();
            } catch (const std::exception& e) {
                frontend_debug("[list_strategy_configs] canonical失败: " + std::string(e.what()));
            }
            frontend_debug("[list_strategy_configs] 最终路径: " + config_dir);
            frontend_debug("[list_strategy_configs] 路径存在: " + std::string(std::filesystem::exists(config_dir) ? "是" : "否"));
            nlohmann::json configs_json = nlohmann::json::array();

            try {
                if (std::filesystem::exists(config_dir) && std::filesystem::is_directory(config_dir)) {
                    for (const auto& entry : std::filesystem::directory_iterator(config_dir)) {
                        if (entry.is_regular_file() && entry.path().extension() == ".json") {
                            try {
                                std::ifstream file(entry.path().string());
                                nlohmann::json config;
                                file >> config;

                                // 返回配置摘要（不暴露密钥）
                                nlohmann::json item;
                                item["filename"] = entry.path().filename().string();
                                item["strategy_id"] = config.value("strategy_id", entry.path().stem().string());
                                item["account_id"] = config.value("account_id", "");
                                item["strategy_name"] = config.value("strategy_name", "");
                                item["exchange"] = config.value("exchange", "okx");
                                item["is_testnet"] = config.value("is_testnet", true);
                                item["enabled"] = config.value("enabled", true);
                                item["description"] = config.value("description", "");
                                if (config.contains("params")) {
                                    item["params"] = config["params"];
                                }
                                configs_json.push_back(item);
                            } catch (const std::exception& e) {
                                LOG_INFO("[list_strategy_configs] 跳过无效配置: " + entry.path().filename().string() + " - " + e.what());
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                LOG_INFO("[list_strategy_configs] 扫描目录失败: " + std::string(e.what()));
            }

            response = {
                {"success", true},
                {"data", configs_json}
            };
            LOG_INFO("[list_strategy_configs] 返回 " + std::to_string(configs_json.size()) + " 个配置");
        }
        else if (action == "create_strategy") {
            // 策略管理员禁止创建策略
            auth::TokenInfo caller;
            if (get_client_auth(client_id, caller) && caller.role == auth::UserRole::STRATEGY_MANAGER) {
                response = {{"success", false}, {"message", "策略管理员无权创建策略"}};
                if (!request_id.empty()) response["requestId"] = request_id;
                g_frontend_server->send_response(client_id, false, "策略管理员无权创建策略", response);
                return;
            }
            // 创建策略：从模板复制配置 + 注入账户凭证 + 可选自动启动
            std::string config_file = data.value("config_file", "");
            std::string strategy_id = data.value("strategy_id", "");
            std::string account_id = data.value("account_id", "");
            std::string strategy_name = data.value("strategy_name", "");
            std::string symbol = data.value("symbol", "");
            std::string python_file = data.value("python_file", "");
            std::string qq_email = data.value("qq_email", "");
            std::string lark_email = data.value("lark_email", "");
            std::string description = data.value("description", "");
            bool auto_start = data.value("auto_start", false);

            if (config_file.empty() || strategy_id.empty() || account_id.empty()) {
                response = {{"success", false}, {"message", "缺少必要参数 (config_file, strategy_id, account_id)"}};
            } else {
                std::string config_dir = get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_config_dir;
                std::string template_path = config_dir + "/" + config_file;

                try {
                    // 1. 读取模板配置文件（只读，不修改模板）
                    std::ifstream infile(template_path);
                    if (!infile.is_open()) {
                        response = {{"success", false}, {"message", "模板配置文件不存在: " + config_file}};
                    } else {
                        nlohmann::json config;
                        infile >> config;
                        infile.close();

                        // 2. 读取账户配置文件获取凭证
                        std::string acct_dir = get_account_config_dir();
                        std::string acct_exchange;
                        std::string acct_api_key;
                        std::string acct_secret_key;
                        std::string acct_passphrase;
                        bool acct_is_testnet = true;
                        bool found_account = false;

                        // 遍历 acount_configs/ 目录按 account_id 匹配
                        if (std::filesystem::exists(acct_dir)) {
                            for (const auto& entry : std::filesystem::directory_iterator(acct_dir)) {
                                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                                try {
                                    std::ifstream af(entry.path().string());
                                    nlohmann::json acfg;
                                    af >> acfg;
                                    af.close();
                                    if (acfg.value("account_id", "") == account_id) {
                                        acct_exchange = acfg.value("exchange", "");
                                        acct_api_key = acfg.value("api_key", "");
                                        acct_secret_key = acfg.value("secret_key", "");
                                        acct_passphrase = acfg.value("passphrase", "");
                                        acct_is_testnet = acfg.value("is_testnet", true);
                                        found_account = true;
                                        break;
                                    }
                                } catch (...) {}
                            }
                        }

                        if (!found_account) {
                            response = {{"success", false}, {"message", "未找到账户配置: " + account_id}};
                        } else {
                            // 3. 覆盖字段
                            config["strategy_id"] = strategy_id;
                            config["account_id"] = account_id;
                            if (!strategy_name.empty()) config["strategy_name"] = strategy_name;
                            if (!description.empty()) config["description"] = description;

                            // 注入账户凭证
                            config["exchange"] = acct_exchange;
                            config["api_key"] = acct_api_key;
                            config["secret_key"] = acct_secret_key;
                            if (acct_exchange == "okx") {
                                config["passphrase"] = acct_passphrase;
                            } else {
                                config["passphrase"] = "";
                            }
                            config["is_testnet"] = acct_is_testnet;

                            // 设置交易对（如果指定了）
                            if (!symbol.empty()) {
                                config["symbols"] = nlohmann::json::array({symbol});
                            }

                            // 设置联系人
                            if (!qq_email.empty() || !lark_email.empty()) {
                                nlohmann::json contact;
                                if (config.contains("contacts") && config["contacts"].is_array() && !config["contacts"].empty()) {
                                    contact = config["contacts"][0];
                                }
                                if (!qq_email.empty()) contact["email"] = qq_email;
                                if (!lark_email.empty()) contact["lark_email"] = lark_email;
                                config["contacts"] = nlohmann::json::array({contact});
                            }

                            // 4. 生成新文件名: {exchange}_{account_id}_{strategy_id}.json
                            //    前端生成的策略配置存放到 strategy_configs/ 目录（区别于模板 configs/）
                            //    同时保存 python_file 到配置中，以便重启后恢复启动命令
                            if (!python_file.empty()) {
                                config["python_file"] = python_file;
                            }
                            std::string new_filename = acct_exchange + "_" + account_id + "_" + strategy_id + ".json";
                            std::string strategy_cfg_dir = get_strategy_config_dir();
                            std::filesystem::create_directories(strategy_cfg_dir);
                            std::string new_config_path = strategy_cfg_dir + "/" + new_filename;

                            // 检查文件是否已存在
                            if (std::filesystem::exists(new_config_path)) {
                                response = {{"success", false}, {"message", "配置文件已存在: " + new_filename + "，请使用不同的策略ID"}};
                            } else {
                                std::ofstream outfile(new_config_path);
                                if (!outfile.is_open()) {
                                    response = {{"success", false}, {"message", "无法写入配置文件: " + new_filename}};
                                } else {
                                    outfile << config.dump(2);
                                    outfile.close();

                                    // 重新加载配置（两个目录都加载）
                                    trading::StrategyConfigManager::instance().load_configs(config_dir);
                                    trading::StrategyConfigManager::instance().load_configs(strategy_cfg_dir);

                                    LOG_INFO("[create_strategy] ✓ " + strategy_id + " (文件: " + new_filename + ", 账户: " + account_id + ")");

                                    // 5. 构建启动命令并注册到 StrategyProcessManager
                                    std::string strategy_source_dir = get_exe_dir() + "/" +
                                        trading::config::ConfigCenter::instance().server().strategy_source_dir;
                                    std::string abs_config_path = std::filesystem::canonical(new_config_path).string();
                                    std::string abs_py_dir = std::filesystem::canonical(strategy_source_dir).string();

                                    // 启动命令: cd {py_dir} && python3 {python_file} --config {config_path} [--symbol {symbol}]
                                    std::string start_cmd = "cd " + abs_py_dir + " && python3 " + python_file + " --config " + abs_config_path;
                                    if (!symbol.empty()) {
                                        start_cmd += " --symbol " + symbol;
                                    }

                                    // 注册策略（状态为 stopped，等待启动）
                                    g_strategy_manager.register_strategy(strategy_id, 0, account_id, acct_exchange, start_cmd, abs_py_dir);
                                    // 标记为 stopped（因为还没启动）
                                    g_strategy_manager.stop_strategy(strategy_id);

                                    // 6. 如果自动启动
                                    if (auto_start && !python_file.empty()) {
                                        auto [ok, msg, pid] = g_strategy_manager.start_strategy(strategy_id);
                                        if (ok) {
                                            LOG_INFO("[create_strategy] 自动启动成功, PID=" + std::to_string(pid));
                                            response = {{"success", true}, {"message", "策略创建并启动成功, PID=" + std::to_string(pid)}, {"pid", pid}};
                                        } else {
                                            LOG_INFO("[create_strategy] 自动启动失败: " + msg);
                                            response = {{"success", true}, {"message", "策略创建成功，但自动启动失败: " + msg}};
                                        }
                                    } else {
                                        response = {{"success", true}, {"message", "策略创建成功: " + new_filename}};
                                    }
                                }
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    response = {{"success", false}, {"message", "创建失败: " + std::string(e.what())}};
                    LOG_ERROR("[create_strategy] ✗ 失败: " + std::string(e.what()));
                }
            }
        }
        else if (action == "list_strategies") {
            // 列出所有策略进程（运行中 + 已停止）
            auto strategies_info = g_strategy_manager.get_all_info();

            // 策略管理员过滤：只返回 allowed_strategies 中的策略
            auth::TokenInfo caller;
            if (get_client_auth(client_id, caller) && caller.role == auth::UserRole::STRATEGY_MANAGER) {
                auto allowed = get_allowed_strategies(caller.username, caller.role);
                nlohmann::json filtered = nlohmann::json::array();
                for (const auto& s : strategies_info) {
                    std::string sid = s.value("strategy_id", s.value("id", ""));
                    if (is_strategy_allowed(allowed, sid)) {
                        filtered.push_back(s);
                    }
                }
                strategies_info = filtered;
            }

            response = {
                {"success", true},
                {"type", "strategies_list"},
                {"data", strategies_info},
                {"running_count", g_strategy_manager.running_count()},
                {"total_count", g_strategy_manager.total_count()}
            };
            LOG_INFO("[list_strategies] 返回 " + std::to_string(strategies_info.size()) + " 个策略");
        }
        else if (action == "stop_strategy") {
            // 中止策略进程（发送 SIGTERM）
            std::string strategy_id = data.value("strategy_id", "");
            if (strategy_id.empty()) {
                response = {{"success", false}, {"message", "缺少 strategy_id"}};
            } else {
                // 策略管理员权限检查
                auth::TokenInfo caller;
                if (get_client_auth(client_id, caller) && caller.role == auth::UserRole::STRATEGY_MANAGER) {
                    auto allowed = get_allowed_strategies(caller.username, caller.role);
                    if (!is_strategy_allowed(allowed, strategy_id)) {
                        response = {{"success", false}, {"message", "无权操作此策略"}};
                        if (!request_id.empty()) response["requestId"] = request_id;
                        g_frontend_server->send_response(client_id, false, "无权操作此策略", response);
                        return;
                    }
                }
                bool success = g_strategy_manager.stop_strategy(strategy_id);
                response["success"] = success;
                response["message"] = success ? "策略已中止" : "策略未找到或已停止";
                LOG_INFO("[stop_strategy] " + strategy_id + " " + (success ? "成功" : "失败"));
            }
        }
        else if (action == "start_strategy") {
            // 从前端启动/重启策略进程
            std::string strategy_id = data.value("strategy_id", data.value("id", ""));
            if (strategy_id.empty()) {
                response = {{"success", false}, {"message", "缺少 strategy_id"}};
            } else {
                // 策略管理员权限检查
                auth::TokenInfo caller;
                if (get_client_auth(client_id, caller) && caller.role == auth::UserRole::STRATEGY_MANAGER) {
                    auto allowed = get_allowed_strategies(caller.username, caller.role);
                    if (!is_strategy_allowed(allowed, strategy_id)) {
                        response = {{"success", false}, {"message", "无权操作此策略"}};
                        if (!request_id.empty()) response["requestId"] = request_id;
                        g_frontend_server->send_response(client_id, false, "无权操作此策略", response);
                        return;
                    }
                }
                auto [success, message, pid] = g_strategy_manager.start_strategy(strategy_id);
                response["success"] = success;
                response["message"] = message;
                if (pid > 0) {
                    response["pid"] = pid;
                }
                LOG_INFO("[start_strategy] " + strategy_id + " " + (success ? "成功" : "失败") + ": " + message);
            }
        }
        else if (action == "delete_strategy") {
            // 策略管理员禁止删除策略
            auth::TokenInfo caller;
            if (get_client_auth(client_id, caller) && caller.role == auth::UserRole::STRATEGY_MANAGER) {
                response = {{"success", false}, {"message", "策略管理员无权删除策略"}};
                if (!request_id.empty()) response["requestId"] = request_id;
                g_frontend_server->send_response(client_id, false, "策略管理员无权删除策略", response);
                return;
            }
            // 删除策略：停止进程 + 删除配置文件 + 从进程管理器移除
            std::string strategy_id = data.value("strategy_id", data.value("id", ""));
            if (strategy_id.empty()) {
                response = {{"success", false}, {"message", "缺少 strategy_id"}};
            } else {
                // 1. 如果策略正在运行，先停止
                g_strategy_manager.stop_strategy(strategy_id);

                // 2. 从进程管理器中移除
                g_strategy_manager.unregister_strategy(strategy_id);

                // 3. 删除策略配置文件（优先扫描 strategy_configs/，再扫描 configs/）
                bool file_deleted = false;
                std::vector<std::string> scan_dirs = {
                    get_strategy_config_dir(),
                    get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_config_dir
                };
                for (const auto& scan_dir : scan_dirs) {
                    if (file_deleted) break;
                    try {
                        if (std::filesystem::exists(scan_dir)) {
                            for (const auto& entry : std::filesystem::directory_iterator(scan_dir)) {
                                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                                try {
                                    std::ifstream f(entry.path().string());
                                    nlohmann::json cfg;
                                    f >> cfg;
                                    f.close();
                                    if (cfg.value("strategy_id", "") == strategy_id) {
                                        std::filesystem::remove(entry.path());
                                        LOG_INFO("[delete_strategy] 已删除配置文件: " + entry.path().string());
                                        file_deleted = true;
                                        break;
                                    }
                                } catch (...) {}
                            }
                        }
                    } catch (...) {}
                }

                // 重新加载配置（两个目录）
                trading::StrategyConfigManager::instance().load_configs(
                    get_exe_dir() + "/" + trading::config::ConfigCenter::instance().server().strategy_config_dir);
                trading::StrategyConfigManager::instance().load_configs(get_strategy_config_dir());

                response = {{"success", true}, {"message", file_deleted ? "策略已删除（含配置文件）" : "策略已删除"}};
                LOG_INFO("[delete_strategy] " + strategy_id + (file_deleted ? " (配置文件已删除)" : " (无配置文件)"));
            }
        }
        // ==================== 用户管理命令 ====================
        else if (action == "list_users") {
            auth::TokenInfo caller;
            if (!get_client_auth(client_id, caller) || caller.role != auth::UserRole::SUPER_ADMIN) {
                response = {{"success", false}, {"message", "无权限"}};
            } else {
                nlohmann::json users = g_auth_manager.get_users();
                response = {{"success", true}, {"data", users}};
            }
        }
        else if (action == "add_user") {
            auth::TokenInfo caller;
            if (!get_client_auth(client_id, caller) || caller.role != auth::UserRole::SUPER_ADMIN) {
                response = {{"success", false}, {"message", "无权限"}};
            } else {
                std::string new_username = data.value("username", "");
                std::string new_password = data.value("password", "");
                std::string role_str = data.value("role", "STRATEGY_MANAGER");
                std::vector<std::string> allowed_strategies;
                if (data.contains("allowed_strategies") && data["allowed_strategies"].is_array()) {
                    for (const auto& s : data["allowed_strategies"]) {
                        allowed_strategies.push_back(s.get<std::string>());
                    }
                }
                if (new_username.empty() || new_password.empty()) {
                    response = {{"success", false}, {"message", "用户名和密码不能为空"}};
                } else {
                    auth::UserRole new_role = auth::AuthManager::string_to_role(role_str);
                    bool ok = g_auth_manager.add_user(new_username, new_password, new_role, allowed_strategies);
                    response = {{"success", ok}, {"message", ok ? "用户创建成功" : "用户已存在"}};
                    LOG_INFO("[用户管理] " + caller.username + " 创建用户: " + new_username + " " + (ok ? "成功" : "失败"));
                }
            }
        }
        else if (action == "delete_user") {
            auth::TokenInfo caller;
            if (!get_client_auth(client_id, caller) || caller.role != auth::UserRole::SUPER_ADMIN) {
                response = {{"success", false}, {"message", "无权限"}};
            } else {
                std::string del_username = data.value("username", "");
                bool ok = g_auth_manager.delete_user(del_username);
                response = {{"success", ok}, {"message", ok ? "用户已删除" : "删除失败（用户不存在或为超级管理员）"}};
                LOG_INFO("[用户管理] " + caller.username + " 删除用户: " + del_username + " " + (ok ? "成功" : "失败"));
            }
        }
        else if (action == "update_user") {
            auth::TokenInfo caller;
            if (!get_client_auth(client_id, caller) || caller.role != auth::UserRole::SUPER_ADMIN) {
                response = {{"success", false}, {"message", "无权限"}};
            } else {
                std::string upd_username = data.value("username", "");
                std::vector<std::string> allowed_strategies;
                if (data.contains("allowed_strategies") && data["allowed_strategies"].is_array()) {
                    for (const auto& s : data["allowed_strategies"]) {
                        allowed_strategies.push_back(s.get<std::string>());
                    }
                }
                bool ok = g_auth_manager.update_user(upd_username, allowed_strategies);
                response = {{"success", ok}, {"message", ok ? "用户更新成功" : "用户不存在"}};
                LOG_INFO("[用户管理] " + caller.username + " 更新用户: " + upd_username + " " + (ok ? "成功" : "失败"));
            }
        }
        else if (action == "change_password") {
            auth::TokenInfo caller;
            if (!get_client_auth(client_id, caller)) {
                response = {{"success", false}, {"message", "未登录"}};
            } else {
                std::string old_password = data.value("old_password", "");
                std::string new_password = data.value("new_password", "");
                if (old_password.empty() || new_password.empty()) {
                    response = {{"success", false}, {"message", "密码不能为空"}};
                } else {
                    bool ok = g_auth_manager.change_password(caller.username, old_password, new_password);
                    response = {{"success", ok}, {"message", ok ? "密码修改成功" : "旧密码错误"}};
                    LOG_INFO("[用户管理] " + caller.username + " 修改密码 " + (ok ? "成功" : "失败"));
                }
            }
        }
        else {
            response = {{"success", false}, {"message", "未知命令: " + action}};
        }

        if (!request_id.empty()) {
            response["requestId"] = request_id;
        }

        frontend_debug("<<< 发送响应 action=" + action + " requestId=" + request_id + " success=" + std::string(response.value("success", false) ? "true" : "false") + " data_keys=" + std::to_string(response.size()));

        g_frontend_server->send_response(client_id, response.value("success", false),
                                        response.value("message", ""), response);

    } catch (const std::exception& e) {
        frontend_debug("!!! 异常 " + std::string(e.what()));
        std::cerr << "[前端] 处理命令异常: " << e.what() << "\n";
        g_frontend_server->send_response(client_id, false,
                                        std::string("处理命令异常: ") + e.what(), {});
    }
}

} // namespace server
} // namespace trading
