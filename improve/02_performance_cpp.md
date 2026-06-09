# 性能优化清单 - C++ 后端

> 本文档供 Claude 逐项执行优化。每个条目包含：问题描述、涉及文件和行号、具体修改方法。

---

## PERF-C1: Redis 写入未使用 Pipeline（3倍延迟浪费）

**文件**: `cpp/server/managers/redis_recorder.cpp`
**行号**: 228-260

**问题**: 每条 K 线写入执行 3 次独立 Redis 命令（ZADD + ZREMRANGEBYRANK + EXPIRE），每次都是一个完整的网络往返。

**当前代码**:
```cpp
// 第1次往返
redisReply* reply = (redisReply*)redisCommand(context_, "ZADD %s %lld %s", ...);
freeReplyObject(reply);
// 第2次往返
reply = (redisReply*)redisCommand(context_, "ZREMRANGEBYRANK %s 0 -%d", ...);
freeReplyObject(reply);
// 第3次往返
reply = (redisReply*)redisCommand(context_, "EXPIRE %s %d", ...);
freeReplyObject(reply);
```

**修改方法**: 使用 `redisAppendCommand` + `redisGetReply` 管道化：
```cpp
redisAppendCommand(context_, "ZADD %s %lld %s", key.c_str(), (long long)timestamp, value.c_str());
redisAppendCommand(context_, "ZREMRANGEBYRANK %s 0 -%d", key.c_str(), max_count + 1);
redisAppendCommand(context_, "EXPIRE %s %d", key.c_str(), expire_seconds);
for (int i = 0; i < 3; i++) {
    redisReply* reply = nullptr;
    redisGetReply(context_, (void**)&reply);
    if (reply) freeReplyObject(reply);
}
```

**预期收益**: 单条 K 线写入延迟从 0.3-1.5ms 降到 0.1-0.5ms。

---

## PERF-C2: C++ data_recorder 用普通 ZADD 写聚合 K 线（产生重复）

**文件**: `cpp/server/klinedata/data_recorder.cpp`
**行号**: 378-381

**问题**: C++ `store_kline` 用普通 `ZADD` 写入聚合后的 K 线。Redis Sorted Set 唯一性基于 member（JSON 字符串）而非 score（时间戳），当同一时间戳的 JSON 内容有细微差异时会产生重复条目。这既是功能 bug 也是性能问题（重复数据占用内存和查询时间）。

**当前代码**:
```cpp
redisReply* reply = (redisReply*)redisCommand(
    context_, "ZADD %s %lld %s",
    zset_key.c_str(), (long long)timestamp, value.c_str()
);
```

**修改方法**: 改为 Lua 原子 upsert：
```cpp
const char* lua_upsert =
    "local existing = redis.call('ZRANGEBYSCORE', KEYS[1], ARGV[1], ARGV[1]) "
    "for _, m in ipairs(existing) do redis.call('ZREM', KEYS[1], m) end "
    "redis.call('ZADD', KEYS[1], ARGV[1], ARGV[2]) "
    "return 1";

redisReply* reply = (redisReply*)redisCommand(
    context_, "EVAL %s 1 %s %lld %s",
    lua_upsert, zset_key.c_str(), (long long)timestamp, value.c_str()
);
```

注意：建议将 Lua 脚本用 `SCRIPT LOAD` 预加载，后续用 `EVALSHA` 调用以减少脚本传输开销。

---

## PERF-C3: KEYS 命令阻塞 Redis

**文件**: `cpp/server/managers/redis_data_provider.cpp`
**行号**: 374-383

**问题**: `get_available_symbols()` 使用 `KEYS` 命令扫描全库，这是 O(N) 阻塞操作。

**当前代码**:
```cpp
redisReply* reply = (redisReply*)redisCommand(context_, "KEYS %s", pattern.c_str());
```

**修改方法**: 替换为非阻塞的 `SCAN` 迭代器：
```cpp
std::vector<std::string> result;
long cursor = 0;
do {
    redisReply* reply = (redisReply*)redisCommand(
        context_, "SCAN %ld MATCH %s COUNT 200", cursor, pattern.c_str());
    if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements == 2) {
        cursor = std::stoll(reply->element[0]->str);
        for (size_t i = 0; i < reply->element[1]->elements; i++) {
            // 解析 key...
        }
    }
    if (reply) freeReplyObject(reply);
} while (cursor != 0);
```

---

## PERF-C4: ZMQ 轮询线程固定 sleep 增加延迟

**文件**: `cpp/server/trading_server_main.cpp`
**行号**: 282-288（订单线程）、301-304（查询线程）、316-319（订阅线程）

**问题**: 所有轮询线程在处理完队列后无条件 sleep，即使有新消息立即到达也要等 sleep 结束。

**修改方法**: 仅在空闲时 sleep：
```cpp
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
```

对查询线程和订阅线程做同样修改。

---

## PERF-C5: Aggregators Map 无限增长（内存泄漏）

**文件**: `cpp/server/klinedata/data_recorder.cpp`
**行号**: 609-618, 694

**问题**: `aggregators_` map 为每个 `exchange:symbol` 创建聚合器，从不清理。已下架币种的聚合器永远留在内存中。

**修改方法**: 给每个聚合器记录最后活跃时间，定期清理超过 24h 未活跃的条目：
```cpp
struct AggregatorEntry {
    KlineAggregator aggregator;
    int64_t last_active_ms;
};
std::map<std::string, AggregatorEntry> aggregators_;

// process_kline 中更新:
aggregators_[key].last_active_ms = current_timestamp_ms();

// 定期清理（每小时一次）:
void cleanup_stale_aggregators() {
    auto now = current_timestamp_ms();
    std::lock_guard<std::mutex> lock(aggregator_mutex_);
    for (auto it = aggregators_.begin(); it != aggregators_.end();) {
        if (now - it->second.last_active_ms > 24 * 3600 * 1000)
            it = aggregators_.erase(it);
        else
            ++it;
    }
}
```

---

## PERF-C6: Redis Mutex 竞争

**文件**: `cpp/server/managers/redis_recorder.cpp`
**行号**: 144, 192, 275, 312

**问题**: 所有 Redis 操作共用一个 `redis_mutex_`，锁持有期间包含网络 I/O。高并发时所有线程串行等待。

**修改方法**:
1. 将数据准备（JSON dump）移到锁外
2. 锁内只执行 pipeline 化的 Redis 命令

```cpp
void record_kline(const nlohmann::json& data, ...) {
    // 锁外准备数据
    std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;
    std::string value = data.dump();

    // 最小化锁范围
    {
        std::lock_guard<std::mutex> lock(redis_mutex_);
        if (!is_connected() && !reconnect()) { error_count_++; return; }
        redisAppendCommand(context_, "ZADD %s %lld %s", key.c_str(), (long long)timestamp, value.c_str());
        redisAppendCommand(context_, "ZREMRANGEBYRANK %s 0 -%d", key.c_str(), max_count + 1);
        redisAppendCommand(context_, "EXPIRE %s %d", key.c_str(), expire_seconds);
        for (int i = 0; i < 3; i++) {
            redisReply* r = nullptr;
            redisGetReply(context_, (void**)&r);
            if (r) freeReplyObject(r);
        }
    }
}
```

---

## 优先级排序

| 编号 | 预期收益 | 改动复杂度 | 建议优先级 |
|------|---------|-----------|-----------|
| PERF-C1 | 3x 写入延迟降低 | 低 | 第一优先 |
| PERF-C2 | 消除数据重复 + 性能 | 低 | 第一优先 |
| PERF-C3 | 消除 Redis 阻塞 | 中 | 第一优先 |
| PERF-C4 | 订单延迟降低 50-100us | 低 | 第二优先 |
| PERF-C6 | 高并发吞吐提升 | 中 | 第二优先 |
| PERF-C5 | 消除内存泄漏 | 中 | 第三优先 |
