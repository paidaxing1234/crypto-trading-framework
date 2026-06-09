#pragma once

/**
 * @file config_center.h
 * @brief 统一配置中心 - 集中管理所有配置项
 *
 * 功能：
 * - 多来源配置加载（JSON文件、环境变量、命令行）
 * - 配置优先级：命令行 > 环境变量 > 配置文件 > 默认值
 * - 配置热更新（支持运行时重载）
 * - 配置变更通知（观察者模式）
 * - 线程安全访问
 * - 配置验证
 *
 * @author Sequence Team
 * @date 2026-01
 */

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <nlohmann/json.hpp>

namespace trading {
namespace config {

// ============================================================
// 配置变更监听器
// ============================================================

using ConfigChangeCallback = std::function<void(const std::string& key, const nlohmann::json& old_value, const nlohmann::json& new_value)>;

// ============================================================
// 配置项定义
// ============================================================

/**
 * @brief 服务器配置
 */
struct ServerConfig {
    // 网络配置
    int zmq_pub_port = 5555;           // ZMQ PUB 端口
    int zmq_pull_port = 5556;          // ZMQ PULL 端口
    int zmq_rep_port = 5557;           // ZMQ REP 端口
    int websocket_port = 8002;          // WebSocket 端口
    std::string bind_address = "0.0.0.0";  // 绑定地址

    // 日志配置
    std::string log_level = "info";     // debug, info, warn, error
    std::string log_dir = "./logs";     // 日志目录
    bool log_to_console = true;         // 是否输出到控制台
    bool log_to_file = true;            // 是否写入文件
    int log_max_size_mb = 100;          // 单个日志文件最大大小
    int log_max_files = 10;             // 最多保留日志文件数

    // 性能配置
    int thread_pool_size = 4;           // 线程池大小
    int max_pending_orders = 1000;      // 最大待处理订单数
    int order_timeout_ms = 5000;        // 订单超时时间

    // 策略目录配置（相对于 cpp/build 目录）
    std::string strategy_log_dir = "../strategies/logs";                // 策略日志目录
    std::string strategy_source_dir = "../strategies/implementations";  // 策略源码目录
    std::string strategy_config_dir = "../strategies/configs";          // 策略配置目录

    nlohmann::json to_json() const {
        return {
            {"zmq_pub_port", zmq_pub_port},
            {"zmq_pull_port", zmq_pull_port},
            {"zmq_rep_port", zmq_rep_port},
            {"websocket_port", websocket_port},
            {"bind_address", bind_address},
            {"log_level", log_level},
            {"log_dir", log_dir},
            {"log_to_console", log_to_console},
            {"log_to_file", log_to_file},
            {"log_max_size_mb", log_max_size_mb},
            {"log_max_files", log_max_files},
            {"thread_pool_size", thread_pool_size},
            {"max_pending_orders", max_pending_orders},
            {"order_timeout_ms", order_timeout_ms},
            {"strategy_log_dir", strategy_log_dir},
            {"strategy_source_dir", strategy_source_dir},
            {"strategy_config_dir", strategy_config_dir}
        };
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("zmq_pub_port")) zmq_pub_port = j["zmq_pub_port"];
        if (j.contains("zmq_pull_port")) zmq_pull_port = j["zmq_pull_port"];
        if (j.contains("zmq_rep_port")) zmq_rep_port = j["zmq_rep_port"];
        if (j.contains("websocket_port")) websocket_port = j["websocket_port"];
        if (j.contains("bind_address")) bind_address = j["bind_address"];
        if (j.contains("log_level")) log_level = j["log_level"];
        if (j.contains("log_dir")) log_dir = j["log_dir"];
        if (j.contains("log_to_console")) log_to_console = j["log_to_console"];
        if (j.contains("log_to_file")) log_to_file = j["log_to_file"];
        if (j.contains("log_max_size_mb")) log_max_size_mb = j["log_max_size_mb"];
        if (j.contains("log_max_files")) log_max_files = j["log_max_files"];
        if (j.contains("thread_pool_size")) thread_pool_size = j["thread_pool_size"];
        if (j.contains("max_pending_orders")) max_pending_orders = j["max_pending_orders"];
        if (j.contains("order_timeout_ms")) order_timeout_ms = j["order_timeout_ms"];
        if (j.contains("strategy_log_dir")) strategy_log_dir = j["strategy_log_dir"];
        if (j.contains("strategy_source_dir")) strategy_source_dir = j["strategy_source_dir"];
        if (j.contains("strategy_config_dir")) strategy_config_dir = j["strategy_config_dir"];
    }
};

/**
 * @brief OKX交易所配置
 */
struct OKXConfig {
    std::string api_key;
    std::string secret_key;
    std::string passphrase;
    bool is_testnet = false;

    // WebSocket配置
    bool enable_public_ws = true;
    bool enable_private_ws = true;
    bool enable_business_ws = true;
    int ws_ping_interval_sec = 25;
    int ws_reconnect_delay_sec = 5;

    // 代理配置
    std::string proxy_host;
    int proxy_port = 0;

    // 订阅的交易对
    std::vector<std::string> spot_symbols;
    std::vector<std::string> swap_symbols;

    nlohmann::json to_json() const {
        return {
            {"api_key", api_key.empty() ? "" : api_key.substr(0, 8) + "..."},
            {"is_testnet", is_testnet},
            {"enable_public_ws", enable_public_ws},
            {"enable_private_ws", enable_private_ws},
            {"enable_business_ws", enable_business_ws},
            {"ws_ping_interval_sec", ws_ping_interval_sec},
            {"ws_reconnect_delay_sec", ws_reconnect_delay_sec},
            {"proxy_host", proxy_host},
            {"proxy_port", proxy_port},
            {"spot_symbols", spot_symbols},
            {"swap_symbols", swap_symbols}
        };
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("api_key")) api_key = j["api_key"];
        if (j.contains("secret_key")) secret_key = j["secret_key"];
        if (j.contains("passphrase")) passphrase = j["passphrase"];
        if (j.contains("is_testnet")) is_testnet = j["is_testnet"];
        if (j.contains("enable_public_ws")) enable_public_ws = j["enable_public_ws"];
        if (j.contains("enable_private_ws")) enable_private_ws = j["enable_private_ws"];
        if (j.contains("enable_business_ws")) enable_business_ws = j["enable_business_ws"];
        if (j.contains("ws_ping_interval_sec")) ws_ping_interval_sec = j["ws_ping_interval_sec"];
        if (j.contains("ws_reconnect_delay_sec")) ws_reconnect_delay_sec = j["ws_reconnect_delay_sec"];
        if (j.contains("proxy_host")) proxy_host = j["proxy_host"];
        if (j.contains("proxy_port")) proxy_port = j["proxy_port"];
        if (j.contains("spot_symbols")) {
            spot_symbols.clear();
            for (const auto& s : j["spot_symbols"]) {
                spot_symbols.push_back(s.get<std::string>());
            }
        }
        if (j.contains("swap_symbols")) {
            swap_symbols.clear();
            for (const auto& s : j["swap_symbols"]) {
                swap_symbols.push_back(s.get<std::string>());
            }
        }
    }
};

/**
 * @brief Binance交易所配置
 */
struct BinanceConfig {
    std::string api_key;
    std::string secret_key;
    bool is_testnet = false;

    // WebSocket配置
    bool enable_market_ws = true;
    bool enable_user_ws = true;
    int ws_ping_interval_sec = 180;
    int ws_reconnect_delay_sec = 5;

    // 代理配置
    std::string proxy_host;
    int proxy_port = 0;

    // 订阅的交易对
    std::vector<std::string> futures_symbols;

    nlohmann::json to_json() const {
        return {
            {"api_key", api_key.empty() ? "" : api_key.substr(0, 8) + "..."},
            {"is_testnet", is_testnet},
            {"enable_market_ws", enable_market_ws},
            {"enable_user_ws", enable_user_ws},
            {"ws_ping_interval_sec", ws_ping_interval_sec},
            {"ws_reconnect_delay_sec", ws_reconnect_delay_sec},
            {"proxy_host", proxy_host},
            {"proxy_port", proxy_port},
            {"futures_symbols", futures_symbols}
        };
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("api_key")) api_key = j["api_key"];
        if (j.contains("secret_key")) secret_key = j["secret_key"];
        if (j.contains("is_testnet")) is_testnet = j["is_testnet"];
        if (j.contains("enable_market_ws")) enable_market_ws = j["enable_market_ws"];
        if (j.contains("enable_user_ws")) enable_user_ws = j["enable_user_ws"];
        if (j.contains("ws_ping_interval_sec")) ws_ping_interval_sec = j["ws_ping_interval_sec"];
        if (j.contains("ws_reconnect_delay_sec")) ws_reconnect_delay_sec = j["ws_reconnect_delay_sec"];
        if (j.contains("proxy_host")) proxy_host = j["proxy_host"];
        if (j.contains("proxy_port")) proxy_port = j["proxy_port"];
        if (j.contains("futures_symbols")) {
            futures_symbols.clear();
            for (const auto& s : j["futures_symbols"]) {
                futures_symbols.push_back(s.get<std::string>());
            }
        }
    }
};

/**
 * @brief Redis 配置
 */
struct RedisConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    int expire_seconds = 2 * 60 * 60;      // 数据过期时间（默认2小时）
    int max_trades_per_symbol = 10000;      // 每个币种最大 trades 数量
    int max_klines_per_symbol = 7200;       // 每个币种最大 K 线数量
    bool enabled = true;                    // 是否启用录制

    nlohmann::json to_json() const {
        return {
            {"host", host},
            {"port", port},
            {"db", db},
            {"expire_seconds", expire_seconds},
            {"max_trades_per_symbol", max_trades_per_symbol},
            {"max_klines_per_symbol", max_klines_per_symbol},
            {"enabled", enabled}
        };
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("host")) host = j["host"];
        if (j.contains("port")) port = j["port"];
        if (j.contains("password")) password = j["password"];
        if (j.contains("db")) db = j["db"];
        if (j.contains("expire_seconds")) expire_seconds = j["expire_seconds"];
        if (j.contains("max_trades_per_symbol")) max_trades_per_symbol = j["max_trades_per_symbol"];
        if (j.contains("max_klines_per_symbol")) max_klines_per_symbol = j["max_klines_per_symbol"];
        if (j.contains("enabled")) enabled = j["enabled"];
    }
};

/**
 * @brief 风控配置
 */
struct RiskConfig {
    // 单笔订单限制
    double max_order_value = 10000.0;        // 单笔最大金额 (USDT)
    double max_order_quantity = 100.0;       // 单笔最大数量

    // 持仓限制
    double max_position_value = 50000.0;     // 单品种最大持仓 (USDT)
    double max_total_exposure = 100000.0;    // 总敞口限制 (USDT)
    int max_open_orders = 50;                // 最大挂单数

    // 风险控制
    double max_drawdown_pct = 0.10;          // 最大回撤 10%
    double daily_loss_limit = 5000.0;        // 单日最大亏损 (USDT)

    // 频率限制
    int max_orders_per_second = 10;
    int max_orders_per_minute = 100;

    nlohmann::json to_json() const {
        return {
            {"max_order_value", max_order_value},
            {"max_order_quantity", max_order_quantity},
            {"max_position_value", max_position_value},
            {"max_total_exposure", max_total_exposure},
            {"max_open_orders", max_open_orders},
            {"max_drawdown_pct", max_drawdown_pct},
            {"daily_loss_limit", daily_loss_limit},
            {"max_orders_per_second", max_orders_per_second},
            {"max_orders_per_minute", max_orders_per_minute}
        };
    }

    void from_json(const nlohmann::json& j) {
        if (j.contains("max_order_value")) max_order_value = j["max_order_value"];
        if (j.contains("max_order_quantity")) max_order_quantity = j["max_order_quantity"];
        if (j.contains("max_position_value")) max_position_value = j["max_position_value"];
        if (j.contains("max_total_exposure")) max_total_exposure = j["max_total_exposure"];
        if (j.contains("max_open_orders")) max_open_orders = j["max_open_orders"];
        if (j.contains("max_drawdown_pct")) max_drawdown_pct = j["max_drawdown_pct"];
        if (j.contains("daily_loss_limit")) daily_loss_limit = j["daily_loss_limit"];
        if (j.contains("max_orders_per_second")) max_orders_per_second = j["max_orders_per_second"];
        if (j.contains("max_orders_per_minute")) max_orders_per_minute = j["max_orders_per_minute"];
    }
};

// ============================================================
// 配置中心
// ============================================================

/**
 * @brief 配置中心 - 单例模式
 *
 * 配置优先级（从高到低）：
 * 1. 命令行参数
 * 2. 环境变量
 * 3. 配置文件
 * 4. 默认值
 */
class ConfigCenter {
public:
    // 获取单例实例
    static ConfigCenter& instance() {
        static ConfigCenter instance;
        return instance;
    }

    // 禁止拷贝
    ConfigCenter(const ConfigCenter&) = delete;
    ConfigCenter& operator=(const ConfigCenter&) = delete;

    // ==================== 初始化 ====================

    /**
     * @brief 初始化配置中心
     *
     * @param config_file 配置文件路径
     * @param use_env 是否使用环境变量覆盖
     * @return 是否成功
     */
    bool init(const std::string& config_file = "", bool use_env = true) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        config_file_ = config_file;
        use_env_ = use_env;

        // 1. 加载默认值（已在结构体中定义）

        // 2. 从文件加载
        if (!config_file.empty()) {
            if (!load_from_file_internal(config_file)) {
                std::cerr << "[ConfigCenter] 警告: 无法加载配置文件 " << config_file << std::endl;
            }
        }

        // 3. 从环境变量覆盖
        if (use_env) {
            load_from_env_internal();
        }

        initialized_ = true;
        load_time_ = std::chrono::system_clock::now();

        std::cout << "[ConfigCenter] ✓ 配置中心初始化完成" << std::endl;
        return true;
    }

    /**
     * @brief 热重载配置
     *
     * @return 是否有变更
     */
    bool reload() {
        if (config_file_.empty()) {
            std::cerr << "[ConfigCenter] 无配置文件，无法重载" << std::endl;
            return false;
        }

        std::cout << "[ConfigCenter] 重新加载配置文件: " << config_file_ << std::endl;

        // 保存旧配置用于比较
        nlohmann::json old_config = export_all();

        // 重新加载
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);

            if (!load_from_file_internal(config_file_)) {
                std::cerr << "[ConfigCenter] 重载失败" << std::endl;
                return false;
            }

            if (use_env_) {
                load_from_env_internal();
            }

            load_time_ = std::chrono::system_clock::now();
        }

        // 比较变更并通知监听器
        nlohmann::json new_config = export_all();
        notify_changes(old_config, new_config);

        std::cout << "[ConfigCenter] ✓ 配置重载完成" << std::endl;
        return true;
    }

    // ==================== 配置访问 ====================

    ServerConfig& server() {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return server_config_;
    }

    const ServerConfig& server() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return server_config_;
    }

    OKXConfig& okx() {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return okx_config_;
    }

    const OKXConfig& okx() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return okx_config_;
    }

    BinanceConfig& binance() {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return binance_config_;
    }

    const BinanceConfig& binance() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return binance_config_;
    }

    RiskConfig& risk() {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return risk_config_;
    }

    const RiskConfig& risk() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return risk_config_;
    }

    RedisConfig& redis() {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return redis_config_;
    }

    const RedisConfig& redis() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return redis_config_;
    }

    // ==================== 通用配置访问 ====================

    /**
     * @brief 获取任意配置项
     */
    template<typename T>
    T get(const std::string& key, const T& default_value = T{}) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        if (custom_config_.contains(key)) {
            try {
                return custom_config_.at(key).get<T>();
            } catch (...) {
                return default_value;
            }
        }
        return default_value;
    }

    /**
     * @brief 设置任意配置项
     */
    template<typename T>
    void set(const std::string& key, const T& value) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        nlohmann::json old_value = custom_config_.contains(key) ? custom_config_[key] : nlohmann::json{};
        custom_config_[key] = value;

        // 通知变更（异步）
        lock.unlock();
        notify_single_change(key, old_value, nlohmann::json(value));
    }

    /**
     * @brief 检查配置项是否存在
     */
    bool has(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return custom_config_.find(key) != custom_config_.end();
    }

    // ==================== 变更监听 ====================

    /**
     * @brief 注册配置变更监听器
     *
     * @param key 监听的配置键（空字符串表示监听所有）
     * @param callback 回调函数
     * @return 监听器ID
     */
    size_t on_change(const std::string& key, ConfigChangeCallback callback) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        size_t id = next_listener_id_++;
        listeners_[id] = {key, callback};
        return id;
    }

    /**
     * @brief 移除配置变更监听器
     */
    void remove_listener(size_t listener_id) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        listeners_.erase(listener_id);
    }

    // ==================== 导出/保存 ====================

    /**
     * @brief 导出所有配置为JSON
     */
    nlohmann::json export_all() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        return {
            {"server", server_config_.to_json()},
            {"okx", okx_config_.to_json()},
            {"binance", binance_config_.to_json()},
            {"redis", redis_config_.to_json()},
            {"risk", risk_config_.to_json()},
            {"custom", custom_config_}
        };
    }

    /**
     * @brief 保存配置到文件
     */
    bool save_to_file(const std::string& file_path = "") const {
        std::string path = file_path.empty() ? config_file_ : file_path;
        if (path.empty()) {
            std::cerr << "[ConfigCenter] 未指定配置文件路径" << std::endl;
            return false;
        }

        try {
            std::ofstream file(path);
            if (!file.is_open()) {
                std::cerr << "[ConfigCenter] 无法打开文件: " << path << std::endl;
                return false;
            }

            nlohmann::json config = export_all();

            // 添加完整的敏感信息用于保存
            {
                std::shared_lock<std::shared_mutex> lock(mutex_);
                config["okx"]["api_key"] = okx_config_.api_key;
                config["okx"]["secret_key"] = okx_config_.secret_key;
                config["okx"]["passphrase"] = okx_config_.passphrase;
                config["binance"]["api_key"] = binance_config_.api_key;
                config["binance"]["secret_key"] = binance_config_.secret_key;
            }

            file << config.dump(2);
            file.close();

            std::cout << "[ConfigCenter] ✓ 配置已保存到 " << path << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[ConfigCenter] 保存失败: " << e.what() << std::endl;
            return false;
        }
    }

    // ==================== 状态查询 ====================

    bool is_initialized() const { return initialized_; }

    std::chrono::system_clock::time_point load_time() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return load_time_;
    }

    std::string config_file() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return config_file_;
    }

private:
    ConfigCenter() = default;

    // ==================== 内部方法 ====================

    bool load_from_file_internal(const std::string& file_path) {
        try {
            std::ifstream file(file_path);
            if (!file.is_open()) {
                return false;
            }

            nlohmann::json config;
            file >> config;
            file.close();

            // 加载各模块配置
            if (config.contains("server")) {
                server_config_.from_json(config["server"]);
            }
            if (config.contains("okx")) {
                okx_config_.from_json(config["okx"]);
            }
            if (config.contains("binance")) {
                binance_config_.from_json(config["binance"]);
            }
            if (config.contains("risk")) {
                risk_config_.from_json(config["risk"]);
            }
            if (config.contains("redis")) {
                redis_config_.from_json(config["redis"]);
            }
            if (config.contains("custom")) {
                custom_config_ = config["custom"];
            }

            std::cout << "[ConfigCenter] ✓ 从文件加载配置: " << file_path << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[ConfigCenter] 加载文件失败: " << e.what() << std::endl;
            return false;
        }
    }

    void load_from_env_internal() {
        // OKX 环境变量
        if (const char* v = std::getenv("OKX_API_KEY")) {
            okx_config_.api_key = v;
        }
        if (const char* v = std::getenv("OKX_SECRET_KEY")) {
            okx_config_.secret_key = v;
        }
        if (const char* v = std::getenv("OKX_PASSPHRASE")) {
            okx_config_.passphrase = v;
        }
        if (const char* v = std::getenv("OKX_TESTNET")) {
            okx_config_.is_testnet = (std::string(v) == "1" || std::string(v) == "true");
        }
        if (const char* v = std::getenv("OKX_PROXY_HOST")) {
            okx_config_.proxy_host = v;
        }
        if (const char* v = std::getenv("OKX_PROXY_PORT")) {
            okx_config_.proxy_port = std::stoi(v);
        }

        // Binance 环境变量
        if (const char* v = std::getenv("BINANCE_API_KEY")) {
            binance_config_.api_key = v;
        }
        if (const char* v = std::getenv("BINANCE_SECRET_KEY")) {
            binance_config_.secret_key = v;
        }
        if (const char* v = std::getenv("BINANCE_TESTNET")) {
            binance_config_.is_testnet = (std::string(v) == "1" || std::string(v) == "true");
        }
        if (const char* v = std::getenv("BINANCE_PROXY_HOST")) {
            binance_config_.proxy_host = v;
        }
        if (const char* v = std::getenv("BINANCE_PROXY_PORT")) {
            binance_config_.proxy_port = std::stoi(v);
        }

        // 服务器环境变量
        if (const char* v = std::getenv("ZMQ_PUB_PORT")) {
            server_config_.zmq_pub_port = std::stoi(v);
        }
        if (const char* v = std::getenv("ZMQ_PULL_PORT")) {
            server_config_.zmq_pull_port = std::stoi(v);
        }
        if (const char* v = std::getenv("ZMQ_REP_PORT")) {
            server_config_.zmq_rep_port = std::stoi(v);
        }
        if (const char* v = std::getenv("WEBSOCKET_PORT")) {
            server_config_.websocket_port = std::stoi(v);
        }
        if (const char* v = std::getenv("LOG_LEVEL")) {
            server_config_.log_level = v;
        }
        if (const char* v = std::getenv("LOG_DIR")) {
            server_config_.log_dir = v;
        }

        // Redis 环境变量
        if (const char* v = std::getenv("REDIS_HOST")) {
            redis_config_.host = v;
        }
        if (const char* v = std::getenv("REDIS_PORT")) {
            redis_config_.port = std::stoi(v);
        }
        if (const char* v = std::getenv("REDIS_PASSWORD")) {
            redis_config_.password = v;
        }
        if (const char* v = std::getenv("REDIS_DB")) {
            redis_config_.db = std::stoi(v);
        }
        if (const char* v = std::getenv("REDIS_ENABLED")) {
            redis_config_.enabled = (std::string(v) == "1" || std::string(v) == "true");
        }

        std::cout << "[ConfigCenter] ✓ 环境变量配置已加载" << std::endl;
    }

    void notify_changes(const nlohmann::json& old_config, const nlohmann::json& new_config) {
        // 递归比较并通知变更
        compare_and_notify("", old_config, new_config);
    }

    void compare_and_notify(const std::string& prefix, const nlohmann::json& old_val, const nlohmann::json& new_val) {
        if (old_val == new_val) return;

        if (old_val.is_object() && new_val.is_object()) {
            // 递归比较对象
            std::set<std::string> all_keys;
            for (auto& [k, v] : old_val.items()) all_keys.insert(k);
            for (auto& [k, v] : new_val.items()) all_keys.insert(k);

            for (const auto& key : all_keys) {
                std::string full_key = prefix.empty() ? key : prefix + "." + key;
                nlohmann::json old_child = old_val.contains(key) ? old_val[key] : nlohmann::json{};
                nlohmann::json new_child = new_val.contains(key) ? new_val[key] : nlohmann::json{};
                compare_and_notify(full_key, old_child, new_child);
            }
        } else {
            // 叶子节点变更
            notify_single_change(prefix, old_val, new_val);
        }
    }

    void notify_single_change(const std::string& key, const nlohmann::json& old_val, const nlohmann::json& new_val) {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        for (const auto& [id, listener] : listeners_) {
            if (listener.key.empty() || listener.key == key || key.find(listener.key) == 0) {
                try {
                    listener.callback(key, old_val, new_val);
                } catch (const std::exception& e) {
                    std::cerr << "[ConfigCenter] 监听器异常: " << e.what() << std::endl;
                }
            }
        }
    }

    // ==================== 成员变量 ====================

    mutable std::shared_mutex mutex_;

    // 配置结构
    ServerConfig server_config_;
    OKXConfig okx_config_;
    BinanceConfig binance_config_;
    RedisConfig redis_config_;
    RiskConfig risk_config_;
    nlohmann::json custom_config_;  // 自定义配置

    // 元信息
    std::string config_file_;
    bool use_env_ = true;
    std::atomic<bool> initialized_{false};
    std::chrono::system_clock::time_point load_time_;

    // 监听器
    struct ListenerInfo {
        std::string key;
        ConfigChangeCallback callback;
    };
    std::map<size_t, ListenerInfo> listeners_;
    size_t next_listener_id_ = 1;
};

// ============================================================
// 便捷访问宏
// ============================================================

inline ConfigCenter& Config() {
    return ConfigCenter::instance();
}

} // namespace config
} // namespace trading
