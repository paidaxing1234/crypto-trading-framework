/**
 * @file redis_recorder.cpp
 * @brief Redis 数据录制模块实现
 *
 * @author Sequence Team
 * @date 2026-01
 */

#include "redis_recorder.h"
#include <iostream>
#include <cstdarg>
#include <chrono>

namespace trading {
namespace server {

// 全局 Redis 录制器实例
std::unique_ptr<RedisRecorder> g_redis_recorder;

RedisRecorder::RedisRecorder() = default;

RedisRecorder::~RedisRecorder() {
    stop();
}

void RedisRecorder::set_config(const RedisConfig& config) {
    config_ = config;
}

bool RedisRecorder::start() {
    if (running_.load()) {
        return true;
    }

    if (!config_.enabled) {
        log_info("[RedisRecorder] 录制功能已禁用");
        return true;
    }

    if (!connect()) {
        log_error("[RedisRecorder] Redis 连接失败");
        return false;
    }

    running_.store(true);
    log_info("[RedisRecorder] 启动成功，开始录制行情数据");
    return true;
}

void RedisRecorder::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    disconnect();

    log_info("[RedisRecorder] 已停止");
    log_info("[RedisRecorder] 统计: Trades=" + std::to_string(trade_count_.load()) +
             " K线=" + std::to_string(kline_count_.load()) +
             " 深度=" + std::to_string(orderbook_count_.load()) +
             " 资金费率=" + std::to_string(funding_rate_count_.load()) +
             " 错误=" + std::to_string(error_count_.load()));
}

bool RedisRecorder::is_connected() const {
    return context_ != nullptr && context_->err == 0;
}

bool RedisRecorder::connect() {
    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (context_) {
        redisFree(context_);
        context_ = nullptr;
    }

    context_ = redisConnect(config_.host.c_str(), config_.port);

    if (context_ == nullptr || context_->err) {
        if (context_) {
            log_error("[RedisRecorder] 连接失败: " + std::string(context_->errstr));
            redisFree(context_);
            context_ = nullptr;
        } else {
            log_error("[RedisRecorder] 无法分配 Redis context");
        }
        return false;
    }

    // 认证
    if (!config_.password.empty()) {
        redisReply* reply = (redisReply*)redisCommand(context_, "AUTH %s", config_.password.c_str());
        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            log_error("[RedisRecorder] 认证失败");
            if (reply) freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);
    }

    // 选择数据库
    if (config_.db != 0) {
        redisReply* reply = (redisReply*)redisCommand(context_, "SELECT %d", config_.db);
        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            log_error("[RedisRecorder] 选择数据库失败");
            if (reply) freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);
    }

    // 测试连接
    redisReply* reply = (redisReply*)redisCommand(context_, "PING");
    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        log_error("[RedisRecorder] PING 失败");
        if (reply) freeReplyObject(reply);
        return false;
    }
    freeReplyObject(reply);

    log_info("[RedisRecorder] Redis 连接成功: " + config_.host + ":" + std::to_string(config_.port));
    return true;
}

void RedisRecorder::disconnect() {
    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (context_) {
        redisFree(context_);
        context_ = nullptr;
    }
}

bool RedisRecorder::reconnect() {
    disconnect();
    return connect();
}

void RedisRecorder::record_trade(const std::string& symbol, const std::string& exchange,
                                  const nlohmann::json& data) {
    if (!running_.load() || !config_.enabled) return;

    // 锁外准备数据（PERF-C6: 减少锁持有时间）
    nlohmann::json trade_data = data;
    trade_data["exchange"] = exchange;
    trade_data["symbol"] = symbol;

    std::string key = "trades:" + symbol;
    std::string value = trade_data.dump();

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return;
        }
    }

    // Pipeline: 3条命令一次往返（PERF-C1）
    redisAppendCommand(context_, "LPUSH %s %s", key.c_str(), value.c_str());
    redisAppendCommand(context_, "LTRIM %s 0 %d", key.c_str(), config_.max_trades_per_symbol - 1);
    redisAppendCommand(context_, "EXPIRE %s %d", key.c_str(), config_.expire_seconds);

    for (int i = 0; i < 3; i++) {
        redisReply* reply = nullptr;
        if (redisGetReply(context_, (void**)&reply) != REDIS_OK || reply == nullptr) {
            error_count_++;
            if (reply) freeReplyObject(reply);
            // pipeline 中断时需要 drain 剩余 reply
            for (int j = i + 1; j < 3; j++) {
                redisReply* r = nullptr;
                redisGetReply(context_, (void**)&r);
                if (r) freeReplyObject(r);
            }
            return;
        }
        if (reply->type == REDIS_REPLY_ERROR && i == 0) {
            error_count_++;
            freeReplyObject(reply);
            for (int j = i + 1; j < 3; j++) {
                redisReply* r = nullptr;
                redisGetReply(context_, (void**)&r);
                if (r) freeReplyObject(r);
            }
            return;
        }
        freeReplyObject(reply);
    }

    trade_count_++;
}

void RedisRecorder::record_kline(const std::string& symbol, const std::string& interval,
                                  const std::string& exchange, const nlohmann::json& data) {
    if (!running_.load() || !config_.enabled) return;

    // 锁外准备数据（PERF-C6: 减少锁持有时间）
    int64_t timestamp = data.value("timestamp", 0LL);
    if (timestamp == 0) {
        timestamp = data.value("ts", 0LL);
        if (timestamp == 0) {
            timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
        }
    }

    nlohmann::json kline_data = data;
    kline_data["exchange"] = exchange;
    kline_data["symbol"] = symbol;
    kline_data["interval"] = interval;
    if (!kline_data.contains("timestamp")) {
        kline_data["timestamp"] = timestamp;
    }

    std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;
    std::string value = kline_data.dump();

    int max_count = 43200;
    int expire_days = 30;
    auto it = config_.kline_retention.find(interval);
    if (it != config_.kline_retention.end()) {
        max_count = it->second.max_count;
        expire_days = it->second.expire_days;
    }
    int expire_seconds = expire_days * 24 * 60 * 60;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return;
        }
    }

    // Pipeline: 3条命令一次往返（PERF-C1）
    redisAppendCommand(context_, "ZADD %s %lld %s",
        key.c_str(), (long long)timestamp, value.c_str());
    redisAppendCommand(context_, "ZREMRANGEBYRANK %s 0 -%d",
        key.c_str(), max_count + 1);
    redisAppendCommand(context_, "EXPIRE %s %d", key.c_str(), expire_seconds);

    for (int i = 0; i < 3; i++) {
        redisReply* reply = nullptr;
        if (redisGetReply(context_, (void**)&reply) != REDIS_OK || reply == nullptr) {
            error_count_++;
            if (reply) freeReplyObject(reply);
            for (int j = i + 1; j < 3; j++) {
                redisReply* r = nullptr;
                redisGetReply(context_, (void**)&r);
                if (r) freeReplyObject(r);
            }
            return;
        }
        if (reply->type == REDIS_REPLY_ERROR && i == 0) {
            error_count_++;
            freeReplyObject(reply);
            for (int j = i + 1; j < 3; j++) {
                redisReply* r = nullptr;
                redisGetReply(context_, (void**)&r);
                if (r) freeReplyObject(r);
            }
            return;
        }
        freeReplyObject(reply);
    }

    kline_count_++;

    // 如果是 1m K 线且启用了聚合，则聚合到其他周期
    if (interval == "1m" && config_.aggregate_on_receive) {
        aggregate_and_store(symbol, exchange, kline_data);
    }
}

void RedisRecorder::record_orderbook(const std::string& symbol, const std::string& exchange,
                                      const nlohmann::json& data) {
    if (!running_.load() || !config_.enabled) return;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return;
        }
    }

    // 构建完整数据
    nlohmann::json orderbook_data = data;
    orderbook_data["exchange"] = exchange;
    orderbook_data["symbol"] = symbol;

    std::string key = "orderbook:" + symbol;
    std::string value = orderbook_data.dump();

    // SET 只保留最新快照
    redisReply* reply = (redisReply*)redisCommand(
        context_, "SET %s %s EX %d",
        key.c_str(), value.c_str(), config_.expire_seconds
    );

    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        error_count_++;
        if (reply) freeReplyObject(reply);
        return;
    }
    freeReplyObject(reply);

    orderbook_count_++;
}

void RedisRecorder::record_funding_rate(const std::string& symbol, const std::string& exchange,
                                         const nlohmann::json& data) {
    if (!running_.load() || !config_.enabled) return;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return;
        }
    }

    // 获取时间戳（0LL 确保 int64_t 推导，避免 32 位截断）
    int64_t timestamp = data.value("timestamp", 0LL);
    if (timestamp == 0) {
        timestamp = data.value("ts", 0LL);
        if (timestamp == 0) {
            timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
        }
    }

    // 构建完整数据
    nlohmann::json fr_data = data;
    fr_data["exchange"] = exchange;
    fr_data["symbol"] = symbol;
    if (!fr_data.contains("timestamp")) {
        fr_data["timestamp"] = timestamp;
    }

    std::string key = "funding_rate:" + symbol;
    std::string value = fr_data.dump();

    // ZADD 添加到有序集合
    redisReply* reply = (redisReply*)redisCommand(
        context_, "ZADD %s %lld %s",
        key.c_str(), (long long)timestamp, value.c_str()
    );

    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        error_count_++;
        if (reply) freeReplyObject(reply);
        return;
    }
    freeReplyObject(reply);

    // 保持最近 100 条
    reply = (redisReply*)redisCommand(
        context_, "ZREMRANGEBYRANK %s 0 -101", key.c_str()
    );
    if (reply) freeReplyObject(reply);

    // 设置过期时间
    reply = (redisReply*)redisCommand(
        context_, "EXPIRE %s %d", key.c_str(), config_.expire_seconds
    );
    if (reply) freeReplyObject(reply);

    funding_rate_count_++;
}

void RedisRecorder::log_info(const std::string& msg) {
    std::cout << msg << std::endl;
}

void RedisRecorder::log_error(const std::string& msg) {
    std::cerr << msg << std::endl;
}

int64_t RedisRecorder::get_interval_ms(const std::string& interval) {
    if (interval == "1m") return 60 * 1000LL;
    if (interval == "5m") return 5 * 60 * 1000LL;
    if (interval == "15m") return 15 * 60 * 1000LL;
    if (interval == "30m") return 30 * 60 * 1000LL;
    if (interval == "1H" || interval == "1h") return 60 * 60 * 1000LL;
    if (interval == "4H" || interval == "4h") return 4 * 60 * 60 * 1000LL;
    if (interval == "1D" || interval == "1d") return 24 * 60 * 60 * 1000LL;
    return 60 * 1000LL;  // 默认 1m
}

int64_t RedisRecorder::align_timestamp(int64_t ts, int64_t interval_ms) {
    return (ts / interval_ms) * interval_ms;
}

void RedisRecorder::aggregate_and_store(const std::string& symbol, const std::string& exchange,
                                         const nlohmann::json& data) {
    // 解析 1m K 线数据
    int64_t timestamp = data.value("timestamp", 0LL);
    double open = 0, high = 0, low = 0, close = 0, volume = 0, vol_ccy = 0;

    // 尝试解析 OHLCV 数据
    if (data.contains("open")) {
        open = std::stod(data["open"].get<std::string>());
        high = std::stod(data["high"].get<std::string>());
        low = std::stod(data["low"].get<std::string>());
        close = std::stod(data["close"].get<std::string>());
        volume = data.contains("vol") ? std::stod(data["vol"].get<std::string>()) : 0;
        vol_ccy = data.contains("volCcy") ? std::stod(data["volCcy"].get<std::string>()) : 0;
    } else if (data.contains("o")) {
        open = std::stod(data["o"].get<std::string>());
        high = std::stod(data["h"].get<std::string>());
        low = std::stod(data["l"].get<std::string>());
        close = std::stod(data["c"].get<std::string>());
        volume = data.contains("vol") ? std::stod(data["vol"].get<std::string>()) : 0;
        vol_ccy = data.contains("volCcy") ? std::stod(data["volCcy"].get<std::string>()) : 0;
    } else {
        return;  // 无法解析
    }

    std::lock_guard<std::mutex> agg_lock(aggregate_mutex_);

    // 对每个目标周期进行聚合
    for (const auto& target_interval : aggregate_intervals_) {
        int64_t interval_ms = get_interval_ms(target_interval);
        int64_t period_start = align_timestamp(timestamp, interval_ms);

        std::string buffer_key = symbol + ":" + exchange + ":" + target_interval;
        auto& buffer = aggregate_buffers_[buffer_key];

        // 检查是否是新周期
        if (buffer.period_start != period_start) {
            // 如果有旧数据，先存储
            if (buffer.period_start > 0 && buffer.bar_count > 0) {
                store_aggregated_kline(symbol, exchange, target_interval, buffer);
            }

            // 开始新周期
            buffer.period_start = period_start;
            buffer.open = open;
            buffer.high = high;
            buffer.low = low;
            buffer.close = close;
            buffer.volume = volume;
            buffer.vol_ccy = vol_ccy;
            buffer.bar_count = 1;
        } else {
            // 继续聚合当前周期
            buffer.high = std::max(buffer.high, high);
            buffer.low = std::min(buffer.low, low);
            buffer.close = close;
            buffer.volume += volume;
            buffer.vol_ccy += vol_ccy;
            buffer.bar_count++;
        }

        // 检查周期是否完成（根据 bar_count 判断）
        int bars_per_period = interval_ms / (60 * 1000);  // 每个周期需要多少个 1m K 线
        if (buffer.bar_count >= bars_per_period) {
            store_aggregated_kline(symbol, exchange, target_interval, buffer);
            // 重置 buffer
            buffer.period_start = 0;
            buffer.bar_count = 0;
        }
    }
}

void RedisRecorder::store_aggregated_kline(const std::string& symbol, const std::string& exchange,
                                            const std::string& interval, const KlineAggregateBuffer& buffer) {
    // 构建 K 线 JSON
    nlohmann::json kline_data;
    kline_data["timestamp"] = buffer.period_start;
    kline_data["open"] = std::to_string(buffer.open);
    kline_data["high"] = std::to_string(buffer.high);
    kline_data["low"] = std::to_string(buffer.low);
    kline_data["close"] = std::to_string(buffer.close);
    kline_data["vol"] = std::to_string(buffer.volume);
    kline_data["volCcy"] = std::to_string(buffer.vol_ccy);
    kline_data["exchange"] = exchange;
    kline_data["symbol"] = symbol;
    kline_data["interval"] = interval;

    std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;
    std::string value = kline_data.dump();

    // 注意：这里不需要加锁，因为调用者已经持有 aggregate_mutex_
    // 但需要确保 redis_mutex_ 已被持有（在 record_kline 中已加锁）

    // ZADD 添加到有序集合
    redisReply* reply = (redisReply*)redisCommand(
        context_, "ZADD %s %lld %s",
        key.c_str(), (long long)buffer.period_start, value.c_str()
    );

    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        error_count_++;
        if (reply) freeReplyObject(reply);
        return;
    }
    freeReplyObject(reply);

    // 根据周期获取保存配置
    int max_count = 43200;
    int expire_days = 30;
    auto it = config_.kline_retention.find(interval);
    if (it != config_.kline_retention.end()) {
        max_count = it->second.max_count;
        expire_days = it->second.expire_days;
    }

    // ZREMRANGEBYRANK 保持有序集合大小
    reply = (redisReply*)redisCommand(
        context_, "ZREMRANGEBYRANK %s 0 -%d",
        key.c_str(), max_count + 1
    );
    if (reply) freeReplyObject(reply);

    // 设置过期时间
    int expire_seconds = expire_days * 24 * 60 * 60;
    reply = (redisReply*)redisCommand(
        context_, "EXPIRE %s %d", key.c_str(), expire_seconds
    );
    if (reply) freeReplyObject(reply);

    kline_count_++;
}

} // namespace server
} // namespace trading
