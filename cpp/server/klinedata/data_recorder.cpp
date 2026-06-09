/**
 * @file data_recorder.cpp
 * @brief 数据记录器 - 将实盘行情数据存入 Redis
 *
 * 功能：
 * 1. 被动监听 trade-server-main 发布的行情数据（ZMQ SUB）
 * 2. 记录所有通过 ZMQ 通道接收到的 1min K线数据
 * 3. 将 1min K线数据存入 Redis，过期时间 2 个月
 * 4. 聚合 1min K线为 5min, 15min, 30min, 1h
 * 5. 不同周期使用不同的过期时间（1min/5min/15min/30min: 2个月，1h: 6个月）
 *
 * 架构说明：
 * - trade-server-main 通过 WebSocket 订阅交易所的全市场合约
 * - data_recorder 只负责被动接收和记录数据，不发送订阅请求
 *
 * Redis 数据结构：
 * - kline:{exchange}:{symbol}:{interval} -> Sorted Set (K线数据，score=timestamp)
 *
 * 使用方法：
 *   ./data_recorder --redis-host 127.0.0.1 --redis-port 6379
 *
 * 依赖：
 *   - ZeroMQ (libzmq + cppzmq)
 *   - hiredis (Redis C 客户端)
 *   - nlohmann/json
 *
 * @author Sequence Team
 * @date 2025-12
 */

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <mutex>

#include <zmq.hpp>
#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>

using namespace std::chrono;
using json = nlohmann::json;

// ============================================================
// 配置
// ============================================================

namespace Config {
    // ZMQ IPC 地址（与实盘服务器一致）
    const std::string MARKET_DATA_IPC = "ipc:///tmp/seq_md.ipc";

    // Redis 配置
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string redis_password = "";

    // 数据过期时间（秒）
    const int EXPIRE_2_MONTHS = 60 * 24 * 60 * 60;  // 2个月
    const int EXPIRE_4_MONTHS = 120 * 24 * 60 * 60; // 4个月
    const int EXPIRE_6_MONTHS = 180 * 24 * 60 * 60; // 6个月

    // 每个币种/周期保留的最大 K 线数量
    int max_klines_1m = 60 * 24 * 60;      // 2个月的1分钟K线
    int max_klines_5m = 12 * 24 * 60;      // 2个月的5分钟K线
    int max_klines_15m = 4 * 24 * 60;      // 2个月的15分钟K线
    int max_klines_30m = 2 * 24 * 60;      // 2个月的30分钟K线
    int max_klines_1h = 24 * 180;          // 6个月的1小时K线
    int max_klines_4h = 6 * 60;            // 2个月的4小时K线
    int max_klines_8h = 3 * 60;            // 2个月的8小时K线
    int max_klines_1d = 120;               // 120天的日K线
}

// ============================================================
// 全局状态
// ============================================================

std::atomic<bool> g_running{true};

// 统计
std::atomic<uint64_t> g_kline_1m_count{0};
std::atomic<uint64_t> g_kline_5m_count{0};
std::atomic<uint64_t> g_kline_15m_count{0};
std::atomic<uint64_t> g_kline_30m_count{0};
std::atomic<uint64_t> g_kline_1h_count{0};
std::atomic<uint64_t> g_kline_4h_count{0};
std::atomic<uint64_t> g_kline_8h_count{0};
std::atomic<uint64_t> g_kline_1d_count{0};
std::atomic<uint64_t> g_redis_write_count{0};
std::atomic<uint64_t> g_redis_error_count{0};
std::atomic<uint64_t> g_backfill_count{0};   // 冷启动周期中段回填次数(symbol×interval)

// ============================================================
// 信号处理
// ============================================================

void signal_handler(int signum) {
    std::cout << "\n[DataRecorder] 收到信号 " << signum << "，正在停止...\n";
    g_running.store(false);
}

// ============================================================
// K线数据结构
// ============================================================

struct KlineData {
    int64_t timestamp;
    double open;
    double high;
    double low;
    double close;
    double volume;
    double amount;
    double buy_amount;
    int64_t trades;

    KlineData() : timestamp(0), open(0), high(0), low(0), close(0), volume(0), amount(0), buy_amount(0), trades(0) {}

    KlineData(const json& j) {
        timestamp = j.value("timestamp", 0LL);
        open = j.value("open", 0.0);
        high = j.value("high", 0.0);
        low = j.value("low", 0.0);
        close = j.value("close", 0.0);
        volume = j.value("volume", 0.0);
        amount = j.value("amount", 0.0);
        buy_amount = j.value("buy_amount", 0.0);
        trades = j.value("trades", 0LL);
    }

    json to_json(const std::string& exchange, const std::string& symbol, const std::string& interval) const {
        json j = {
            {"type", "kline"},
            {"exchange", exchange},
            {"symbol", symbol},
            {"interval", interval},
            {"timestamp", timestamp},
            {"open", open},
            {"high", high},
            {"low", low},
            {"close", close},
            {"volume", volume}
        };
        if (amount != 0 || buy_amount != 0 || trades != 0) {
            j["amount"] = amount;
            j["buy_amount"] = buy_amount;
            j["trades"] = trades;
        }
        return j;
    }
};

// ============================================================
// K线聚合器
// ============================================================

class KlineAggregator {
public:
    /**
     * @brief 聚合 1min K线到更大周期
     * @param interval_minutes 目标周期（分钟）
     * @param kline_1m 1分钟K线数据
     * @return 是否生成了新的聚合K线
     *
     * 聚合逻辑：收齐 interval_minutes 根1分钟K线后立即输出，无需等待下一个周期的第一根。
     * 例如：8h K线在收到 07:59 的1m K线（第480根）时立即输出，不等 08:00。
     */
    bool aggregate(int interval_minutes, const KlineData& kline_1m, KlineData& output) {
        // 计算当前K线所属的聚合周期起始时间 (统一 UTC 0:00 对齐, 1d K线日界 = UTC 00:00)
        int64_t period_ms = (int64_t)interval_minutes * 60 * 1000;
        int64_t period_start = (kline_1m.timestamp / period_ms) * period_ms;

        auto& state = aggregation_state_[interval_minutes];

        // 去重：跳过已处理过的 timestamp（防止重复K线导致 count 错误）
        if (state.last_1m_ts == kline_1m.timestamp) {
            // 同一根1m K线重复到达，更新OHLCV但不增加count
            if (state.timestamp != 0 && period_start == state.timestamp && state.count > 0) {
                state.kline.high = std::max(state.kline.high, kline_1m.high);
                state.kline.low = std::min(state.kline.low, kline_1m.low);
                state.kline.close = kline_1m.close;
            }
            return false;
        }
        state.last_1m_ts = kline_1m.timestamp;

        // 如果是新周期（上一个周期如果完整，已经在收齐时立即输出了）
        if (state.timestamp != 0 && period_start != state.timestamp) {
            // 上一个周期未完整（冷启动/丢数据），尝试从 Redis 补全
            if (state.count < interval_minutes && redis_query_fn_) {
                int64_t prev_period_end = period_start - 1;
                auto hist = redis_query_fn_(state.timestamp, prev_period_end);
                if ((int)hist.size() >= interval_minutes) {
                    // Redis 数据完整，重建并输出
                    KlineData rebuilt;
                    rebuilt.timestamp = state.timestamp;
                    rebuilt.open = hist.front().open;
                    rebuilt.high = hist.front().high;
                    rebuilt.low = hist.front().low;
                    rebuilt.volume = 0;
                    rebuilt.amount = 0;
                    rebuilt.buy_amount = 0;
                    rebuilt.trades = 0;
                    for (const auto& h : hist) {
                        if (h.high > rebuilt.high) rebuilt.high = h.high;
                        if (h.low < rebuilt.low) rebuilt.low = h.low;
                        rebuilt.volume += h.volume;
                        rebuilt.amount += h.amount;
                        rebuilt.buy_amount += h.buy_amount;
                        rebuilt.trades += h.trades;
                    }
                    rebuilt.close = hist.back().close;
                    output = rebuilt;

                    // 开始新周期
                    state.timestamp = period_start;
                    state.kline.timestamp = period_start;
                    state.kline.open = kline_1m.open;
                    state.kline.high = kline_1m.high;
                    state.kline.low = kline_1m.low;
                    state.kline.close = kline_1m.close;
                    state.kline.volume = kline_1m.volume;
                    state.kline.amount = kline_1m.amount;
                    state.kline.buy_amount = kline_1m.buy_amount;
                    state.kline.trades = kline_1m.trades;
                    state.count = 1;
                    return true;
                }
            }

            // 上一个周期不完整且无法补全，丢弃并开始新周期
            state.timestamp = period_start;
            state.kline.timestamp = period_start;
            state.kline.open = kline_1m.open;
            state.kline.high = kline_1m.high;
            state.kline.low = kline_1m.low;
            state.kline.close = kline_1m.close;
            state.kline.volume = kline_1m.volume;
            state.kline.amount = kline_1m.amount;
            state.kline.buy_amount = kline_1m.buy_amount;
            state.kline.trades = kline_1m.trades;
            state.count = 1;
            return false;
        }

        // 初始化或更新当前周期
        if (state.timestamp == 0) {
            state.timestamp = period_start;
            state.kline.timestamp = period_start;
            state.kline.open = kline_1m.open;
            state.kline.high = kline_1m.high;
            state.kline.low = kline_1m.low;
            state.kline.close = kline_1m.close;
            state.kline.volume = kline_1m.volume;
            state.kline.amount = kline_1m.amount;
            state.kline.buy_amount = kline_1m.buy_amount;
            state.kline.trades = kline_1m.trades;
            state.count = 1;

            // 冷启动且在周期中段(仅长周期): 当场从 Redis 回填本周期已过的 1m, 把回填
            // 成本从"整点那一秒"挪到现在(离边界远)。正常运行时新周期第一根 bar 的
            // 时间戳 == period_start, 此分支不会执行 —— 稳态路径零改动。
            // 仅预热状态, 不改输出语义: 收齐仍按 count>=interval_minutes 判定,
            // 回填不全(真缺口)照旧走周期末的现有兜底补全。
            if (interval_minutes >= 60 && kline_1m.timestamp > period_start
                    && redis_query_fn_) {
                auto hist = redis_query_fn_(period_start, kline_1m.timestamp - 1);
                if (!hist.empty()) {
                    state.kline.open = hist.front().open;   // open = 周期最早一根的 open
                    for (const auto& h : hist) {
                        if (h.high > state.kline.high) state.kline.high = h.high;
                        if (h.low < state.kline.low) state.kline.low = h.low;
                        state.kline.volume += h.volume;
                        state.kline.amount += h.amount;
                        state.kline.buy_amount += h.buy_amount;
                        state.kline.trades += h.trades;
                    }
                    state.count += (int)hist.size();        // close 保持当前根(最新)不变
                    g_backfill_count++;
                }
            }
        } else {
            state.kline.high = std::max(state.kline.high, kline_1m.high);
            state.kline.low = std::min(state.kline.low, kline_1m.low);
            state.kline.close = kline_1m.close;
            state.kline.volume += kline_1m.volume;
            state.kline.amount += kline_1m.amount;
            state.kline.buy_amount += kline_1m.buy_amount;
            state.kline.trades += kline_1m.trades;
            state.count++;
        }

        // 收齐所有1m K线后立即输出，不等下一个周期
        if (state.count >= interval_minutes) {
            output = state.kline;
            // 重置状态，等待下一个周期
            state.timestamp = 0;
            state.count = 0;
            return true;
        }

        // 当前K线已是本周期最后一根（下一分钟就跨周期），但 count 不足
        // 说明中间有丢失（断网等），立刻尝试从 Redis 补全，不等下一个周期
        if (state.count < interval_minutes && redis_query_fn_) {
            int64_t period_end = period_start + period_ms - 1;
            bool is_last_bar = (kline_1m.timestamp + 60000 >= period_start + period_ms);
            if (is_last_bar) {
                auto hist = redis_query_fn_(period_start, period_end);
                if ((int)hist.size() >= interval_minutes) {
                    KlineData rebuilt;
                    rebuilt.timestamp = period_start;
                    rebuilt.open = hist.front().open;
                    rebuilt.high = hist.front().high;
                    rebuilt.low = hist.front().low;
                    rebuilt.volume = 0;
                    rebuilt.amount = 0;
                    rebuilt.buy_amount = 0;
                    rebuilt.trades = 0;
                    for (const auto& h : hist) {
                        if (h.high > rebuilt.high) rebuilt.high = h.high;
                        if (h.low < rebuilt.low) rebuilt.low = h.low;
                        rebuilt.volume += h.volume;
                        rebuilt.amount += h.amount;
                        rebuilt.buy_amount += h.buy_amount;
                        rebuilt.trades += h.trades;
                    }
                    rebuilt.close = hist.back().close;
                    output = rebuilt;
                    // 重置状态
                    state.timestamp = 0;
                    state.count = 0;
                    return true;
                }
            }
        }

        return false;
    }

private:
    struct AggregationState {
        int64_t timestamp = 0;  // 当前聚合周期的起始时间
        KlineData kline;        // 当前聚合的K线数据
        int count = 0;          // 已收集的1分钟K线数量
        int64_t last_1m_ts = 0; // 上一根已处理的1m K线 timestamp（去重用）
    };

    std::map<int, AggregationState> aggregation_state_;  // interval_minutes -> AggregationState

    // Redis 查询回调：给定时间范围，返回1m K线列表
    std::function<std::vector<KlineData>(int64_t start_ms, int64_t end_ms)> redis_query_fn_;

public:
    void set_redis_query_fn(std::function<std::vector<KlineData>(int64_t, int64_t)> fn) {
        redis_query_fn_ = std::move(fn);
    }
};

// ============================================================
// Redis 客户端封装
// ============================================================

class RedisClient {
public:
    RedisClient() : context_(nullptr) {}

    ~RedisClient() {
        disconnect();
    }

    bool connect(const std::string& host, int port, const std::string& password = "") {
        context_ = redisConnect(host.c_str(), port);

        if (context_ == nullptr || context_->err) {
            if (context_) {
                std::cerr << "[Redis] 连接失败: " << context_->errstr << "\n";
                redisFree(context_);
                context_ = nullptr;
            } else {
                std::cerr << "[Redis] 无法分配 context\n";
            }
            return false;
        }

        // 如果有密码，进行认证
        if (!password.empty()) {
            redisReply* reply = (redisReply*)redisCommand(context_, "AUTH %s", password.c_str());
            if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
                std::cerr << "[Redis] 认证失败\n";
                if (reply) freeReplyObject(reply);
                return false;
            }
            freeReplyObject(reply);
        }

        // 测试连接
        redisReply* reply = (redisReply*)redisCommand(context_, "PING");
        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            std::cerr << "[Redis] PING 失败\n";
            if (reply) freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);

        std::cout << "[Redis] 连接成功: " << host << ":" << port << "\n";
        return true;
    }

    void disconnect() {
        if (context_) {
            redisFree(context_);
            context_ = nullptr;
        }
        pending_replies_ = 0;   // 新连接上没有未读回复
    }

    bool is_connected() const {
        return context_ != nullptr && context_->err == 0;
    }

    /**
     * @brief 入队一根 K 线（管线批量写, 不立即往返）
     *
     * Key: kline:{exchange}:{symbol}:{interval} -> Sorted Set (score=timestamp, member=json)
     *
     * PERF: 旧实现每根K线 2 次同步往返(EVAL + pipeline裁剪), 00:00 UTC 全周期同收时
     * 529币×8周期≈4232次写严格串行, 实测把新bar写入拖到 ~12s。现改为:
     *   - queue_kline 只 redisAppendCommand 入 hiredis 输出缓冲, 不读回复;
     *   - flush_batch 一次性发出+读回全部回复(整批≈1次往返);
     *   - 裁剪/EXPIRE 从"每根都做"降频为"该 key 每 64 根做一次"(zset 最多临时超容 63 根, 无害)。
     */
    bool queue_kline(const std::string& exchange, const std::string& symbol,
                     const std::string& interval, const json& kline_data) {
        if (!is_connected()) return false;

        int64_t timestamp = kline_data.value("timestamp", 0LL);
        if (timestamp == 0) return false;

        std::string zset_key = "kline:" + exchange + ":" + symbol + ":" + interval;
        std::string value = kline_data.dump();

        // 去重 upsert: 管线内两条普通命令(替代旧 Lua EVAL)。
        // EVAL 每笔进 Lua VM ~50-150µs, 整点 7000+ 笔突发、又撞 bgsave-COW 时被放大成
        // 秒级排空(2026-06-07 probe 实测 3.2s 的主因)。普通命令 ~5-10µs/条。
        // 非原子窗口仅在"重写已存在 score"时出现(正常新 bar 的 ZREMRANGEBYSCORE 删不到
        // 任何东西), 两条又在同一 pipeline 内间隙 µs 级, 读侧 0.5s 轮询不可感知。
        redisAppendCommand(context_, "ZREMRANGEBYSCORE %s %lld %lld",
            zset_key.c_str(), (long long)timestamp, (long long)timestamp);
        redisAppendCommand(context_, "ZADD %s %lld %s",
            zset_key.c_str(), (long long)timestamp, value.c_str());
        pending_replies_ += 2;

        // 维护性命令降频: 每 key 第 1 根及之后每 64 根做一次裁剪+续期
        uint32_t seq = key_write_seq_[zset_key]++;
        if (seq % 64 == 0) {
            int expire_seconds, max_count;
            interval_policy(interval, expire_seconds, max_count);
            redisAppendCommand(context_, "ZREMRANGEBYRANK %s 0 -%d",
                zset_key.c_str(), max_count + 1);
            redisAppendCommand(context_, "EXPIRE %s %d", zset_key.c_str(), expire_seconds);
            pending_replies_ += 2;
        }

        g_redis_write_count++;
        return true;
    }

    /**
     * @brief 把已入队的命令一次性落盘并读回全部回复（整批≈1次往返）
     */
    void flush_batch() {
        while (pending_replies_ > 0) {
            redisReply* r = nullptr;
            if (redisGetReply(context_, (void**)&r) != REDIS_OK) {
                // 连接级错误: 剩余回复不可读, 计数清零, 交给主循环的重连逻辑
                g_redis_error_count += pending_replies_;
                pending_replies_ = 0;
                std::cerr << "[Redis] flush_batch 连接错误: "
                          << (context_ ? context_->errstr : "(null)") << "\n";
                return;
            }
            if (r) {
                if (r->type == REDIS_REPLY_ERROR) {
                    g_redis_error_count++;
                    std::cerr << "[Redis] 写入错误: " << r->str << "\n";
                }
                freeReplyObject(r);
            }
            pending_replies_--;
        }
    }

    int pending() const { return pending_replies_; }

    /// 兼容旧接口: 入队 + 立即落盘
    bool store_kline(const std::string& exchange, const std::string& symbol,
                     const std::string& interval, const json& kline_data) {
        bool ok = queue_kline(exchange, symbol, interval, kline_data);
        flush_batch();
        return ok;
    }

    /// 各周期的过期时间与最大保留数量
    static void interval_policy(const std::string& interval, int& expire_seconds, int& max_count) {
        if (interval == "1m") {
            expire_seconds = Config::EXPIRE_2_MONTHS;
            max_count = Config::max_klines_1m;
        } else if (interval == "5m") {
            expire_seconds = Config::EXPIRE_2_MONTHS;
            max_count = Config::max_klines_5m;
        } else if (interval == "15m") {
            expire_seconds = Config::EXPIRE_2_MONTHS;
            max_count = Config::max_klines_15m;
        } else if (interval == "30m") {
            expire_seconds = Config::EXPIRE_2_MONTHS;
            max_count = Config::max_klines_30m;
        } else if (interval == "1h") {
            expire_seconds = Config::EXPIRE_6_MONTHS;
            max_count = Config::max_klines_1h;
        } else if (interval == "4h") {
            expire_seconds = Config::EXPIRE_2_MONTHS;
            max_count = Config::max_klines_4h;
        } else if (interval == "8h") {
            expire_seconds = Config::EXPIRE_2_MONTHS;
            max_count = Config::max_klines_8h;
        } else if (interval == "1d") {
            expire_seconds = Config::EXPIRE_4_MONTHS;
            max_count = Config::max_klines_1d;
        } else {
            expire_seconds = Config::EXPIRE_2_MONTHS;
            max_count = 10000;
        }
    }

    /**
     * @brief 检查 Redis 连接状态
     */
    bool ping() {
        if (!is_connected()) return false;
        flush_batch();   // 管线途中不可插同步命令(回复会错配), 先排空

        redisReply* reply = (redisReply*)redisCommand(context_, "PING");
        bool ok = (reply != nullptr && reply->type == REDIS_REPLY_STATUS);
        if (reply) freeReplyObject(reply);
        return ok;
    }

    /**
     * @brief 查询指定时间范围内的1m K线（用于聚合器冷启动补全）
     */
    std::vector<KlineData> query_1m_klines(const std::string& exchange, const std::string& symbol,
                                            int64_t start_ms, int64_t end_ms) {
        std::vector<KlineData> result;
        if (!is_connected()) return result;
        flush_batch();   // 同上: 先排空 pending 回复再做同步往返

        std::string key = "kline:" + exchange + ":" + symbol + ":1m";
        redisReply* reply = (redisReply*)redisCommand(
            context_, "ZRANGEBYSCORE %s %lld %lld",
            key.c_str(), (long long)start_ms, (long long)end_ms
        );

        if (reply && reply->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < reply->elements; i++) {
                try {
                    json j = json::parse(std::string(reply->element[i]->str, reply->element[i]->len));
                    result.emplace_back(j);
                } catch (...) {}
            }
        }
        if (reply) freeReplyObject(reply);
        return result;
    }

private:
    redisContext* context_;
    int pending_replies_ = 0;                       // 已 append 未读回的命令数
    std::map<std::string, uint32_t> key_write_seq_; // 每 key 写入序号(维护命令降频用)
};

// ============================================================
// 数据记录器
// ============================================================

class DataRecorder {
public:
    DataRecorder() : zmq_context_(1) {}

    ~DataRecorder() {
        stop();
    }

    bool start() {
        // 连接 Redis
        if (!redis_.connect(Config::redis_host, Config::redis_port, Config::redis_password)) {
            std::cerr << "[DataRecorder] Redis 连接失败\n";
            return false;
        }

        // 创建 ZMQ socket - 只需要 SUB socket 被动接收行情数据
        try {
            // 行情订阅 (SUB) - 被动接收 trade-server-main 发布的所有行情数据
            market_sub_ = std::make_unique<zmq::socket_t>(zmq_context_, zmq::socket_type::sub);
            market_sub_->connect(Config::MARKET_DATA_IPC);
            // 只订阅 kline 主题(topic 格式 {exchange}.kline.{symbol}.{interval}):
            // 本进程只消费K线; 全订阅("")会把 ticker/markPrice 等也灌进来逐条丢弃,
            // 整点洪峰时挤占唯一消费线程
            market_sub_->set(zmq::sockopt::subscribe, "binance.kline.");
            market_sub_->set(zmq::sockopt::subscribe, "okx.kline.");
            market_sub_->set(zmq::sockopt::rcvtimeo, 100);  // 100ms 超时
            std::cout << "[ZMQ] 行情通道: " << Config::MARKET_DATA_IPC << "\n";
            std::cout << "[ZMQ] 订阅过滤: binance.kline.* / okx.kline.*\n";

        } catch (const zmq::error_t& e) {
            std::cerr << "[ZMQ] 连接失败: " << e.what() << "\n";
            return false;
        }

        std::cout << "[DataRecorder] 初始化完成\n";
        return true;
    }

    void stop() {
        // 关闭 ZMQ socket
        if (market_sub_) market_sub_->close();

        // 断开 Redis
        redis_.disconnect();
    }

    /**
     * @brief 主循环：接收数据并存入 Redis
     */
    void run() {
        std::cout << "[DataRecorder] 开始运行...\n";
        std::cout << "  - 被动监听 trade-server-main 发布的所有K线数据\n";
        std::cout << "  - 1min/5min/15min/30min/4h/8h 过期时间: 2 个月\n";
        std::cout << "  - 1h 过期时间: 6 个月\n";
        std::cout << "  - 1d 过期时间: 4 个月（保留 120 天）\n";
        std::cout << "  - 按 Ctrl+C 停止\n\n";

        auto last_status_time = steady_clock::now();

        while (g_running.load()) {
            // —— 贪婪排空(PERF): 一口气收到队列空为止, 期间写入只入队不往返;
            //    旧版"每收 1 条 sleep 100µs + 每根K线 2 次同步往返", 00:00 UTC
            //    全周期同收(529币×8周期)时串行排队 ~12s。——
            int processed = 0;
            while (g_running.load()) {
                zmq::message_t msg;
                zmq::recv_result_t result;
                try {
                    result = market_sub_->recv(msg, zmq::recv_flags::dontwait);
                } catch (const zmq::error_t& e) {
                    if (e.num() != EAGAIN) {
                        std::cerr << "[ZMQ] 接收错误: " << e.what() << "\n";
                    }
                    break;
                }
                if (!result.has_value()) break;   // 队列已空

                std::string data_str(static_cast<char*>(msg.data()), msg.size());
                // 消息格式：topic|json_data, 跳过 topic 只解析 JSON
                size_t separator_pos = data_str.find('|');
                if (separator_pos != std::string::npos) {
                    data_str = data_str.substr(separator_pos + 1);
                }
                try {
                    json data = json::parse(data_str);
                    process_market_data(data);
                } catch (const json::parse_error& e) {
                    std::cerr << "[JSON] 解析错误: " << e.what() << "\n";
                    std::cerr << "[JSON] 原始数据: " << data_str.substr(0, 100) << "...\n";
                }
                processed++;

                // 洪峰保护: 积攒过多先落一批(整批一次往返), 控制 hiredis 缓冲
                if (redis_.pending() >= 512) {
                    redis_.flush_batch();
                }
            }
            // 本轮排空结束: 把入队的写一次性落盘
            redis_.flush_batch();

            // 每 10 秒打印状态 + Redis 断连自愈(旧版断连后会静默罢工)
            auto now = steady_clock::now();
            if (duration_cast<seconds>(now - last_status_time).count() >= 10) {
                last_status_time = now;
                print_status();
                if (!redis_.is_connected()) {
                    std::cerr << "[Redis] 连接丢失, 尝试重连...\n";
                    redis_.disconnect();
                    redis_.connect(Config::redis_host, Config::redis_port,
                                   Config::redis_password);
                }
            }

            // 仅空闲时休眠(旧版每条消息后都 sleep 100µs)
            if (processed == 0) {
                std::this_thread::sleep_for(milliseconds(1));
            }
        }
        redis_.flush_batch();   // 退出前落盘
    }

private:
    void process_market_data(const json& data) {
        std::string type = data.value("type", "");

        if (type == "kline") {
            std::string exchange = data.value("exchange", "okx");
            std::string symbol = data.value("symbol", "");
            std::string interval = data.value("interval", "");

            if (symbol.empty() || interval != "1m") {
                return;  // 只处理 1min K线
            }

            // 解析 1min K线数据
            KlineData kline_1m(data);

            // 存储 1min K线
            redis_.queue_kline(exchange, symbol, "1m", data);
            g_kline_1m_count++;

            // 获取该币种的聚合器，首次创建时注入 Redis 查询回调
            std::string key = exchange + ":" + symbol;
            std::lock_guard<std::mutex> lock(aggregator_mutex_);
            if (aggregators_.find(key) == aggregators_.end()) {
                aggregators_[key].set_redis_query_fn(
                    [this, exchange, symbol](int64_t start_ms, int64_t end_ms) {
                        return redis_.query_1m_klines(exchange, symbol, start_ms, end_ms);
                    }
                );
            }
            auto& aggregator = aggregators_[key];

            // 聚合到 5min
            KlineData kline_5m;
            if (aggregator.aggregate(5, kline_1m, kline_5m)) {
                json j = kline_5m.to_json(exchange, symbol, "5m");
                redis_.queue_kline(exchange, symbol, "5m", j);
                g_kline_5m_count++;
            }

            // 聚合到 15min
            KlineData kline_15m;
            if (aggregator.aggregate(15, kline_1m, kline_15m)) {
                json j = kline_15m.to_json(exchange, symbol, "15m");
                redis_.queue_kline(exchange, symbol, "15m", j);
                g_kline_15m_count++;
            }

            // 聚合到 30min
            KlineData kline_30m;
            if (aggregator.aggregate(30, kline_1m, kline_30m)) {
                json j = kline_30m.to_json(exchange, symbol, "30m");
                redis_.queue_kline(exchange, symbol, "30m", j);
                g_kline_30m_count++;
            }

            // 聚合到 1h
            KlineData kline_1h;
            if (aggregator.aggregate(60, kline_1m, kline_1h)) {
                json j = kline_1h.to_json(exchange, symbol, "1h");
                redis_.queue_kline(exchange, symbol, "1h", j);
                g_kline_1h_count++;
            }

            // 聚合到 4h
            KlineData kline_4h;
            if (aggregator.aggregate(240, kline_1m, kline_4h)) {
                json j = kline_4h.to_json(exchange, symbol, "4h");
                redis_.queue_kline(exchange, symbol, "4h", j);
                g_kline_4h_count++;
            }

            // 聚合到 8h
            KlineData kline_8h;
            if (aggregator.aggregate(480, kline_1m, kline_8h)) {
                json j = kline_8h.to_json(exchange, symbol, "8h");
                redis_.queue_kline(exchange, symbol, "8h", j);
                g_kline_8h_count++;
            }

            // 聚合到 1d (1440 分钟)
            KlineData kline_1d;
            if (aggregator.aggregate(1440, kline_1m, kline_1d)) {
                json j = kline_1d.to_json(exchange, symbol, "1d");
                redis_.queue_kline(exchange, symbol, "1d", j);
                g_kline_1d_count++;
            }
        }
    }

    void print_status() {
        auto now = system_clock::now();
        auto time_t = system_clock::to_time_t(now);
        std::tm* tm = std::localtime(&time_t);

        std::cout << "[" << std::put_time(tm, "%H:%M:%S") << "] "
                  << "1m: " << g_kline_1m_count.load()
                  << " | 5m: " << g_kline_5m_count.load()
                  << " | 15m: " << g_kline_15m_count.load()
                  << " | 30m: " << g_kline_30m_count.load()
                  << " | 1h: " << g_kline_1h_count.load()
                  << " | 4h: " << g_kline_4h_count.load()
                  << " | 8h: " << g_kline_8h_count.load()
                  << " | 1d: " << g_kline_1d_count.load()
                  << " | Redis写入: " << g_redis_write_count.load()
                  << " | 错误: " << g_redis_error_count.load()
                  << " | 冷启动回填: " << g_backfill_count.load()
                  << "\n";
    }

private:
    zmq::context_t zmq_context_;
    std::unique_ptr<zmq::socket_t> market_sub_;
    RedisClient redis_;

    // K线聚合器（每个 exchange:symbol 一个）
    std::map<std::string, KlineAggregator> aggregators_;
    std::mutex aggregator_mutex_;
};

// ============================================================
// 命令行参数解析
// ============================================================

void print_usage(const char* prog) {
    std::cout << "用法: " << prog << " [选项]\n"
              << "\n"
              << "选项:\n"
              << "  --redis-host HOST    Redis 主机 (默认: 127.0.0.1)\n"
              << "  --redis-port PORT    Redis 端口 (默认: 6379)\n"
              << "  --redis-pass PASS    Redis 密码 (默认: 无)\n"
              << "  -h, --help           显示帮助\n"
              << "\n"
              << "示例:\n"
              << "  " << prog << " --redis-host 192.168.1.100 --redis-port 6379\n";
}

void parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            exit(0);
        }
        else if (arg == "--redis-host" && i + 1 < argc) {
            Config::redis_host = argv[++i];
        }
        else if (arg == "--redis-port" && i + 1 < argc) {
            Config::redis_port = std::stoi(argv[++i]);
        }
        else if (arg == "--redis-pass" && i + 1 < argc) {
            Config::redis_password = argv[++i];
        }
    }
}

// ============================================================
// 主函数
// ============================================================

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "    Sequence 数据记录器 (DataRecorder)\n";
    std::cout << "    实盘 1min K线 -> Redis (聚合多周期)\n";
    std::cout << "========================================\n\n";

    // 解析命令行参数
    parse_args(argc, argv);

    // 打印配置
    std::cout << "[配置]\n";
    std::cout << "  Redis: " << Config::redis_host << ":" << Config::redis_port << "\n";
    std::cout << "  模式: 被动监听 trade-server-main 发布的所有K线数据\n";
    std::cout << "  聚合周期: 1min -> 5min, 15min, 30min, 1h, 4h, 8h\n";
    std::cout << "  过期时间: 1m/5m/15m/30m/4h/8h = 2个月, 1h = 6个月\n\n";

    // 注册信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 创建并启动数据记录器
    DataRecorder recorder;

    if (!recorder.start()) {
        std::cerr << "[错误] 启动失败\n";
        return 1;
    }

    // 主循环 - 被动接收并记录所有K线数据
    recorder.run();

    // 停止
    recorder.stop();

    // 打印统计
    std::cout << "\n========================================\n";
    std::cout << "  数据记录器已停止\n";
    std::cout << "  1min K线: " << g_kline_1m_count.load() << " 条\n";
    std::cout << "  5min K线: " << g_kline_5m_count.load() << " 条\n";
    std::cout << "  15min K线: " << g_kline_15m_count.load() << " 条\n";
    std::cout << "  30min K线: " << g_kline_30m_count.load() << " 条\n";
    std::cout << "  1h K线: " << g_kline_1h_count.load() << " 条\n";
    std::cout << "  4h K线: " << g_kline_4h_count.load() << " 条\n";
    std::cout << "  8h K线: " << g_kline_8h_count.load() << " 条\n";
    std::cout << "  Redis 写入: " << g_redis_write_count.load() << " 次\n";
    std::cout << "  Redis 错误: " << g_redis_error_count.load() << " 次\n";
    std::cout << "========================================\n";

    return 0;
}
