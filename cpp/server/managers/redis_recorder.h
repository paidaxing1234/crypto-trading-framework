/**
 * @file redis_recorder.h
 * @brief Redis 数据录制模块 - 将行情数据实时存入 Redis
 *
 * 功能：
 * 1. 订阅 ZMQ 行情数据（trades, K线, 深度, 资金费率）
 * 2. 将数据存入 Redis（除订单数据外）
 * 3. 集成到主服务器，随服务器启动
 *
 * Redis 数据结构：
 * - trades:{symbol} -> List (最近的 trades)
 * - kline:{symbol}:{interval} -> Sorted Set (score=timestamp)
 * - orderbook:{symbol} -> Hash (最新深度快照)
 * - funding_rate:{symbol} -> Sorted Set (score=timestamp)
 *
 * @author Sequence Team
 * @date 2026-01
 */

#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <functional>
#include <map>
#include <vector>
#include <deque>

#ifdef HAS_HIREDIS
#include <hiredis/hiredis.h>
#endif
#include <nlohmann/json.hpp>

namespace trading {
namespace server {

/**
 * @brief Redis 配置
 */
struct RedisConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    int expire_seconds = 60 * 24 * 60 * 60;  // 默认数据过期时间（60天）
    int max_trades_per_symbol = 100000;       // 每个币种最大 trades 数量
    bool enabled = true;                      // 是否启用录制
    bool aggregate_on_receive = true;         // 收到1m K线时自动聚合生成其他周期

    // 不同周期的 K 线保存配置
    struct KlineRetention {
        int max_count;      // 最大保存数量
        int expire_days;    // 过期天数
    };

    // 各周期保存配置: 1m/5m/15m/30m=2个月, 1H=6个月, 4H=12个月, 1D=24个月
    std::map<std::string, KlineRetention> kline_retention = {
        {"1m",  {60 * 24 * 60, 60}},      // 2个月: 86400 条
        {"5m",  {60 * 24 * 12, 60}},      // 2个月: 17280 条
        {"15m", {60 * 24 * 4, 60}},       // 2个月: 5760 条
        {"30m", {60 * 24 * 2, 60}},       // 2个月: 2880 条
        {"1H",  {180 * 24, 180}},         // 6个月: 4320 条
        {"4H",  {365 * 6, 365}},          // 12个月: 2190 条
        {"1D",  {730, 730}}               // 24个月: 730 条
    };
};

/**
 * @brief K 线聚合缓存结构
 */
struct KlineAggregateBuffer {
    int64_t period_start = 0;    // 当前聚合周期开始时间
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    double volume = 0;
    double vol_ccy = 0;          // 成交额
    int bar_count = 0;           // 已聚合的 1m K 线数量
};

/**
 * @brief Redis 数据录制器
 *
 * 接收行情数据并存入 Redis，用于策略获取历史数据
 */
class RedisRecorder {
public:
    RedisRecorder();
    ~RedisRecorder();

    /**
     * @brief 设置配置
     */
    void set_config(const RedisConfig& config);

    /**
     * @brief 获取配置
     */
    const RedisConfig& get_config() const { return config_; }

    /**
     * @brief 启动录制器
     * @return 是否成功
     */
    bool start();

    /**
     * @brief 停止录制器
     */
    void stop();

    /**
     * @brief 是否正在运行
     */
    bool is_running() const { return running_.load(); }

    /**
     * @brief 是否已连接 Redis
     */
    bool is_connected() const;

    // ==================== 数据录制接口 ====================

    /**
     * @brief 录制 trade 数据
     * @param symbol 交易对
     * @param exchange 交易所 (okx/binance)
     * @param data 原始数据 JSON
     */
    void record_trade(const std::string& symbol, const std::string& exchange,
                      const nlohmann::json& data);

    /**
     * @brief 录制 K 线数据
     * @param symbol 交易对
     * @param interval K 线周期
     * @param exchange 交易所
     * @param data 原始数据 JSON
     */
    void record_kline(const std::string& symbol, const std::string& interval,
                      const std::string& exchange, const nlohmann::json& data);

    /**
     * @brief 录制深度数据（只保留最新快照）
     * @param symbol 交易对
     * @param exchange 交易所
     * @param data 原始数据 JSON
     */
    void record_orderbook(const std::string& symbol, const std::string& exchange,
                          const nlohmann::json& data);

    /**
     * @brief 录制资金费率
     * @param symbol 交易对
     * @param exchange 交易所
     * @param data 原始数据 JSON
     */
    void record_funding_rate(const std::string& symbol, const std::string& exchange,
                             const nlohmann::json& data);

    // ==================== 统计 ====================

    uint64_t get_trade_count() const { return trade_count_.load(); }
    uint64_t get_kline_count() const { return kline_count_.load(); }
    uint64_t get_orderbook_count() const { return orderbook_count_.load(); }
    uint64_t get_funding_rate_count() const { return funding_rate_count_.load(); }
    uint64_t get_error_count() const { return error_count_.load(); }

private:
    /**
     * @brief 连接到 Redis
     */
    bool connect();

    /**
     * @brief 断开 Redis 连接
     */
    void disconnect();

    /**
     * @brief 重连逻辑
     */
    bool reconnect();

    /**
     * @brief 执行 Redis 命令（带重试）
     */
    bool execute_command(const char* format, ...);

    /**
     * @brief 日志输出
     */
    void log_info(const std::string& msg);
    void log_error(const std::string& msg);

    /**
     * @brief 处理 1m K 线聚合到其他周期
     * @param symbol 交易对
     * @param exchange 交易所
     * @param data 1m K 线数据
     */
    void aggregate_and_store(const std::string& symbol, const std::string& exchange,
                             const nlohmann::json& data);

    /**
     * @brief 存储聚合后的 K 线
     */
    void store_aggregated_kline(const std::string& symbol, const std::string& exchange,
                                const std::string& interval, const KlineAggregateBuffer& buffer);

    /**
     * @brief 获取周期的毫秒数
     */
    int64_t get_interval_ms(const std::string& interval);

    /**
     * @brief 对齐时间戳到周期边界
     */
    int64_t align_timestamp(int64_t ts, int64_t interval_ms);

private:
    RedisConfig config_;
#ifdef HAS_HIREDIS
    redisContext* context_ = nullptr;
#else
    void* context_ = nullptr;
#endif
    std::mutex redis_mutex_;
    std::atomic<bool> running_{false};

    // 聚合缓存: key = "symbol:exchange:interval"
    std::map<std::string, KlineAggregateBuffer> aggregate_buffers_;
    std::mutex aggregate_mutex_;

    // 需要聚合的周期列表
    const std::vector<std::string> aggregate_intervals_ = {"5m", "15m", "1H", "4H", "1D"};

    // 统计
    std::atomic<uint64_t> trade_count_{0};
    std::atomic<uint64_t> kline_count_{0};
    std::atomic<uint64_t> orderbook_count_{0};
    std::atomic<uint64_t> funding_rate_count_{0};
    std::atomic<uint64_t> error_count_{0};
};

// 全局 Redis 录制器实例
extern std::unique_ptr<RedisRecorder> g_redis_recorder;

} // namespace server
} // namespace trading
