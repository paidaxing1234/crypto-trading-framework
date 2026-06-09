/**
 * @file logger.h
 * @brief 轻量级日志系统
 *
 * 功能：
 * 1. 多级别日志（DEBUG/INFO/WARN/ERROR）
 * 2. 文件日志持久化
 * 3. 日志轮转（按大小）
 * 4. 线程安全
 * 5. 高性能（异步写入）
 *
 * @author Sequence Team
 * @date 2025-01
 */

#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <memory>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <queue>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>

namespace trading {
namespace core {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void init(const std::string& log_dir = "logs",
              const std::string& log_prefix = "trading",
              LogLevel level = LogLevel::INFO,
              size_t max_file_size = 100 * 1024 * 1024);  // 100MB

    void set_level(LogLevel level) { min_level_ = level; }
    void set_console_output(bool enable) { console_output_ = enable; }

    void debug(const std::string& msg);
    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);

    // 带来源的日志方法
    void debug(const std::string& source, const std::string& msg);
    void info(const std::string& source, const std::string& msg);
    void warn(const std::string& source, const std::string& msg);
    void error(const std::string& source, const std::string& msg);

    // 审计日志
    void audit(const std::string& action, const std::string& details);
    void audit(const std::string& source, const std::string& action, const std::string& details);
    // 订单生命周期日志
    void order_lifecycle(const std::string& order_id, const std::string& action, const std::string& details);
    void order_lifecycle(const std::string& source, const std::string& order_id, const std::string& action, const std::string& details);

    void shutdown();

    // WebSocket 日志推送回调 (level, source, message)
    using LogCallback = std::function<void(const std::string& level, const std::string& source, const std::string& msg)>;
    void set_ws_callback(LogCallback callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        ws_callback_ = std::move(callback);
    }

    ~Logger();

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, const std::string& msg);
    void log(LogLevel level, const std::string& source, const std::string& msg);
    void write_thread_func();
    void rotate_if_needed();
    std::string get_timestamp();
    std::string level_to_string(LogLevel level);
    std::string get_log_filename();

    // 多文件日志辅助方法
    std::string get_source_log_filename(const std::string& source);
    std::ofstream& get_or_create_source_file(const std::string& source);
    void write_to_source_file(const std::string& source, const std::string& log_line);

    std::string log_dir_;
    std::string log_prefix_;
    LogLevel min_level_{LogLevel::INFO};
    size_t max_file_size_{100 * 1024 * 1024};
    bool console_output_{true};

    std::ofstream log_file_;
    std::mutex file_mutex_;
    size_t current_file_size_{0};

    std::queue<std::string> log_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<bool> running_{false};
    std::thread write_thread_;

    // WebSocket 回调
    std::mutex callback_mutex_;
    LogCallback ws_callback_;

    // 多文件日志支持：为每个 source 维护独立的文件（按天分割）
    struct SourceFileInfo {
        std::ofstream file;
        size_t size{0};
        std::string filename;
        std::string date_str;  // 当前文件对应的日期，格式 YYYYMMDD
    };
    std::map<std::string, SourceFileInfo> source_files_;
    std::mutex source_files_mutex_;

    // 获取当前日期字符串（YYYYMMDD）
    std::string get_current_date_str();
};

// 便捷宏
#define LOG_DEBUG(msg) trading::core::Logger::instance().debug(msg)
#define LOG_INFO(msg) trading::core::Logger::instance().info(msg)
#define LOG_WARN(msg) trading::core::Logger::instance().warn(msg)
#define LOG_ERROR(msg) trading::core::Logger::instance().error(msg)

// 审计日志宏
#define LOG_AUDIT(action, details) trading::core::Logger::instance().audit(action, details)
#define LOG_AUDIT_SRC(source, action, details) trading::core::Logger::instance().audit(source, action, details)
#define LOG_ORDER(order_id, action, details) trading::core::Logger::instance().order_lifecycle(order_id, action, details)
#define LOG_ORDER_SRC(source, order_id, action, details) trading::core::Logger::instance().order_lifecycle(source, order_id, action, details)

} // namespace core
} // namespace trading
