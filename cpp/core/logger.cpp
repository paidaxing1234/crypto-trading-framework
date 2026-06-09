#include "logger.h"
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>

namespace trading {
namespace core {

void Logger::init(const std::string& log_dir,
                  const std::string& log_prefix,
                  LogLevel level,
                  size_t max_file_size) {
    log_dir_ = log_dir;
    log_prefix_ = log_prefix;
    min_level_ = level;
    max_file_size_ = max_file_size;

    // 创建日志目录
    mkdir(log_dir_.c_str(), 0755);

    // 启动写入线程
    running_.store(true);
    write_thread_ = std::thread(&Logger::write_thread_func, this);

    info("日志系统已初始化，日志目录: " + log_dir);
}

void Logger::debug(const std::string& msg) {
    log(LogLevel::DEBUG, "system", msg);
}

void Logger::info(const std::string& msg) {
    log(LogLevel::INFO, "system", msg);
}

void Logger::warn(const std::string& msg) {
    log(LogLevel::WARN, "system", msg);
}

void Logger::error(const std::string& msg) {
    log(LogLevel::ERROR, "system", msg);
}

// 带来源的日志方法
void Logger::debug(const std::string& source, const std::string& msg) {
    log(LogLevel::DEBUG, source, msg);
}

void Logger::info(const std::string& source, const std::string& msg) {
    log(LogLevel::INFO, source, msg);
}

void Logger::warn(const std::string& source, const std::string& msg) {
    log(LogLevel::WARN, source, msg);
}

void Logger::error(const std::string& source, const std::string& msg) {
    log(LogLevel::ERROR, source, msg);
}

void Logger::audit(const std::string& action, const std::string& details) {
    std::string audit_msg = "[AUDIT] " + action + " | " + details;
    log(LogLevel::INFO, "system", audit_msg);
}

void Logger::audit(const std::string& source, const std::string& action, const std::string& details) {
    std::string audit_msg = "[AUDIT] " + action + " | " + details;
    log(LogLevel::INFO, source, audit_msg);
}

void Logger::order_lifecycle(const std::string& order_id, const std::string& action, const std::string& details) {
    std::string order_msg = "[ORDER:" + order_id + "] " + action + " | " + details;
    log(LogLevel::INFO, "order", order_msg);
}

void Logger::order_lifecycle(const std::string& source, const std::string& order_id, const std::string& action, const std::string& details) {
    std::string order_msg = "[ORDER:" + order_id + "] " + action + " | " + details;
    log(LogLevel::INFO, source, order_msg);
}

void Logger::log(LogLevel level, const std::string& msg) {
    log(level, "system", msg);
}

void Logger::log(LogLevel level, const std::string& source, const std::string& msg) {
    if (level < min_level_) {
        return;
    }

    std::string log_line = "[" + get_timestamp() + "] "
                         + "[" + level_to_string(level) + "] "
                         + "[" + source + "] "
                         + msg;

    // 控制台输出
    if (console_output_) {
        if (level >= LogLevel::ERROR) {
            std::cerr << log_line << std::endl;
        } else {
            std::cout << log_line << std::endl;
        }
    }

    // WebSocket 推送
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (ws_callback_) {
            ws_callback_(level_to_string(level), source, msg);
        }
    }

    // 加入队列
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        log_queue_.push(log_line);
    }
    queue_cv_.notify_one();
}

void Logger::write_thread_func() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
            return !log_queue_.empty() || !running_.load();
        });

        while (!log_queue_.empty()) {
            std::string log_line = log_queue_.front();
            log_queue_.pop();
            lock.unlock();

            // 解析日志行，提取 source
            // 格式: [timestamp] [level] [source] message
            std::string source = "system";
            size_t first_bracket = log_line.find('[');
            if (first_bracket != std::string::npos) {
                size_t second_bracket = log_line.find('[', first_bracket + 1);
                if (second_bracket != std::string::npos) {
                    size_t third_bracket = log_line.find('[', second_bracket + 1);
                    if (third_bracket != std::string::npos) {
                        size_t close_bracket = log_line.find(']', third_bracket);
                        if (close_bracket != std::string::npos) {
                            source = log_line.substr(third_bracket + 1, close_bracket - third_bracket - 1);
                        }
                    }
                }
            }

            // 写入对应的源文件
            write_to_source_file(source, log_line);

            lock.lock();
        }
    }
}

std::string Logger::get_current_date_str() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y%m%d");
    return ss.str();
}

std::string Logger::get_source_log_filename(const std::string& source) {
    // 将 source 映射到文件名，按天分割
    // system -> main_YYYYMMDD.log
    // 其他 -> {source}_YYYYMMDD.log
    std::string date_str = get_current_date_str();
    std::string base_name;
    if (source == "system" || source == "order" || source.empty()) {
        base_name = "main";
    } else {
        base_name = source;
    }
    return log_dir_ + "/" + base_name + "_" + date_str + ".log";
}

std::ofstream& Logger::get_or_create_source_file(const std::string& source) {
    std::lock_guard<std::mutex> lock(source_files_mutex_);

    std::string current_date = get_current_date_str();
    auto it = source_files_.find(source);

    // 检查是否需要跨天切换
    if (it != source_files_.end() && it->second.file.is_open()) {
        if (it->second.date_str == current_date) {
            return it->second.file;
        }
        // 日期变了，关闭旧文件
        it->second.file.close();
    }

    // 创建新文件
    SourceFileInfo& info = source_files_[source];
    info.filename = get_source_log_filename(source);
    info.date_str = current_date;
    info.file.open(info.filename, std::ios::app);

    if (info.file.is_open()) {
        info.file.seekp(0, std::ios::end);
        info.size = info.file.tellp();
    }

    return info.file;
}

void Logger::write_to_source_file(const std::string& source, const std::string& log_line) {
    std::lock_guard<std::mutex> lock(source_files_mutex_);

    std::string current_date = get_current_date_str();

    auto it = source_files_.find(source);
    if (it == source_files_.end()) {
        // 创建新文件
        SourceFileInfo& info = source_files_[source];
        info.filename = get_source_log_filename(source);
        info.date_str = current_date;

        info.file.open(info.filename, std::ios::app);

        if (!info.file.is_open()) {
            std::cerr << "[Logger] 无法打开日志文件: " << info.filename << std::endl;
            return;
        }

        info.file.seekp(0, std::ios::end);
        info.size = info.file.tellp();
        it = source_files_.find(source);
    }

    SourceFileInfo& info = it->second;

    // 检查是否跨天，需要切换到新文件
    if (info.date_str != current_date) {
        if (info.file.is_open()) {
            info.file.close();
        }
        info.filename = get_source_log_filename(source);
        info.date_str = current_date;
        info.file.open(info.filename, std::ios::app);
        if (info.file.is_open()) {
            info.file.seekp(0, std::ios::end);
            info.size = info.file.tellp();
        } else {
            std::cerr << "[Logger] 无法打开日志文件: " << info.filename << std::endl;
            return;
        }
    }

    if (info.file.is_open()) {
        info.file << log_line << std::endl;
        info.file.flush();  // 立即刷新到磁盘
        info.size += log_line.size() + 1;

        // 检查是否需要轮转（单日内超大文件仍按大小轮转）
        if (info.size >= max_file_size_) {
            info.file.close();

            // 重命名旧文件
            std::string new_filename = info.filename + "." + get_timestamp();
            rename(info.filename.c_str(), new_filename.c_str());

            // 打开新文件
            info.file.open(info.filename, std::ios::app);
            info.size = 0;
        }
    } else {
        std::cerr << "[Logger] 文件未打开: " << info.filename << std::endl;
    }
}

void Logger::rotate_if_needed() {
    log_file_.close();

    // 重命名旧文件
    std::string old_filename = get_log_filename();
    std::string new_filename = old_filename + "." + get_timestamp();
    rename(old_filename.c_str(), new_filename.c_str());

    // 打开新文件
    log_file_.open(old_filename, std::ios::app);
    current_file_size_ = 0;
}

std::string Logger::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

std::string Logger::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

std::string Logger::get_log_filename() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << log_dir_ << "/" << log_prefix_ << "_"
       << std::put_time(std::localtime(&time_t), "%Y%m%d") << ".log";
    return ss.str();
}

void Logger::shutdown() {
    if (running_.load()) {
        running_.store(false);
        queue_cv_.notify_all();
        if (write_thread_.joinable()) {
            write_thread_.join();
        }

        // 关闭所有源文件
        std::lock_guard<std::mutex> lock(source_files_mutex_);
        for (auto& pair : source_files_) {
            if (pair.second.file.is_open()) {
                pair.second.file.close();
            }
        }
        source_files_.clear();
    }
}

Logger::~Logger() {
    shutdown();
}

} // namespace core
} // namespace trading
