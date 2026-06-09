/**
 * @file redis_data_provider.cpp
 * @brief Redis 数据查询模块实现（只读）
 *
 * @author Sequence Team
 * @date 2026-01
 */

#include "redis_data_provider.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <set>

namespace trading {
namespace server {

// 全局 Redis 数据提供者实例
std::unique_ptr<RedisDataProvider> g_redis_data_provider;

RedisDataProvider::RedisDataProvider() = default;

RedisDataProvider::~RedisDataProvider() {
    disconnect();
}

void RedisDataProvider::set_config(const RedisProviderConfig& config) {
    config_ = config;
}

bool RedisDataProvider::connect() {
    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (context_) {
        redisFree(context_);
        context_ = nullptr;
    }

    // 设置连接超时
    struct timeval timeout = {
        config_.connection_timeout_ms / 1000,
        (config_.connection_timeout_ms % 1000) * 1000
    };

    context_ = redisConnectWithTimeout(config_.host.c_str(), config_.port, timeout);

    if (context_ == nullptr || context_->err) {
        if (context_) {
            log_error("[RedisDataProvider] 连接失败: " + std::string(context_->errstr));
            redisFree(context_);
            context_ = nullptr;
        } else {
            log_error("[RedisDataProvider] 无法分配 Redis context");
        }
        return false;
    }

    // 认证
    if (!config_.password.empty()) {
        redisReply* reply = (redisReply*)redisCommand(context_, "AUTH %s", config_.password.c_str());
        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            log_error("[RedisDataProvider] 认证失败");
            if (reply) freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);
    }

    // 选择数据库
    if (config_.db != 0) {
        redisReply* reply = (redisReply*)redisCommand(context_, "SELECT %d", config_.db);
        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            log_error("[RedisDataProvider] 选择数据库失败");
            if (reply) freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);
    }

    log_info("[RedisDataProvider] Redis 连接成功: " + config_.host + ":" + std::to_string(config_.port));
    return true;
}

void RedisDataProvider::disconnect() {
    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (context_) {
        redisFree(context_);
        context_ = nullptr;
    }
}

bool RedisDataProvider::is_connected() const {
    return context_ != nullptr && context_->err == 0;
}

bool RedisDataProvider::reconnect() {
    disconnect();
    return connect();
}

int64_t RedisDataProvider::interval_to_ms(const std::string& interval) const {
    // 解析时间周期字符串
    if (interval == "1s") return 1000;
    if (interval == "5s") return 5000;
    if (interval == "15s") return 15000;
    if (interval == "30s") return 30000;
    if (interval == "1m") return 60 * 1000;
    if (interval == "3m") return 3 * 60 * 1000;
    if (interval == "5m") return 5 * 60 * 1000;
    if (interval == "15m") return 15 * 60 * 1000;
    if (interval == "30m") return 30 * 60 * 1000;
    if (interval == "1h" || interval == "1H") return 60 * 60 * 1000;
    if (interval == "2h" || interval == "2H") return 2 * 60 * 60 * 1000;
    if (interval == "4h" || interval == "4H") return 4 * 60 * 60 * 1000;
    if (interval == "6h" || interval == "6H") return 6 * 60 * 60 * 1000;
    if (interval == "8h" || interval == "8H") return 8 * 60 * 60 * 1000;  // 添加8h支持
    if (interval == "12h" || interval == "12H") return 12 * 60 * 60 * 1000;
    if (interval == "1d" || interval == "1D") return 24 * 60 * 60 * 1000;
    if (interval == "1w" || interval == "1W") return 7 * 24 * 60 * 60 * 1000;

    // 默认返回 1 分钟
    return 60 * 1000;
}

int64_t RedisDataProvider::align_timestamp(int64_t timestamp, const std::string& interval) const {
    int64_t interval_ms = interval_to_ms(interval);
    return (timestamp / interval_ms) * interval_ms;
}

std::vector<KlineBar> RedisDataProvider::query_raw_klines(
    const std::string& key,
    int64_t start_time,
    int64_t end_time
) {
    std::vector<KlineBar> result;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return result;
        }
    }

    // 使用 ZRANGEBYSCORE 查询时间范围内的数据
    redisReply* reply = (redisReply*)redisCommand(
        context_,
        "ZRANGEBYSCORE %s %lld %lld",
        key.c_str(),
        (long long)start_time,
        (long long)end_time
    );

    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        error_count_++;
        if (reply) freeReplyObject(reply);
        return result;
    }

    query_count_++;

    if (reply->type == REDIS_REPLY_ARRAY) {
        result.reserve(reply->elements);
        for (size_t i = 0; i < reply->elements; i++) {
            try {
                std::string json_str(reply->element[i]->str, reply->element[i]->len);
                nlohmann::json j = nlohmann::json::parse(json_str);
                result.push_back(KlineBar::from_json(j));
            } catch (const std::exception& e) {
                // 解析失败，跳过
                log_error("[RedisDataProvider] JSON 解析失败: " + std::string(e.what()));
            }
        }
    }

    freeReplyObject(reply);
    return result;
}

std::vector<KlineBar> RedisDataProvider::get_klines(
    const std::string& symbol,
    const std::string& exchange,
    const std::string& interval,
    int64_t start_time,
    int64_t end_time
) {
    // 构建 Redis key
    // 格式: kline:{exchange}:{symbol}:{interval}
    std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;

    // 先尝试直接查询该周期的数据
    auto result = query_raw_klines(key, start_time, end_time);

    // 如果没有数据且请求的不是 1m，尝试从 1m 聚合
    if (result.empty() && interval != "1m") {
        result = aggregate_klines(symbol, exchange, interval, start_time, end_time);
    }

    return result;
}

std::vector<KlineBar> RedisDataProvider::get_klines_by_days(
    const std::string& symbol,
    const std::string& exchange,
    const std::string& interval,
    int days
) {
    // 限制最大 60 天
    if (days > 60) days = 60;
    if (days < 1) days = 1;

    // 计算时间范围
    auto now = std::chrono::system_clock::now();
    auto end_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    auto start_time = end_time - (int64_t)days * 24 * 60 * 60 * 1000;

    return get_klines(symbol, exchange, interval, start_time, end_time);
}

std::vector<KlineBar> RedisDataProvider::get_latest_klines(
    const std::string& symbol,
    const std::string& exchange,
    const std::string& interval,
    int count
) {
    std::vector<KlineBar> result;

    if (count <= 0) return result;

    std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return result;
        }
    }

    // 使用 ZREVRANGE 获取最新的 N 条数据（按 score 降序）
    redisReply* reply = (redisReply*)redisCommand(
        context_,
        "ZREVRANGE %s 0 %d",
        key.c_str(),
        count - 1
    );

    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        error_count_++;
        if (reply) freeReplyObject(reply);
        return result;
    }

    query_count_++;

    if (reply->type == REDIS_REPLY_ARRAY) {
        result.reserve(reply->elements);
        for (size_t i = 0; i < reply->elements; i++) {
            try {
                std::string json_str(reply->element[i]->str, reply->element[i]->len);
                nlohmann::json j = nlohmann::json::parse(json_str);
                result.push_back(KlineBar::from_json(j));
            } catch (const std::exception& e) {
                log_error("[RedisDataProvider] JSON 解析失败: " + std::string(e.what()));
            }
        }
    }

    freeReplyObject(reply);

    // 反转结果，使其按时间升序
    std::reverse(result.begin(), result.end());

    return result;
}

std::vector<KlineBar> RedisDataProvider::aggregate_klines(
    const std::string& symbol,
    const std::string& exchange,
    const std::string& target_interval,
    int64_t start_time,
    int64_t end_time
) {
    // 从 1m K 线聚合（基础周期为 1m）
    std::string source_key = "kline:" + exchange + ":" + symbol + ":1m";

    // 对齐开始时间到目标周期边界
    start_time = align_timestamp(start_time, target_interval);

    auto source_bars = query_raw_klines(source_key, start_time, end_time);

    if (source_bars.empty()) {
        return {};
    }

    return do_aggregate(source_bars, target_interval, symbol, exchange);
}

std::vector<KlineBar> RedisDataProvider::do_aggregate(
    const std::vector<KlineBar>& source_bars,
    const std::string& target_interval,
    const std::string& symbol,
    const std::string& exchange
) {
    std::vector<KlineBar> result;

    if (source_bars.empty()) return result;

    int64_t interval_ms = interval_to_ms(target_interval);

    // 按目标周期分组聚合
    std::map<int64_t, std::vector<const KlineBar*>> groups;

    for (const auto& bar : source_bars) {
        int64_t group_ts = align_timestamp(bar.timestamp, target_interval);
        groups[group_ts].push_back(&bar);
    }

    result.reserve(groups.size());

    // 计算当前未完成周期的起始时间，用于过滤 partial bar
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t current_period_start = (now_ms / interval_ms) * interval_ms;

    for (const auto& [group_ts, bars] : groups) {
        if (bars.empty()) continue;

        // 跳过当前未完成周期的 partial bar，避免污染策略的 period 判断
        if (group_ts >= current_period_start) continue;

        KlineBar aggregated;
        aggregated.symbol = symbol;
        aggregated.exchange = exchange;
        aggregated.interval = target_interval;
        aggregated.timestamp = group_ts;
        aggregated.open = bars.front()->open;
        aggregated.high = bars.front()->high;
        aggregated.low = bars.front()->low;
        aggregated.close = bars.back()->close;
        aggregated.volume = 0;
        aggregated.turnover = 0;
        aggregated.is_closed = true;

        for (const auto* bar : bars) {
            if (bar->high > aggregated.high) aggregated.high = bar->high;
            if (bar->low < aggregated.low) aggregated.low = bar->low;
            aggregated.volume += bar->volume;
            aggregated.turnover += bar->turnover;
        }

        result.push_back(aggregated);
    }

    // 按时间排序
    std::sort(result.begin(), result.end(),
        [](const KlineBar& a, const KlineBar& b) {
            return a.timestamp < b.timestamp;
        });

    return result;
}

std::vector<std::string> RedisDataProvider::get_available_symbols(const std::string& exchange) {
    std::vector<std::string> result;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return result;
        }
    }

    // 使用 SCAN 代替 KEYS，避免阻塞 Redis（PERF-C3）
    std::string pattern = exchange.empty()
        ? "kline:*:*:1m"
        : "kline:" + exchange + ":*:1m";

    long long cursor = 0;
    do {
        redisReply* reply = (redisReply*)redisCommand(
            context_,
            "SCAN %lld MATCH %s COUNT 200",
            cursor, pattern.c_str()
        );

        if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            error_count_++;
            if (reply) freeReplyObject(reply);
            break;
        }

        // element[0] = 新 cursor, element[1] = 匹配的 keys 数组
        cursor = std::stoll(reply->element[0]->str);

        if (reply->element[1]->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < reply->element[1]->elements; i++) {
                std::string key(reply->element[1]->element[i]->str,
                                reply->element[1]->element[i]->len);
                // 解析 key: kline:{exchange}:{symbol}:{interval}
                size_t pos1 = key.find(':');
                if (pos1 == std::string::npos) continue;
                size_t pos2 = key.find(':', pos1 + 1);
                if (pos2 == std::string::npos) continue;
                size_t pos3 = key.find(':', pos2 + 1);
                if (pos3 == std::string::npos) continue;

                std::string symbol = key.substr(pos2 + 1, pos3 - pos2 - 1);
                result.push_back(symbol);
            }
        }

        freeReplyObject(reply);
    } while (cursor != 0);

    query_count_++;

    // 去重
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

std::pair<int64_t, int64_t> RedisDataProvider::get_data_time_range(
    const std::string& symbol,
    const std::string& exchange,
    const std::string& interval
) {
    std::pair<int64_t, int64_t> result = {0, 0};

    std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return result;
        }
    }

    // 获取最早的时间戳
    redisReply* reply = (redisReply*)redisCommand(
        context_,
        "ZRANGE %s 0 0 WITHSCORES",
        key.c_str()
    );

    if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2) {
        result.first = std::stoll(reply->element[1]->str);
    }
    if (reply) freeReplyObject(reply);

    // 获取最新的时间戳
    reply = (redisReply*)redisCommand(
        context_,
        "ZREVRANGE %s 0 0 WITHSCORES",
        key.c_str()
    );

    if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2) {
        result.second = std::stoll(reply->element[1]->str);
    }
    if (reply) freeReplyObject(reply);

    query_count_ += 2;

    return result;
}

int64_t RedisDataProvider::get_kline_count(
    const std::string& symbol,
    const std::string& exchange,
    const std::string& interval
) {
    std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return 0;
        }
    }

    redisReply* reply = (redisReply*)redisCommand(
        context_,
        "ZCARD %s",
        key.c_str()
    );

    int64_t count = 0;
    if (reply && reply->type == REDIS_REPLY_INTEGER) {
        count = reply->integer;
    }
    if (reply) freeReplyObject(reply);

    query_count_++;

    return count;
}

void RedisDataProvider::log_info(const std::string& msg) {
    std::cout << msg << std::endl;
}

void RedisDataProvider::log_error(const std::string& msg) {
    std::cerr << msg << std::endl;
}

std::map<std::string, int64_t> RedisDataProvider::batch_get_latest_kline_timestamps(
    const std::vector<std::string>& symbols,
    const std::string& exchange,
    const std::string& interval
) {
    std::map<std::string, int64_t> result;

    if (symbols.empty()) return result;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return result;
        }
    }

    // Pipeline: 批量发送 ZREVRANGE key 0 0 WITHSCORES
    for (const auto& symbol : symbols) {
        std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;
        redisAppendCommand(context_, "ZREVRANGE %s 0 0 WITHSCORES", key.c_str());
    }

    // 批量读取回复
    for (size_t i = 0; i < symbols.size(); i++) {
        redisReply* reply = nullptr;
        if (redisGetReply(context_, (void**)&reply) != REDIS_OK || reply == nullptr) {
            error_count_++;
            if (reply) freeReplyObject(reply);
            continue;
        }

        if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2) {
            // elements[0] = value (JSON), elements[1] = score (timestamp)
            int64_t ts = std::stoll(std::string(reply->element[1]->str, reply->element[1]->len));
            result[symbols[i]] = ts;
        }

        freeReplyObject(reply);
    }

    query_count_ += symbols.size();
    return result;
}

std::map<std::string, KlineBar> RedisDataProvider::batch_get_latest_klines(
    const std::vector<std::string>& symbols,
    const std::string& exchange,
    const std::string& interval
) {
    std::map<std::string, KlineBar> result;

    if (symbols.empty()) return result;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return result;
        }
    }

    // Pipeline: 批量发送 ZREVRANGE key 0 0
    for (const auto& symbol : symbols) {
        std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;
        redisAppendCommand(context_, "ZREVRANGE %s 0 0", key.c_str());
    }

    // 批量读取回复
    for (size_t i = 0; i < symbols.size(); i++) {
        redisReply* reply = nullptr;
        if (redisGetReply(context_, (void**)&reply) != REDIS_OK || reply == nullptr) {
            error_count_++;
            if (reply) freeReplyObject(reply);
            continue;
        }

        if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 1) {
            try {
                std::string json_str(reply->element[0]->str, reply->element[0]->len);
                nlohmann::json j = nlohmann::json::parse(json_str);
                result[symbols[i]] = KlineBar::from_json(j);
            } catch (const std::exception&) {
                // skip
            }
        }

        freeReplyObject(reply);
    }

    query_count_ += symbols.size();
    return result;
}

std::map<std::string, int64_t> RedisDataProvider::lua_batch_get_latest_timestamps(
    const std::vector<std::string>& symbols,
    const std::string& exchange,
    const std::string& interval
) {
    std::map<std::string, int64_t> result;

    if (symbols.empty()) return result;

    std::lock_guard<std::mutex> lock(redis_mutex_);

    if (!is_connected()) {
        if (!reconnect()) {
            error_count_++;
            return result;
        }
    }

    // Lua脚本：在Redis服务端遍历所有key，取最新score，返回数组
    // 返回格式: [score1, score2, ...] 对应传入的key顺序，无数据返回 -1
    static const char* LUA_SCRIPT =
        "local res = {}\n"
        "for i, key in ipairs(KEYS) do\n"
        "  local r = redis.call('ZREVRANGE', key, 0, 0, 'WITHSCORES')\n"
        "  if r and #r >= 2 then\n"
        "    res[i] = tonumber(r[2])\n"
        "  else\n"
        "    res[i] = -1\n"
        "  end\n"
        "end\n"
        "return res\n";

    // 先尝试 EVALSHA，失败则 EVAL 并缓存SHA
    // 构建key列表
    std::vector<std::string> keys;
    keys.reserve(symbols.size());
    for (const auto& symbol : symbols) {
        keys.push_back("kline:" + exchange + ":" + symbol + ":" + interval);
    }

    redisReply* reply = nullptr;

    // 构建 EVAL 命令参数
    // EVAL script numkeys key1 key2 ...
    std::vector<const char*> argv;
    std::vector<size_t> argvlen;

    if (!lua_batch_ts_sha_.empty()) {
        // 尝试 EVALSHA
        std::string cmd = "EVALSHA";
        argv.push_back(cmd.c_str());
        argvlen.push_back(cmd.size());
        argv.push_back(lua_batch_ts_sha_.c_str());
        argvlen.push_back(lua_batch_ts_sha_.size());
        std::string numkeys = std::to_string(keys.size());
        argv.push_back(numkeys.c_str());
        argvlen.push_back(numkeys.size());
        for (const auto& k : keys) {
            argv.push_back(k.c_str());
            argvlen.push_back(k.size());
        }

        reply = (redisReply*)redisCommandArgv(context_, (int)argv.size(), argv.data(), argvlen.data());

        // 如果NOSCRIPT错误，回退到EVAL
        if (reply && reply->type == REDIS_REPLY_ERROR &&
            std::string(reply->str, reply->len).find("NOSCRIPT") != std::string::npos) {
            freeReplyObject(reply);
            reply = nullptr;
            lua_batch_ts_sha_.clear();
        }
    }

    if (!reply) {
        // 使用 EVAL
        argv.clear();
        argvlen.clear();
        std::string cmd = "EVAL";
        argv.push_back(cmd.c_str());
        argvlen.push_back(cmd.size());
        std::string script(LUA_SCRIPT);
        argv.push_back(script.c_str());
        argvlen.push_back(script.size());
        std::string numkeys = std::to_string(keys.size());
        argv.push_back(numkeys.c_str());
        argvlen.push_back(numkeys.size());
        for (const auto& k : keys) {
            argv.push_back(k.c_str());
            argvlen.push_back(k.size());
        }

        reply = (redisReply*)redisCommandArgv(context_, (int)argv.size(), argv.data(), argvlen.data());

        // 缓存SHA: 用 SCRIPT LOAD
        if (lua_batch_ts_sha_.empty()) {
            redisReply* sha_reply = (redisReply*)redisCommand(context_, "SCRIPT LOAD %s", LUA_SCRIPT);
            if (sha_reply && sha_reply->type == REDIS_REPLY_STRING) {
                lua_batch_ts_sha_ = std::string(sha_reply->str, sha_reply->len);
            }
            if (sha_reply) freeReplyObject(sha_reply);
        }
    }

    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        error_count_++;
        if (reply) freeReplyObject(reply);
        return result;
    }

    query_count_++;

    if (reply->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < reply->elements && i < symbols.size(); i++) {
            int64_t ts = -1;
            if (reply->element[i]->type == REDIS_REPLY_INTEGER) {
                ts = reply->element[i]->integer;
            } else if (reply->element[i]->type == REDIS_REPLY_STRING) {
                ts = std::stoll(std::string(reply->element[i]->str, reply->element[i]->len));
            }
            if (ts > 0) {
                result[symbols[i]] = ts;
            }
        }
    }

    freeReplyObject(reply);
    return result;
}

} // namespace server
} // namespace trading
