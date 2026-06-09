/**
 * @file trading_server_main.cpp
 * @brief 完整实盘交易服务器 - 主入口
 *
 * 功能：
 * 1. WebSocket 行情 (trades, K线, 订单状态, 账户/持仓更新)
 * 2. REST API 交易 (下单, 批量下单, 撤单, 修改订单)
 * 3. REST API 查询 (账户余额, 持仓, 未成交订单)
 *
 * @author Sequence Team
 * @date 2025-12
 */

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <mutex>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

#include "config/server_config.h"
#include "../network/auth_manager.h"
#include "managers/account_manager.h"
#include "managers/account_monitor.h"  // 账户监控模块
#include "managers/redis_recorder.h"
#include "handlers/order_processor.h"
#include "handlers/query_handler.h"

#include "handlers/frontend_command_handler.h"
#include "handlers/subscription_manager.h"
#include "callbacks/websocket_callbacks.h"

#include "../network/zmq_server.h"
#include "../network/frontend_handler.h"
#include "../network/websocket_server.h"
#include "../trading/config_loader.h"
#include "../trading/account_registry.h"
#include "../trading/strategy_config_loader.h"
#include "../adapters/okx/okx_websocket.h"
#include "../adapters/okx/okx_rest_api.h"
#include "../adapters/binance/binance_websocket.h"
#include "../adapters/binance/binance_rest_api.h"
#include "../core/logger.h"
#include "../network/vpn_network_monitor.h"
#include "managers/symbol_delist_monitor.h"
#include <filesystem>

using namespace trading;
using namespace trading::server;
using namespace trading::okx;
using namespace trading::binance;
using namespace std::chrono;

struct BinanceKlineBatchState {
    size_t batch_no = 0;
    std::vector<std::string> streams;
    std::unique_ptr<binance::BinanceWebSocket> ws;
    std::atomic<int64_t> last_kline_ms{0};
    std::atomic<uint64_t> closed_kline_count{0};
};

std::vector<std::unique_ptr<BinanceKlineBatchState>> g_binance_kline_batches;
std::mutex g_binance_kline_batches_mutex;

// ============================================================
// CPU 亲和性
// ============================================================

bool pin_thread_to_cpu(int cpu_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (result == 0) {
        std::cout << "[绑核] 线程已绑定到 CPU " << cpu_id << std::endl;
        return true;
    }
    return false;
#else
    (void)cpu_id;
    return false;
#endif
}

bool set_realtime_priority(int priority = 50) {
#ifdef __linux__
    struct sched_param param;
    param.sched_priority = priority;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#else
    (void)priority;
    return false;
#endif
}

// ============================================================
// 信号处理
// ============================================================

static std::atomic<int> signal_count{0};

// 全局路径（崩溃信号处理函数需要）
static char g_exe_dir[4096] = {0};

void signal_handler(int signum) {
    int count = signal_count.fetch_add(1) + 1;

    if (count == 1) {
        std::cout << "\n[Server] 收到信号 " << signum << "，正在停止...\n";
        std::cout << "[Server] 再次按 Ctrl+C 可强制退出\n";
        g_running.store(false);
        set_curl_abort_flag(true);
        // 不在信号处理函数中调用 disconnect，让主循环处理
    } else {
        std::cout << "\n[Server] 收到第二次信号，强制退出！\n" << std::flush;
        std::_Exit(1);  // 立即退出，不执行清理
    }
}

/**
 * @brief 崩溃信号处理函数（SIGSEGV, SIGABRT, SIGBUS, SIGFPE）
 *
 * 在信号处理函数中只能使用 async-signal-safe 的函数。
 * 策略：
 * 1. 直接杀死所有策略子进程（不加锁，崩溃时其他线程不可靠）
 * 2. fork 子进程发送通知（邮件 + 飞书）
 * 3. _exit 退出
 */
void crash_signal_handler(int signum) {
    // 防止递归崩溃
    static volatile sig_atomic_t crash_entered = 0;
    if (crash_entered) {
        _exit(128 + signum);
    }
    crash_entered = 1;

    // 恢复默认信号处理（防止再次触发）
    std::signal(signum, SIG_DFL);

    const char* sig_name = "UNKNOWN";
    switch (signum) {
        case SIGSEGV: sig_name = "SIGSEGV (段错误)"; break;
        case SIGABRT: sig_name = "SIGABRT (异常终止)"; break;
        case SIGBUS:  sig_name = "SIGBUS (总线错误)"; break;
        case SIGFPE:  sig_name = "SIGFPE (浮点异常)"; break;
    }

    // 写入 stderr（write 是 async-signal-safe 的）
    const char* prefix = "\n[Server] 致命崩溃! 信号: ";
    (void)write(STDERR_FILENO, prefix, strlen(prefix));
    (void)write(STDERR_FILENO, sig_name, strlen(sig_name));
    (void)write(STDERR_FILENO, "\n", 1);

    // 写入崩溃日志文件 (crash_YYYYMMDD.log)
    {
        time_t now = time(nullptr);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);

        char log_path[4200];
        snprintf(log_path, sizeof(log_path), "%s/logs/crash_%04d%02d%02d.log",
                 g_exe_dir, tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);

        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

            char log_buf[2048];
            char hostname[256] = {0};
            gethostname(hostname, sizeof(hostname));

            int len = snprintf(log_buf, sizeof(log_buf),
                "[%s] 致命崩溃!\n"
                "  信号: %s (signum=%d)\n"
                "  PID: %d\n"
                "  主机: %s\n"
                "  exe_dir: %s\n"
                "---\n",
                time_str, sig_name, signum, (int)getpid(), hostname, g_exe_dir);

            if (len > 0) {
                (void)write(log_fd, log_buf, (size_t)len);
            }
            close(log_fd);
        }
    }

    // 1. 杀死所有策略子进程（不加锁）
    const char* kill_msg = "[Server] 正在杀死所有策略子进程...\n";
    (void)write(STDERR_FILENO, kill_msg, strlen(kill_msg));
    trading::server::g_strategy_manager.kill_all_strategies_unsafe();

    // 2. fork 子进程发送崩溃通知
    pid_t notify_pid = fork();
    if (notify_pid == 0) {
        // 子进程：发送通知后退出
        // 构建通知消息
        char message[2048];
        char hostname[256] = {0};
        gethostname(hostname, sizeof(hostname));

        time_t now = time(nullptr);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

        snprintf(message, sizeof(message),
            "交易服务器崩溃告警！\\n\\n"
            "信号: %s\\n"
            "主机: %s\\n"
            "时间: %s\\n"
            "PID: %d\\n\\n"
            "所有策略子进程已被终止。\\n"
            "请立即检查并重启服务器！",
            sig_name, hostname, time_str, (int)getppid());

        // 构建邮件通知命令
        char email_cmd[8192];
        snprintf(email_cmd, sizeof(email_cmd),
            "python3 %s/trading/alerts/email_alert.py"
            " -m \"%s\""
            " -l critical"
            " -s \"[崩溃告警] 交易服务器崩溃 - %s\""
            " -c \"%s/totalconfig/email_alert_network.json\""
            " --to \"alert@example.com\"",
            g_exe_dir, message, sig_name, g_exe_dir);

        // 构建飞书通知命令
        char lark_cmd[8192];
        snprintf(lark_cmd, sizeof(lark_cmd),
            "python3 %s/trading/alerts/lark_alert.py"
            " -m \"%s\""
            " -l critical"
            " --title \"[崩溃告警] 交易服务器崩溃 - %s\""
            " -c \"%s/trading/alerts/lark_config.json\""
            " --text",
            g_exe_dir, message, sig_name, g_exe_dir);

        // 执行通知（忽略返回值，尽力而为）
        (void)system(email_cmd);
        (void)system(lark_cmd);

        _exit(0);
    }

    // 父进程：等待子进程一小段时间（最多3秒）
    if (notify_pid > 0) {
        int wait_count = 0;
        while (wait_count < 30) {  // 最多等3秒
            int status;
            pid_t result = waitpid(notify_pid, &status, WNOHANG);
            if (result != 0) break;
            usleep(100000);  // 100ms
            wait_count++;
        }
    }

    const char* exit_msg = "[Server] 崩溃处理完成，退出\n";
    (void)write(STDERR_FILENO, exit_msg, strlen(exit_msg));

    // 重新触发信号，生成 core dump
    raise(signum);
}

// ============================================================
// 全局策略进程管理器
// ============================================================
namespace trading { namespace server {
StrategyProcessManager g_strategy_manager;
} }

// ============================================================
// 工作线程
// ============================================================

void order_thread(ZmqServer& server) {
    std::cout << "[订单线程] 启动\n";
    pin_thread_to_cpu(2);
    set_realtime_priority(49);

    while (g_running.load()) {
        int processed = 0;
        nlohmann::json order;
        while (server.recv_order_json(order)) {
            process_order_request(server, order);
            processed++;
        }
        if (processed == 0) {
            std::this_thread::sleep_for(microseconds(50));
        }
    }

    std::cout << "[订单线程] 停止\n";
}

void query_thread(ZmqServer& server) {
    std::cout << "[查询线程] 启动\n";
    pin_thread_to_cpu(3);

    server.set_query_callback([](const nlohmann::json& request) -> nlohmann::json {
        return handle_query(request);
    });

    while (g_running.load()) {
        int processed = server.poll_queries();
        if (processed == 0) {
            std::this_thread::sleep_for(milliseconds(1));
        }
    }

    std::cout << "[查询线程] 停止\n";
}

void subscription_thread(ZmqServer& server) {
    std::cout << "[订阅线程] 启动\n";

    server.set_subscribe_callback([](const nlohmann::json& request) {
        handle_subscription(request);
    });

    while (g_running.load()) {
        int processed = server.poll_subscriptions();
        if (processed == 0) {
            std::this_thread::sleep_for(milliseconds(5));
        }
    }

    std::cout << "[订阅线程] 停止\n";
}

// ============================================================
// 主函数
// ============================================================

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    using namespace trading::core;
    // 通过可执行文件路径推导项目根目录 (exe在cpp/build/下)
    std::string exe_dir = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path().string();
    // 保存到全局变量（崩溃信号处理函数需要）
    strncpy(g_exe_dir, exe_dir.c_str(), sizeof(g_exe_dir) - 1);
    Logger::instance().init(exe_dir + "/logs", "trading_server", LogLevel::INFO);

    std::cout << "========================================\n";
    std::cout << "    Sequence 实盘交易服务器 (Full)\n";
    std::cout << "    支持 OKX + Binance\n";
    std::cout << "========================================\n\n";

    LOG_INFO("实盘交易服务器启动");

    // 初始化用户认证管理器（加载用户配置）
    std::string user_config_dir = exe_dir + "/user_configs";
    g_auth_manager.init_user_configs(user_config_dir);
    LOG_INFO("用户配置目录: " + user_config_dir);

    load_config();

    // 打印风控配置
    print_risk_config();

    // ========================================
    // 初始化 Redis 录制器
    // ========================================
    std::cout << "\n[初始化] Redis 录制器...\n";
    g_redis_recorder = std::make_unique<RedisRecorder>();

    // 配置 Redis
    RedisConfig redis_config;
    // 从环境变量读取
    if (const char* v = std::getenv("REDIS_HOST")) redis_config.host = v;
    if (const char* v = std::getenv("REDIS_PORT")) redis_config.port = std::stoi(v);
    if (const char* v = std::getenv("REDIS_PASSWORD")) redis_config.password = v;
    if (const char* v = std::getenv("REDIS_DB")) redis_config.db = std::stoi(v);
    if (const char* v = std::getenv("REDIS_ENABLED")) {
        redis_config.enabled = (std::string(v) == "1" || std::string(v) == "true");
    }

    g_redis_recorder->set_config(redis_config);

    if (redis_config.enabled) {
        if (g_redis_recorder->start()) {
            std::cout << "[Redis] 录制器启动成功 ✓\n";
            std::cout << "[Redis] 服务器: " << redis_config.host << ":" << redis_config.port << "\n";
        } else {
            std::cerr << "[Redis] 录制器启动失败，继续运行但不录制数据\n";
        }
    } else {
        std::cout << "[Redis] 录制功能已禁用\n";
    }

    pin_thread_to_cpu(1);
    set_realtime_priority(50);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGHUP, signal_handler);   // tmux 会话关闭时发送 SIGHUP，需要走正常清理流程

    // 注册崩溃信号处理函数（非正常退出时杀死策略子进程 + 发送通知）
    std::signal(SIGSEGV, crash_signal_handler);
    std::signal(SIGABRT, crash_signal_handler);
    std::signal(SIGBUS, crash_signal_handler);
    std::signal(SIGFPE, crash_signal_handler);

    std::cout << "[配置] OKX 交易模式: " << (Config::is_testnet ? "模拟盘" : "实盘") << "\n";
    std::cout << "[配置] Binance 交易模式: " << (Config::binance_is_testnet ? "测试网" : "主网") << "\n";

    // ========================================
    // 加载策略配置（但不自动注册账户）
    // ========================================
    std::cout << "\n[初始化] 加载策略配置...\n";
    StrategyConfigManager::instance().load_configs("../strategies/configs");
    // 注意：不再自动注册账户，策略运行时会自己注册
    // load_and_register_strategies(g_account_registry, "../strategies/configs");

    std::cout << "[提示] 策略运行时会通过 register_account 消息注册自己的账户\n";

    // ========================================
    // 从磁盘加载账户配置
    // ========================================
    std::cout << "\n[初始化] 加载账户配置文件...\n";
    {
        std::string acct_dir = exe_dir + "/strategies/acount_configs";
        int loaded = 0;
        if (std::filesystem::exists(acct_dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(acct_dir)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                try {
                    std::ifstream f(entry.path().string());
                    nlohmann::json config;
                    f >> config;
                    f.close();

                    std::string account_id = config.value("account_id", "");
                    std::string exchange = config.value("exchange", "");
                    std::string api_key = config.value("api_key", "");
                    std::string secret_key = config.value("secret_key", "");
                    std::string passphrase = config.value("passphrase", "");
                    bool is_testnet = config.value("is_testnet", true);

                    if (account_id.empty() || api_key.empty()) continue;

                    ExchangeType ex_type = string_to_exchange_type(exchange);
                    bool ok = g_account_registry.register_account(
                        account_id, ex_type, api_key, secret_key, passphrase, is_testnet, account_id
                    );
                    if (ok) {
                        std::cout << "[账户配置] 加载: " << account_id << " (" << exchange << ")\n";
                        loaded++;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[账户配置] 加载失败: " << entry.path().filename().string() << " - " << e.what() << "\n";
                }
            }
        }
        std::cout << "[账户配置] 共加载 " << loaded << " 个账户\n";
    }

    // ========================================
    // 从 strategy_configs/ 加载策略并注册到进程管理器
    // ========================================
    std::cout << "\n[初始化] 加载策略配置文件 (strategy_configs/)...\n";
    {
        std::string strategy_cfg_dir = exe_dir + "/strategies/strategy_configs";
        // exe_dir 已经是 cpp/（两层 parent_path），直接拼接
        std::string strategy_source_dir = exe_dir + "/strategies/implementations";
        int loaded = 0;
        if (std::filesystem::exists(strategy_cfg_dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(strategy_cfg_dir)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                try {
                    std::ifstream f(entry.path().string());
                    nlohmann::json config;
                    f >> config;
                    f.close();

                    std::string strategy_id = config.value("strategy_id", "");
                    std::string account_id = config.value("account_id", "");
                    std::string exchange = config.value("exchange", "");
                    std::string python_file = config.value("python_file", "");

                    if (strategy_id.empty()) continue;

                    // 构建���动命令（如果配置中保存了 python_file）
                    std::string abs_config_path = std::filesystem::canonical(entry.path()).string();
                    std::string abs_py_dir;
                    try {
                        abs_py_dir = std::filesystem::canonical(strategy_source_dir).string();
                    } catch (...) {
                        abs_py_dir = strategy_source_dir;
                    }

                    std::string start_cmd;
                    if (!python_file.empty()) {
                        start_cmd = "cd " + abs_py_dir + " && python3 " + python_file + " --config " + abs_config_path;
                        // 如果配置中有 symbols，添加第一个 symbol 作为参数
                        if (config.contains("symbols") && config["symbols"].is_array() && !config["symbols"].empty()) {
                            std::string sym = config["symbols"][0].get<std::string>();
                            if (!sym.empty()) {
                                start_cmd += " --symbol " + sym;
                            }
                        }
                    }

                    // 注册策略（状态为 pending，等待手动启动）
                    g_strategy_manager.register_strategy(strategy_id, 0, account_id, exchange, start_cmd, abs_py_dir, "pending");

                    std::cout << "[策略配置] 加载: " << strategy_id << " (账户: " << account_id << ", " << exchange
                              << ", python: " << (python_file.empty() ? "未指定" : python_file) << ")\n";
                    loaded++;
                } catch (const std::exception& e) {
                    std::cerr << "[策略配置] 加载失败: " << entry.path().filename().string() << " - " << e.what() << "\n";
                }
            }
        }
        std::cout << "[策略配置] 共加载 " << loaded << " 个策略\n";
    }

    // ========================================
    // 启动前端处理器
    // ========================================
    std::cout << "\n[初始化] 启动前端处理器...\n";
    FrontendHandler frontend_handler(g_account_registry);
    if (!frontend_handler.start("tcp://*:5556")) {
        std::cerr << "[错误] 前端处理器启动失败\n";
        return 1;
    }
    std::cout << "[前端] 监听端口 5556 ✓\n";

    // ========================================
    // 初始化 ZeroMQ
    // ========================================
    ZmqServer zmq_server(0);  // mode=0: 使用 trading_*.ipc 地址，实盘和模拟策略都能连接
    if (!zmq_server.start()) {
        std::cerr << "[错误] ZeroMQ 服务启动失败\n";
        return 1;
    }

    std::cout << "[初始化] ZeroMQ 通道:\n";
    std::cout << "  - 行情(统一): " << IpcAddresses::MARKET_DATA << "\n";
    std::cout << "  - 行情(OKX):  " << IpcAddresses::MARKET_DATA_OKX << "\n";
    std::cout << "  - 行情(Binance): " << IpcAddresses::MARKET_DATA_BINANCE << "\n";
    std::cout << "  - 订单: " << IpcAddresses::ORDER << "\n";
    std::cout << "  - 回报: " << IpcAddresses::REPORT << "\n";
    std::cout << "  - 查询: " << IpcAddresses::QUERY << "\n";
    std::cout << "  - 订阅: " << IpcAddresses::SUBSCRIBE << "\n";

    // ========================================
    // 初始化 OKX WebSocket (只订阅公共行情)
    // ========================================
    std::cout << "\n[初始化] OKX WebSocket...\n";

    g_ws_business = create_business_ws(Config::is_testnet);
    g_ws_business->set_auto_reconnect(true);

    // 设置 OKX K线回调
    setup_websocket_callbacks(zmq_server);

    if (!g_ws_business->connect()) {
        std::cerr << "[错误] WebSocket Business 连接失败\n";
        return 1;
    }
    std::cout << "[WebSocket] OKX Business ✓\n";

    // 动态获取 OKX 所有永续合约交易对
    std::vector<std::string> okx_swap_symbols = Config::swap_symbols;

    if (okx_swap_symbols.empty() || okx_swap_symbols.size() <= 5) {
        std::cout << "[OKX] 动态获取所有永续合约交易对...\n";

        try {
            OKXRestAPI okx_api("", "", "", Config::is_testnet);
            auto instruments = okx_api.get_instruments("SWAP");

            if (instruments.contains("data") && instruments["data"].is_array()) {
                okx_swap_symbols.clear();
                for (const auto& inst : instruments["data"]) {
                    std::string inst_id = inst.value("instId", "");
                    std::string state = inst.value("state", "");
                    std::string settle_ccy = inst.value("settleCcy", "");

                    if (!inst_id.empty() && state == "live" && settle_ccy == "USDT") {
                        okx_swap_symbols.push_back(inst_id);
                    }
                }
                std::cout << "[OKX] 获取到 " << okx_swap_symbols.size() << " 个 USDT 永续合约\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[OKX] 获取交易对失败: " << e.what() << "\n";
            okx_swap_symbols = {"BTC-USDT-SWAP", "ETH-USDT-SWAP", "SOL-USDT-SWAP", "XRP-USDT-SWAP", "DOGE-USDT-SWAP"};
            std::cout << "[OKX] 使用默认 " << okx_swap_symbols.size() << " 个币种\n";
        }
    }

    // 批量订阅 OKX K线（1m 和 1s）
    const size_t okx_batch_size = 100;

    // 订阅 1m K线
    for (size_t i = 0; i < okx_swap_symbols.size(); i += okx_batch_size) {
        size_t end = std::min(i + okx_batch_size, okx_swap_symbols.size());
        std::vector<std::string> batch(okx_swap_symbols.begin() + i, okx_swap_symbols.begin() + end);
        g_ws_business->subscribe_klines_batch(batch, "1m");
        std::cout << "[订阅] OKX K线(1m)批次 " << (i / okx_batch_size + 1) << ": " << batch.size() << " 个币种\n";
    }
    for (const auto& symbol : okx_swap_symbols) {
        g_subscribed_klines[symbol].insert("1m");
    }
    std::cout << "[订阅] OKX K线(1m): " << okx_swap_symbols.size() << " 个 ✓\n";

    // OKX 1s K线订阅已禁用：283个交易对的1s数据量过大，
    // 会导致 g_ws_business 单连接积压，拖慢1m K线的实时接收
    // 如需恢复，建议创建独立的 WebSocket 连接（参考 Binance kline_1s_ws 的做法）
    std::cout << "[订阅] OKX K线(1s): 已跳过（减少 ws_business 负载）\n";

    // 订阅 OKX Ticker（用于前端实时行情显示）
    // 需要先初始化 g_ws_public（公共频道）
    std::cout << "\n[初始化] OKX Public WebSocket (Ticker)...\n";
    g_ws_public = create_public_ws(Config::is_testnet);
    g_ws_public->set_auto_reconnect(true);

    // 设置 OKX Ticker 回调（必须在 g_ws_public 初始化后设置）
    g_ws_public->set_ticker_callback([&zmq_server](const nlohmann::json& raw) {
        g_okx_ticker_count++;

        std::string symbol = "";
        if (raw.contains("instId")) {
            symbol = raw["instId"].get<std::string>();
        }
        // 去掉 -SWAP 后缀
        const std::string suffix = "-SWAP";
        if (symbol.size() > suffix.size() &&
            symbol.compare(symbol.size() - suffix.size(), suffix.size(), suffix) == 0) {
            symbol = symbol.substr(0, symbol.size() - suffix.size());
        }

        nlohmann::json msg = {
            {"type", "ticker"},
            {"exchange", "okx"},
            {"symbol", symbol},
            {"timestamp_ns", current_timestamp_ns()}
        };

        // 提取价格字段
        if (raw.contains("last")) {
            if (raw["last"].is_string()) {
                msg["price"] = std::stod(raw["last"].get<std::string>());
            } else if (raw["last"].is_number()) {
                msg["price"] = raw["last"].get<double>();
            }
        }
        if (raw.contains("ts")) {
            if (raw["ts"].is_string()) {
                msg["timestamp"] = std::stoll(raw["ts"].get<std::string>());
            } else if (raw["ts"].is_number()) {
                msg["timestamp"] = raw["ts"].get<int64_t>();
            }
        }

        // 发布到 ZMQ
        zmq_server.publish_okx_market(msg, MessageType::TICKER);
        zmq_server.publish_ticker(msg);

        // 发送到前端
        if (g_frontend_server) {
            g_frontend_server->send_event("ticker", msg);
        }
    });

    if (!g_ws_public->connect()) {
        std::cerr << "[警告] OKX Public WebSocket 连接失败，跳过 Ticker 订阅\n";
    } else {
        std::cout << "[WebSocket] OKX Public ✓\n";
        // 订阅前端需要显示的主要币种 ticker
        std::vector<std::string> ticker_symbols = {
            "BTC-USDT-SWAP", "ETH-USDT-SWAP", "SOL-USDT-SWAP",
            "XRP-USDT-SWAP", "DOGE-USDT-SWAP"
        };
        for (const auto& symbol : ticker_symbols) {
            g_ws_public->subscribe_ticker(symbol);
        }
        std::cout << "[订阅] OKX Ticker: " << ticker_symbols.size() << " 个主要币种 ✓\n";
    }

    // ========================================
    // 初始化 Binance WebSocket
    // ========================================
    std::cout << "\n[初始化] Binance WebSocket...\n";

    // 动态获取所有交易对
    std::vector<std::string> symbols_to_subscribe = Config::binance_symbols;

    if (symbols_to_subscribe.empty()) {
        std::cout << "[Binance] 配置为空，动态获取所有永续合约交易对...\n";

        try {
            BinanceRestAPI binance_api("", "", MarketType::FUTURES, Config::binance_is_testnet);
            auto exchange_info = binance_api.get_exchange_info();

            if (exchange_info.contains("symbols") && exchange_info["symbols"].is_array()) {
                for (const auto& sym : exchange_info["symbols"]) {
                    // 只订阅永续合约且状态为 TRADING 的交易对
                    std::string contract_type = sym.value("contractType", "");
                    std::string status = sym.value("status", "");
                    std::string symbol = sym.value("symbol", "");

                    if (contract_type == "PERPETUAL" && status == "TRADING" && !symbol.empty()) {
                        symbols_to_subscribe.push_back(symbol);
                    }
                }
                std::cout << "[Binance] 获取到 " << symbols_to_subscribe.size() << " 个永续合约交易对\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Binance] ❌ 获取交易对失败: " << e.what() << "\n";
            std::cerr << "[Binance] 使用默认主流币种列表...\n";
            // 使用默认主流币种作为后备
            symbols_to_subscribe = {
                "BTCUSDT", "ETHUSDT", "BNBUSDT", "SOLUSDT", "XRPUSDT",
                "DOGEUSDT", "ADAUSDT", "AVAXUSDT", "LINKUSDT", "DOTUSDT",
                "MATICUSDT", "LTCUSDT", "TRXUSDT", "ATOMUSDT", "UNIUSDT"
            };
        }
    }

    // 准备小写的币种列表
    size_t subscribe_count = symbols_to_subscribe.size();
    std::vector<std::string> lower_symbols;
    lower_symbols.reserve(subscribe_count);
    for (size_t i = 0; i < subscribe_count; ++i) {
        std::string lower_symbol = symbols_to_subscribe[i];
        std::transform(lower_symbol.begin(), lower_symbol.end(), lower_symbol.begin(), ::tolower);
        lower_symbols.push_back(lower_symbol);
    }

    // ========================================
    // 订阅 K线（1m；1s 已禁用）
    // ========================================
    std::cout << "\n[初始化] Binance K线 WebSocket (组合流URL方式，150 streams/批)...\n";

    std::vector<std::string> kline_1m_streams;
    kline_1m_streams.reserve(lower_symbols.size());
    for (const auto& sym : lower_symbols) {
        kline_1m_streams.push_back(sym + "_perpetual@continuousKline_1m");
    }

    auto connect_binance_kline_batch = [&zmq_server](BinanceKlineBatchState& state) -> bool {
        auto ws = create_market_ws(MarketType::FUTURES, Config::binance_is_testnet);
        ws->set_auto_reconnect(true);
        setup_binance_kline_callback(
            ws.get(),
            zmq_server,
            [&state](const std::string&, int64_t) {
                state.last_kline_ms.store(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
                state.closed_kline_count.fetch_add(1);
            }
        );

        int64_t now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        if (ws->connect_with_streams(state.streams)) {
            state.ws = std::move(ws);
            state.last_kline_ms.store(now_ms);
            state.closed_kline_count.store(0);
            return true;
        }

        state.last_kline_ms.store(now_ms);
        state.closed_kline_count.store(0);
        return false;
    };

    const size_t binance_kline_batch_size = 150;
    size_t kline_1m_connections = 0;
    for (size_t i = 0; i < kline_1m_streams.size(); i += binance_kline_batch_size) {
        size_t end = std::min(i + binance_kline_batch_size, kline_1m_streams.size());
        auto state = std::make_unique<BinanceKlineBatchState>();
        state->batch_no = i / binance_kline_batch_size + 1;
        state->streams.assign(kline_1m_streams.begin() + i, kline_1m_streams.begin() + end);

        if (connect_binance_kline_batch(*state)) {
            std::cout << "[WebSocket] Binance K线(1m)连接" << state->batch_no << " ✓ ("
                      << state->streams.size() << " streams)\n";
            kline_1m_connections++;
        } else {
            std::cerr << "[警告] Binance K线(1m)连接" << state->batch_no << " 失败 ("
                      << state->streams.size() << " streams)\n";
        }

        g_binance_kline_batches.push_back(std::move(state));
        // 错峰建连: 间隔 5 秒, 避免一次性多条 ws 触发 Binance 服务端 throttle
        // (之前 300ms 太短, 多批 ws 同时升级会被服务端冷处理, 导致重连后依然不推数据)
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    std::cout << "[订阅] Binance kline(1m): " << subscribe_count << " 个币种 (通过 "
              << kline_1m_connections << " 个连接, 每批最多 "
              << binance_kline_batch_size << " streams) ✓\n";

    std::cout << "[订阅] Binance kline(1s): 已跳过（当前策略不使用，减少 WebSocket 负载）\n";

    std::atomic<bool> binance_kline_health_running{true};
    std::thread binance_kline_health_thread([&]() {
        const int64_t stale_ms = 120000;
        const int64_t check_interval_ms = 30000;
        int64_t last_check_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

        while (binance_kline_health_running.load() && g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int64_t now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            if (now_ms - last_check_ms < check_interval_ms) {
                continue;
            }
            last_check_ms = now_ms;

            std::lock_guard<std::mutex> lock(g_binance_kline_batches_mutex);
            for (auto& state_ptr : g_binance_kline_batches) {
                auto& state = *state_ptr;
                int64_t last_ms = state.last_kline_ms.load();
                if (last_ms <= 0 || now_ms - last_ms <= stale_ms) {
                    continue;
                }

                std::cerr << "[健康检查] Binance K线(1m)批次" << state.batch_no
                          << " 已 " << (now_ms - last_ms) / 1000
                          << " 秒无闭合K线，重建连接...\n";

                if (state.ws) {
                    state.ws->disconnect();
                    state.ws.reset();
                }

                if (connect_binance_kline_batch(state)) {
                    std::cout << "[健康检查] Binance K线(1m)批次" << state.batch_no
                              << " 重建成功 (" << state.streams.size() << " streams)\n";
                } else {
                    std::cerr << "[健康检查] Binance K线(1m)批次" << state.batch_no
                              << " 重建失败 (" << state.streams.size() << " streams)\n";
                }
            }
        }
    });

    // ========================================
    // 订阅 Binance Ticker（用于前端实时行情显示）
    // ========================================
    std::cout << "\n[初始化] Binance Ticker WebSocket...\n";
    g_binance_ws_market = create_market_ws(MarketType::FUTURES, Config::binance_is_testnet);
    g_binance_ws_market->set_auto_reconnect(true);

    // 设置 Binance ticker 回调
    setup_binance_websocket_callbacks(zmq_server);

    // 仅订阅前端展示需要的主要币种 ticker（!ticker@arr 在部分网络环境下不下发数据）
    std::vector<std::string> binance_ticker_streams = {
        "btcusdt@ticker", "ethusdt@ticker", "solusdt@ticker",
        "xrpusdt@ticker", "dogeusdt@ticker"
    };
    if (g_binance_ws_market->connect_with_streams(binance_ticker_streams)) {
        std::cout << "[订阅] Binance Ticker: " << binance_ticker_streams.size() << " 个主要币种 ✓\n";
    } else {
        std::cerr << "[警告] Binance Ticker 连接失败\n";
    }

    // ========================================
    // 启动前端 WebSocket 服务器
    // ========================================
    g_frontend_server = std::make_unique<core::WebSocketServer>();
    g_frontend_server->set_message_callback(handle_frontend_command);

    if (!g_frontend_server->start("0.0.0.0", 8002)) {
        std::cerr << "[错误] 前端WebSocket服务器启动失败\n";
        binance_kline_health_running.store(false);
        if (binance_kline_health_thread.joinable()) {
            binance_kline_health_thread.join();
        }
        return 1;
    }

    // 设置 Logger 的 WebSocket 回调，将日志推送到前端
    Logger::instance().set_ws_callback([](const std::string& level, const std::string& source, const std::string& msg) {
        if (g_frontend_server && g_frontend_server->is_running()) {
            g_frontend_server->send_log(level, source, msg);
        }
    });

    std::cout << "[前端] WebSocket服务器已启动（端口8002）\n";
    std::cout << "[日志] 日志推送到前端已启用\n";

    // ========================================
    // 启动账户监控
    // ========================================
    std::cout << "\n[初始化] 账户监控模块...\n";
    auto account_monitor = std::make_unique<AccountMonitor>(g_risk_manager);

    // 设置全局账户监控器指针（用于动态添加账户）
    g_account_monitor = account_monitor.get();

    // 注册所有已注册的 OKX 账户（传入 account_id 避免日志源重复）
    auto okx_accounts = g_account_registry.get_all_okx_accounts();
    for (const auto& [id, api] : okx_accounts) {
        account_monitor->register_okx_account(id, api, nullptr, id);
    }

    // 注册所有已注册的 Binance 账户
    auto binance_accounts = g_account_registry.get_all_binance_accounts();
    for (const auto& [id, api] : binance_accounts) {
        account_monitor->register_binance_account(id, api, nullptr, id);
    }

    // 禁用WebSocket模式，使用REST API轮询
    account_monitor->set_use_websocket(false);

    // 启动监控
    account_monitor->start(10);
    size_t acct_count = okx_accounts.size() + binance_accounts.size();
    if (acct_count > 0) {
        std::cout << "[账户监控] ✓ 已启动，监控 " << okx_accounts.size() << " 个OKX账户 + "
                  << binance_accounts.size() << " 个Binance账户\n";
    } else {
        std::cout << "[账户监控] ✓ 已启动，等待账户动态注册...\n";
    }
    std::cout << "[账户监控] 监控间隔: 10秒\n";

    // ========================================
    // 启动网络监控（监控 WebSocket 重连状态）
    // ========================================
    std::cout << "\n[初始化] 网络监控模块...\n";
    std::atomic<bool> network_monitor_running{true};
    std::thread network_monitor_thread([&network_monitor_running]() {
        std::cout << "[网络监控] 监控线程已启动\n";
        while (network_monitor_running.load() && g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));  // 每 10 秒检查一次

            if (g_ws_business) {
                auto [fail_count, first_fail_time, alert_sent] = g_ws_business->get_reconnect_fail_status();

                if (fail_count > 0 && first_fail_time > 0 && !alert_sent) {
                    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    int64_t elapsed_ms = now_ms - first_fail_time;

                    // 连续 1 分钟重连失败，发送告警
                    if (elapsed_ms >= 60000) {  // 60秒 = 60000毫秒
                        std::cerr << "[网络监控] ⚠️  OKX WebSocket 连续 " << (elapsed_ms / 1000)
                                  << " 秒重连失败（" << fail_count << " 次），发送告警邮件\n";

                        // 调用风控系统发送告警
                        g_risk_manager.send_alert(
                            "OKX WebSocket 连续 " + std::to_string(elapsed_ms / 1000) + " 秒重连失败（"
                            + std::to_string(fail_count) + " 次）。请检查网络连接和代理设置。",
                            AlertLevel::CRITICAL,
                            "网络连接异常"
                        );

                        // 标记告警已发送
                        g_ws_business->mark_network_alert_sent();
                        std::cout << "[网络监控] ✓ 告警已发送\n";
                    }
                }
            }
        }
        std::cout << "[网络监控] 监控线程已退出\n";
    });
    std::cout << "[网络监控] ✓ 已启动，检查间隔: 10秒\n";

    // ========================================
    // 启动 VPN/代理 网络监控
    // ========================================
    std::cout << "\n[初始化] VPN/代理网络监控...\n";
    VpnNetworkMonitor vpn_monitor(g_risk_manager);
    {
        std::string vpn_config_path = exe_dir + "/totalconfig/network_monitor_config.json";
        if (vpn_monitor.load_config(vpn_config_path)) {
            vpn_monitor.start();
        } else {
            std::cerr << "[VPN监控] 配置加载失败，跳过启动\n";
        }
    }

    // ========================================
    // 启动合约下架监控
    // ========================================
    std::cout << "\n[初始化] 合约下架监控...\n";
    SymbolDelistMonitor::Config delist_config;
    delist_config.base_url = Config::binance_is_testnet
        ? "https://testnet.binancefuture.com" : "https://fapi.binance.com";
    delist_config.poll_interval_sec = 30;
    delist_config.email_config_file = exe_dir + "/totalconfig/email_alert_network.json";
    delist_config.lark_config_file = exe_dir + "/trading/alerts/lark_config.json";
    delist_config.alerts_script_dir = exe_dir + "/trading/alerts";
    delist_config.user_config_dir = exe_dir + "/user_configs";
    delist_config.state_file = exe_dir + "/totalconfig/delist_notified_state.json";
    delist_config.to_emails = {"alert@example.com"};  // 默认收件人（同VPN告警配置）

    SymbolDelistMonitor delist_monitor(delist_config);
    delist_monitor.start();
    std::cout << "[下架监控] ✓ 已启动，轮询间隔: 30秒\n";

    // ========================================
    // 启动工作线程
    // ========================================
    std::thread order_worker(order_thread, std::ref(zmq_server));
    std::thread query_worker(query_thread, std::ref(zmq_server));
    std::thread sub_worker(subscription_thread, std::ref(zmq_server));

    // ========================================
    // 主循环
    // ========================================
    std::cout << "\n========================================\n";
    std::cout << "  服务器启动完成！\n";
    std::cout << "  等待策略连接...\n";
    std::cout << "  按 Ctrl+C 停止\n";
    std::cout << "========================================\n\n";

    int status_counter = 0;
    int heartbeat_check_counter = 0;
    while (g_running.load()) {
        std::this_thread::sleep_for(milliseconds(100));
        status_counter++;
        heartbeat_check_counter++;

        // 每10秒检查一次策略心跳
        if (heartbeat_check_counter >= 100) {
            heartbeat_check_counter = 0;
            g_strategy_manager.check_heartbeats(15);
        }

        if (status_counter >= 100 && g_running.load()) {
            status_counter = 0;
            std::stringstream ss;
            ss << "K线[OKX:" << g_okx_kline_count
               << " Binance:" << g_binance_kline_count << "]"
               << " | 订单:" << g_order_count
               << "(成功:" << g_order_success
               << " 失败:" << g_order_failed << ")"
               << " | 查询:" << g_query_count
               << " | 账户:" << get_registered_strategy_count()
               << " | 策略(运行):" << g_strategy_manager.running_count();
            Logger::instance().info("market", ss.str());
        }
    }

    // ========================================
    // 清理
    // ========================================
    std::cout << "\n[Server] 正在停止...\n";
    LOG_INFO("服务器正在停止...");

    // 停止所有策略子进程
    std::cout << "[Server] 停止所有策略进程...\n";
    {
        size_t stopped = g_strategy_manager.stop_all_strategies();
        std::cout << "[Server] 已停止 " << stopped << " 个策略进程\n";
        LOG_INFO("已停止 " + std::to_string(stopped) + " 个策略进程");
    }

    // 停止 Binance K线批次健康检查
    std::cout << "[Server] 停止 Binance K线健康检查...\n";
    binance_kline_health_running.store(false);
    if (binance_kline_health_thread.joinable()) {
        binance_kline_health_thread.join();
    }

    // 停止网络监控
    std::cout << "[Server] 停止网络监控...\n";
    network_monitor_running.store(false);
    if (network_monitor_thread.joinable()) {
        network_monitor_thread.join();
    }

    // 停止 VPN/代理 网络监控
    std::cout << "[Server] 停止VPN网络监控...\n";
    vpn_monitor.stop();

    // 停止合约下架监控
    std::cout << "[Server] 停止合约下架监控...\n";
    delist_monitor.stop();

    // 停止账户监控
    if (account_monitor) {
        std::cout << "[Server] 停止账户监控...\n";
        account_monitor->stop();
        g_account_monitor = nullptr;  // 清空全局指针
        std::cout << "[Server] 账户监控已停止\n";
    }

    std::cout << "[Server] 断开 WebSocket 连接...\n";
    if (g_ws_business && g_ws_business->is_connected()) {
        g_ws_business->disconnect();
    }
    {
        std::lock_guard<std::mutex> lock(g_binance_kline_batches_mutex);
        for (auto& state : g_binance_kline_batches) {
            if (state->ws && state->ws->is_connected()) {
                state->ws->disconnect();
            }
        }
    }

    std::cout << "[Server] 等待工作线程退出...\n";
    if (order_worker.joinable()) order_worker.join();
    std::cout << "[Server] 订单线程已退出\n";
    if (query_worker.joinable()) query_worker.join();
    std::cout << "[Server] 查询线程已退出\n";
    if (sub_worker.joinable()) sub_worker.join();
    std::cout << "[Server] 订阅线程已退出\n";

    if (g_frontend_server) {
        std::cout << "[Server] 停止前端WebSocket服务器...\n";
        g_frontend_server->stop();
    }

    std::cout << "[Server] 停止 ZeroMQ...\n";
    zmq_server.stop();

    std::cout << "[Server] 停止前端处理器...\n";
    frontend_handler.stop();

    std::cout << "[Server] 清理账户注册器...\n";
    g_account_registry.clear();

    // 停止 Redis 录制器
    if (g_redis_recorder) {
        std::cout << "[Server] 停止 Redis 录制器...\n";
        g_redis_recorder->stop();
        g_redis_recorder.reset();
    }

    // 显式释放全局 WebSocket 对象，避免程序退出时 double free
    std::cout << "[Server] 释放 WebSocket 对象...\n";
    {
        std::lock_guard<std::mutex> lock(g_binance_kline_batches_mutex);
        g_binance_kline_batches.clear();
    }
    g_ws_business.reset();
    g_frontend_server.reset();

    // 等待一小段时间确保所有 IO 线程完全退出
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "\n========================================\n";
    std::cout << "  服务器已停止\n";
    std::cout << "  K线(OKX): " << g_okx_kline_count << " 条\n";
    std::cout << "  K线(Binance): " << g_binance_kline_count << " 条\n";
    std::cout << "  订单: " << g_order_count << " 笔\n";
    std::cout << "========================================\n";

    LOG_INFO("服务器已停止 | K线(OKX):" + std::to_string(g_okx_kline_count.load()) +
             " K线(Binance):" + std::to_string(g_binance_kline_count.load()) +
             " 订单:" + std::to_string(g_order_count.load()));
    Logger::instance().shutdown();

    std::cout << "[Server] 清理完成，安全退出\n" << std::flush;

    return 0;
}
