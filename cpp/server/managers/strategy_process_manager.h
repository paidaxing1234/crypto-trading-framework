/**
 * @file strategy_process_manager.h
 * @brief 策略进程管理器 - 独立于 AccountRegistry，追踪策略运行状态
 *
 * 通过心跳机制检测策略进程是否存活，支持前端中止策略（kill进程）。
 * 账户注册和策略运行是独立的生命周期：
 * - 策略停止 → 账户仍然注册
 * - 注销账户 → 需要手动操作
 */

#pragma once

#include <string>
#include <map>
#include <set>
#include <mutex>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

namespace trading {
namespace server {

struct StrategyProcessInfo {
    std::string strategy_id;
    std::string account_id;
    std::string exchange;
    pid_t pid = 0;
    std::string status = "running";  // "running" | "stopped" | "error"
    std::string start_command;       // 启动命令行（用于重启）
    std::string work_dir;            // 工作目录
    int64_t start_time = 0;
    int64_t last_heartbeat = 0;

    nlohmann::json to_json() const {
        return {
            {"strategy_id", strategy_id},
            {"account_id", account_id},
            {"exchange", exchange},
            {"pid", pid},
            {"status", status},
            {"start_command", start_command},
            {"work_dir", work_dir},
            {"start_time", start_time},
            {"last_heartbeat", last_heartbeat}
        };
    }
};

class StrategyProcessManager {
public:
    /**
     * @brief 注册一个策略进程（Python 策略启动时调用）
     */
    void register_strategy(const std::string& strategy_id, pid_t pid,
                           const std::string& account_id,
                           const std::string& exchange,
                           const std::string& start_command = "",
                           const std::string& work_dir = "",
                           const std::string& initial_status = "running") {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now_ms = current_timestamp_ms();

        StrategyProcessInfo info;
        info.strategy_id = strategy_id;
        info.account_id = account_id;
        info.exchange = exchange;
        info.pid = pid;
        info.status = initial_status;
        info.start_command = start_command;
        info.work_dir = work_dir;
        info.start_time = now_ms;
        info.last_heartbeat = now_ms;

        strategies_[strategy_id] = info;
    }

    /**
     * @brief 移除策略（注销时调用）
     */
    void unregister_strategy(const std::string& strategy_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        strategies_.erase(strategy_id);
    }

    /**
     * @brief 记录心跳
     */
    void record_heartbeat(const std::string& strategy_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = strategies_.find(strategy_id);
        if (it != strategies_.end()) {
            it->second.last_heartbeat = current_timestamp_ms();
            // 如果之前是 stopped/error 但又收到心跳，说明策略重新启动了
            // 但不覆盖 pending 状态（pending 表示尚未启动，需要用户手动启动）
            if (it->second.status == "stopped" || it->second.status == "error") {
                it->second.status = "running";
            }
        }
    }

    /**
     * @brief 中止策略进程（前端调用）
     * @return true 如果成功发送 SIGTERM
     */
    bool stop_strategy(const std::string& strategy_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = strategies_.find(strategy_id);
        if (it == strategies_.end()) {
            return false;
        }

        if (it->second.pid > 0 && it->second.status == "running") {
            int ret = kill(it->second.pid, SIGTERM);
            it->second.status = "stopped";
            return (ret == 0);
        }

        it->second.status = "stopped";
        return true;
    }

    /**
     * @brief 设置策略状态（如启动时加载的策略设为 "pending"）
     */
    void set_strategy_status(const std::string& strategy_id, const std::string& status) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = strategies_.find(strategy_id);
        if (it != strategies_.end()) {
            it->second.status = status;
        }
    }

    /**
     * @brief 重新启动策略进程（前端调用）
     * @return {success, message, pid}
     */
    std::tuple<bool, std::string, pid_t> start_strategy(const std::string& strategy_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = strategies_.find(strategy_id);
        if (it == strategies_.end()) {
            return {false, "策略未找到", 0};
        }

        if (it->second.status == "running") {
            return {false, "策略已在运行中", it->second.pid};
        }

        if (it->second.start_command.empty()) {
            return {false, "无启动命令记录，请手动运行 Python 策略脚本", 0};
        }

        // fork + exec 在后台启动策略
        std::string cmd = it->second.start_command;
        std::string cwd = it->second.work_dir;
        std::string sid = it->second.strategy_id;

        // 构建子进程日志文件路径（用于捕获 stdout/stderr）
        std::string log_dir = cwd;
        // 尝试找到 logs 目录（相对于 strategies/implementations 的上级 strategies/logs）
        std::string strategies_log_dir;
        {
            std::filesystem::path p(cwd);
            // cwd 通常是 .../strategies/implementations，上级是 strategies
            auto parent = p.parent_path();
            strategies_log_dir = (parent / "logs").string();
            try {
                std::filesystem::create_directories(strategies_log_dir);
            } catch (...) {
                strategies_log_dir = "/tmp";
            }
        }
        std::string child_log = strategies_log_dir + "/start_" + sid + ".log";

        pid_t child = fork();
        if (child < 0) {
            return {false, "fork 失败: " + std::string(strerror(errno)), 0};
        }

        if (child == 0) {
            // 子进程
            // 脱离父进程的会话，成为独立后台进程（在关闭 fd 之前执行）
            setsid();

            // 关闭从父进程继承的所有非标准文件描述符（避免占用 WebSocket 端口等）
            for (int fd = 3; fd < 1024; fd++) {
                close(fd);
            }

            // 重定向 stdout/stderr 到日志文件（用于诊断启动失败）
            int log_fd = open(child_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }

            // 关闭标准输入
            close(STDIN_FILENO);
            // 重新打开 stdin 为 /dev/null（避免 Python 读 stdin 出错）
            open("/dev/null", O_RDONLY);

            // 切换工作目录
            if (!cwd.empty()) {
                if (chdir(cwd.c_str()) != 0) {
                    fprintf(stderr, "chdir failed: %s\n", cwd.c_str());
                    _exit(1);
                }
            }

            // 用 sh -c 执行完整命令行
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            fprintf(stderr, "execl failed: %s\n", strerror(errno));
            _exit(1);  // exec 失败
        }

        // 父进程：短暂等待，检查子进程是否立即退出
        // 非阻塞等待 500ms
        usleep(500000);
        int wstatus;
        pid_t result = waitpid(child, &wstatus, WNOHANG);

        if (result == child) {
            // 子进程已经退出了 → 启动失败
            std::string err_msg = "策略进程启动后立即退出";
            // 尝试读取错误日志
            std::ifstream err_log(child_log);
            if (err_log.is_open()) {
                std::string content((std::istreambuf_iterator<char>(err_log)),
                                     std::istreambuf_iterator<char>());
                if (!content.empty()) {
                    // 截取最后 500 字符
                    if (content.size() > 500) content = content.substr(content.size() - 500);
                    err_msg += ": " + content;
                }
            }
            it->second.status = "error";
            return {false, err_msg, 0};
        }

        // 子进程仍在运行 → 启动成功
        auto now_ms = current_timestamp_ms();
        it->second.pid = child;
        it->second.status = "running";
        it->second.start_time = now_ms;
        it->second.last_heartbeat = now_ms;

        return {true, "策略已启动, PID=" + std::to_string(child), child};
    }

    /**
     * @brief 检查心跳超时，标记无心跳的策略为 stopped
     * @param timeout_sec 心跳超时时间（秒）
     */
    void check_heartbeats(int timeout_sec = 15) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now_ms = current_timestamp_ms();
        int64_t timeout_ms = timeout_sec * 1000LL;

        for (auto& [sid, info] : strategies_) {
            if (info.status == "running") {
                if (now_ms - info.last_heartbeat > timeout_ms) {
                    // 额外检查进程是否还存在
                    if (info.pid > 0 && kill(info.pid, 0) != 0) {
                        info.status = "stopped";
                    } else if (info.pid <= 0) {
                        info.status = "stopped";
                    }
                    // 如果 kill(pid, 0) == 0 说明进程还在，可能只是心跳延迟，
                    // 但超时足够长（15s），仍然标记为 stopped
                    else {
                        info.status = "stopped";
                    }
                }
            }
        }
    }

    /**
     * @brief 获取运行中的策略数量
     */
    size_t running_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& [sid, info] : strategies_) {
            if (info.status == "running") {
                count++;
            }
        }
        return count;
    }

    /**
     * @brief 获取唯一账户数量（去重 account_id）
     * 只统计有运行中策略或已注册的账户
     */
    size_t unique_account_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::set<std::string> accounts;
        for (const auto& [sid, info] : strategies_) {
            if (!info.account_id.empty()) {
                accounts.insert(info.account_id);
            }
        }
        return accounts.size();
    }

    /**
     * @brief 获取所有策略信息（供前端展示）
     */
    nlohmann::json get_all_info() const {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json result = nlohmann::json::array();
        for (const auto& [sid, info] : strategies_) {
            result.push_back(info.to_json());
        }
        return result;
    }

    /**
     * @brief 获取所有策略数量（含已停止的）
     */
    size_t total_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return strategies_.size();
    }

    /**
     * @brief 获取策略对应的 account_id
     */
    std::string get_account_id(const std::string& strategy_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = strategies_.find(strategy_id);
        if (it != strategies_.end()) {
            return it->second.account_id;
        }
        return "";
    }

    /**
     * @brief 停止并删除指定账户下的所有策略
     * @param account_id 账户ID
     * @return 被删除的策略ID列表
     */
    std::vector<std::string> stop_and_remove_by_account(const std::string& account_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> removed;

        for (auto it = strategies_.begin(); it != strategies_.end(); ) {
            if (it->second.account_id == account_id) {
                // 先停止运行中的进程
                if (it->second.pid > 0 && it->second.status == "running") {
                    kill(it->second.pid, SIGTERM);
                }
                removed.push_back(it->first);
                it = strategies_.erase(it);
            } else {
                ++it;
            }
        }

        return removed;
    }

    /**
     * @brief 停止所有运行中的策略进程（主程序退出时调用）
     * @return 被停止的策略数量
     */
    size_t stop_all_strategies() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t stopped = 0;

        for (auto& [sid, info] : strategies_) {
            if (info.pid > 0 && info.status == "running") {
                kill(info.pid, SIGTERM);
                info.status = "stopped";
                stopped++;
            }
        }

        // 等待 1 秒让进程优雅退出
        if (stopped > 0) {
            usleep(1000000);
            // 对仍然存活的进程发 SIGKILL
            for (auto& [sid, info] : strategies_) {
                if (info.pid > 0 && kill(info.pid, 0) == 0) {
                    kill(info.pid, SIGKILL);
                }
            }
        }

        return stopped;
    }

    /**
     * @brief 获取所有运行中策略的 PID（不加锁，仅在信号处理函数中使用）
     * @warning 仅在崩溃信号处理函数中调用，此时其他线程已不可靠
     */
    void kill_all_strategies_unsafe() {
        for (auto& [sid, info] : strategies_) {
            if (info.pid > 0 && info.status == "running") {
                ::kill(info.pid, SIGTERM);
            }
        }
        // 短暂等待后强制杀死
        usleep(500000);  // 0.5秒
        for (auto& [sid, info] : strategies_) {
            if (info.pid > 0 && ::kill(info.pid, 0) == 0) {
                ::kill(info.pid, SIGKILL);
            }
        }
    }

private:
    static int64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    std::map<std::string, StrategyProcessInfo> strategies_;
    mutable std::mutex mutex_;
};

} // namespace server
} // namespace trading
