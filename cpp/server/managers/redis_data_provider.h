/**
 * @file redis_data_provider.h
 * @brief Redis 数据查询模块 - 为策略端提供历史 K 线数据查询接口
 *
 * 功能：
 * 1. 查询指定时间范围的 K 线数据
 * 2. 查询最近 N 天的 K 线数据
 * 3. 支持不同时间周期的 K 线聚合（1m -> 5m/15m/1h/4h/1d）
 * 4. 支持 OKX 和 Binance 两个交易所
 *
 * 注意：本模块只负责从 Redis 读取数据，数据补齐由其他模块负责
 *
 * Redis 数据结构：
 * - kline:{exchange}:{symbol}:{interval} -> Sorted Set (score=timestamp_ms)
 *
 * @author Sequence Team
 * @date 2026-01
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>
#include <map>

#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>

namespace trading {
namespace server {

/**
 * @brief K 线数据结构
 */
struct KlineBar {
    std::string symbol;         // 交易对
    std::string exchange;       // 交易所
    std::string interval;       // 时间周期
    int64_t timestamp;          // 开盘时间戳（毫秒）
    double open;                // 开盘价
    double high;                // 最高价
    double low;                 // 最低价
    double close;               // 收盘价
    double volume;              // 成交量
    double turnover;            // 成交额（可选）
    bool is_closed;             // 是否已完结

    nlohmann::json to_json() const {
        return {
            {"symbol", symbol},
            {"exchange", exchange},
            {"interval", interval},
            {"timestamp", timestamp},
            {"open", open},
            {"high", high},
            {"low", low},
            {"close", close},
            {"volume", volume},
            {"turnover", turnover},
            {"is_closed", is_closed}
        };
    }

    static KlineBar from_json(const nlohmann::json& j) {
        KlineBar bar;
        bar.symbol = j.value("symbol", "");
        bar.exchange = j.value("exchange", "");
        bar.interval = j.value("interval", "1s");
        bar.timestamp = j.value("timestamp", 0LL);  // 使用 0LL 避免整数溢出
        bar.open = j.value("open", 0.0);
        bar.high = j.value("high", 0.0);
        bar.low = j.value("low", 0.0);
        bar.close = j.value("close", 0.0);
        bar.volume = j.value("volume", 0.0);
        bar.turnover = j.value("turnover", 0.0);
        bar.is_closed = j.value("is_closed", true);
        return bar;
    }
};

/**
 * @brief Redis 数据查询配置
 */
struct RedisProviderConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    int connection_timeout_ms = 5000;   // 连接超时
    int query_timeout_ms = 10000;       // 查询超时
};

/**
 * @brief Redis 数据查询提供者
 *
 * 为策略端提供历史 K 线数据查询功能（只读）
 */
class RedisDataProvider {
public:
    RedisDataProvider();
    ~RedisDataProvider();

    /**
     * @brief 设置配置
     */
    void set_config(const RedisProviderConfig& config);

    /**
     * @brief 连接到 Redis
     * @return 是否成功
     */
    bool connect();

    /**
     * @brief 断开连接
     */
    void disconnect();

    /**
     * @brief 是否已连接
     */
    bool is_connected() const;

    // ==================== K 线查询接口 ====================

    /**
     * @brief 查询指定时间范围的 K 线数据
     * @param symbol 交易对（如 BTC-USDT-SWAP 或 BTCUSDT）
     * @param exchange 交易所（okx/binance）
     * @param interval 时间周期（1s/1m/5m/15m/1h/4h/8h/1d）
     * @param start_time 开始时间戳（毫秒）
     * @param end_time 结束时间戳（毫秒）
     * @return K 线数据列表（按时间升序）
     */
    std::vector<KlineBar> get_klines(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval,
        int64_t start_time,
        int64_t end_time
    );

    /**
     * @brief 查询最近 N 天的 K 线数据
     * @param symbol 交易对
     * @param exchange 交易所
     * @param interval 时间周期
     * @param days 天数（最大 60 天）
     * @return K 线数据列表（按时间升序）
     */
    std::vector<KlineBar> get_klines_by_days(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval,
        int days
    );

    /**
     * @brief 查询最近 N 根 K 线
     * @param symbol 交易对
     * @param exchange 交易所
     * @param interval 时间周期
     * @param count 数量
     * @return K 线数据列表（按时间升序）
     */
    std::vector<KlineBar> get_latest_klines(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval,
        int count
    );

    /**
     * @brief 从 1 分钟 K 线聚合成更大周期
     * @param symbol 交易对
     * @param exchange 交易所
     * @param target_interval 目标周期（5m/15m/1h/4h/1d）
     * @param start_time 开始时间戳（毫秒）
     * @param end_time 结束时间戳（毫秒）
     * @return 聚合后的 K 线数据列表
     */
    std::vector<KlineBar> aggregate_klines(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& target_interval,
        int64_t start_time,
        int64_t end_time
    );

    /**
     * @brief 获取可用的交易对列表
     * @param exchange 交易所（可选，空则返回所有）
     * @return 交易对列表
     */
    std::vector<std::string> get_available_symbols(const std::string& exchange = "");

    /**
     * @brief 获取指定交易对的数据时间范围
     * @param symbol 交易对
     * @param exchange 交易所
     * @param interval 时间周期
     * @return {earliest_timestamp, latest_timestamp}
     */
    std::pair<int64_t, int64_t> get_data_time_range(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval
    );

    /**
     * @brief 获取指定交易对的 K 线数量
     * @param symbol 交易对
     * @param exchange 交易所
     * @param interval 时间周期
     * @return K 线数量
     */
    int64_t get_kline_count(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval
    );

    // ==================== 批量查询接口 ====================

    /**
     * @brief 批量获取多个币种最新K线的时间戳（使用Redis Pipeline，单次往返）
     * @param symbols 交易对列表
     * @param exchange 交易所
     * @param interval 时间周期
     * @return {symbol: latest_timestamp_ms} 映射，无数据的币种不包含在结果中
     */
    std::map<std::string, int64_t> batch_get_latest_kline_timestamps(
        const std::vector<std::string>& symbols,
        const std::string& exchange,
        const std::string& interval
    );

    /**
     * @brief 批量获取多个币种最新1根K线数据（使用Redis Pipeline，单次往返）
     * @param symbols 交易对列表
     * @param exchange 交易所
     * @param interval 时间周期
     * @return {symbol: KlineBar} 映射
     */
    std::map<std::string, KlineBar> batch_get_latest_klines(
        const std::vector<std::string>& symbols,
        const std::string& exchange,
        const std::string& interval
    );

    /**
     * @brief 使用Lua脚本在Redis服务端批量获取最新时间戳（最快，单次EVALSHA）
     * @param symbols 交易对列表
     * @param exchange 交易所
     * @param interval 时间周期
     * @return {symbol: latest_timestamp_ms} 映射
     */
    std::map<std::string, int64_t> lua_batch_get_latest_timestamps(
        const std::vector<std::string>& symbols,
        const std::string& exchange,
        const std::string& interval
    );

    // ==================== 统计 ====================

    uint64_t get_query_count() const { return query_count_; }
    uint64_t get_error_count() const { return error_count_; }

private:
    /**
     * @brief 重连逻辑
     */
    bool reconnect();

    /**
     * @brief 获取周期对应的毫秒数
     */
    int64_t interval_to_ms(const std::string& interval) const;

    /**
     * @brief 对齐时间戳到周期边界
     */
    int64_t align_timestamp(int64_t timestamp, const std::string& interval) const;

    /**
     * @brief 从 Redis 查询原始 K 线数据
     */
    std::vector<KlineBar> query_raw_klines(
        const std::string& key,
        int64_t start_time,
        int64_t end_time
    );

    /**
     * @brief 聚合 K 线数据
     */
    std::vector<KlineBar> do_aggregate(
        const std::vector<KlineBar>& source_bars,
        const std::string& target_interval,
        const std::string& symbol,
        const std::string& exchange
    );

    /**
     * @brief 日志输出
     */
    void log_info(const std::string& msg);
    void log_error(const std::string& msg);

private:
    RedisProviderConfig config_;
    redisContext* context_ = nullptr;
    mutable std::mutex redis_mutex_;

    // Lua脚本SHA缓存
    std::string lua_batch_ts_sha_;

    // 统计
    mutable uint64_t query_count_ = 0;
    mutable uint64_t error_count_ = 0;
};

// 全局 Redis 数据提供者实例（策略端使用）
extern std::unique_ptr<RedisDataProvider> g_redis_data_provider;

} // namespace server
} // namespace trading
