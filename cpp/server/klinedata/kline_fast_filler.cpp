#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <nlohmann/json.hpp>
#include "historical_data_fetcher.h"
#include "kline_utils.h"
#include "gap_detector.h"
#include <hiredis/hiredis.h>

using json = nlohmann::json;
using trading::kline_utils::SymbolInfo;
using trading::kline_utils::is_usdt_contract;
using trading::gap_detector::Gap;

// ==================== 数据结构 ====================

struct ChunkInfo {
    int64_t start_ts;
    int64_t end_ts;
    int expected;
    int actual;
};

struct Options {
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::vector<std::string> symbols;
    std::string exchange_filter;
    bool dry_run = false;
    bool verbose = false;
};

// ==================== 阶段1：粗扫描 (Pipelined ZCOUNT) ====================

std::vector<ChunkInfo> pipeline_zcount_scan(
    redisContext* ctx,
    const std::string& key,
    int64_t first_ts,
    int64_t last_ts,
    int64_t interval_ms
) {
    // 按1小时分块（对1m来说每块60条）
    const int64_t chunk_ms = 3600 * 1000LL; // 1小时
    int64_t chunk_start = trading::kline_utils::align_timestamp(first_ts, chunk_ms);

    // 收集所有块信息
    struct PendingChunk {
        int64_t start;
        int64_t end;
        int expected;
    };
    std::vector<PendingChunk> pending;

    for (int64_t cs = chunk_start; cs <= last_ts; cs += chunk_ms) {
        int64_t ce = cs + chunk_ms - interval_ms;

        // 裁剪到实际数据范围
        int64_t effective_start = std::max(cs, first_ts);
        int64_t effective_end = std::min(ce, last_ts);

        if (effective_start > effective_end) continue;

        int expected = static_cast<int>((effective_end - effective_start) / interval_ms) + 1;
        pending.push_back({effective_start, effective_end, expected});

        // Pipeline发送ZCOUNT
        redisAppendCommand(ctx, "ZCOUNT %s %lld %lld",
            key.c_str(), (long long)effective_start, (long long)effective_end);
    }

    // 收集响应
    std::vector<ChunkInfo> missing_chunks;
    for (auto& p : pending) {
        redisReply* reply = nullptr;
        if (redisGetReply(ctx, (void**)&reply) == REDIS_OK && reply) {
            int actual = (reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : 0;
            if (actual < p.expected) {
                missing_chunks.push_back({p.start, p.end, p.expected, actual});
            }
            freeReplyObject(reply);
        }
    }

    return missing_chunks;
}

// ==================== 阶段2：精扫描 ====================

std::vector<int64_t> fine_scan_gaps(
    redisContext* ctx,
    const std::string& key,
    const ChunkInfo& chunk,
    int64_t interval_ms
) {
    // ZRANGEBYSCORE key chunk_start chunk_end WITHSCORES
    redisReply* reply = (redisReply*)redisCommand(ctx,
        "ZRANGEBYSCORE %s %lld %lld WITHSCORES",
        key.c_str(), (long long)chunk.start_ts, (long long)chunk.end_ts);

    std::set<int64_t> existing_ts;
    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        // WITHSCORES: member, score, member, score, ...
        for (size_t i = 1; i < reply->elements; i += 2) {
            int64_t ts = std::stoll(reply->element[i]->str);
            existing_ts.insert(ts);
        }
        freeReplyObject(reply);
    } else {
        if (reply) freeReplyObject(reply);
    }

    // 逐个比对找出缺失
    std::vector<int64_t> missing;
    for (int64_t ts = chunk.start_ts; ts <= chunk.end_ts; ts += interval_ms) {
        if (existing_ts.find(ts) == existing_ts.end()) {
            missing.push_back(ts);
        }
    }

    return missing;
}

// ==================== 阶段2.5：合并缺失区间 ====================

std::vector<Gap> merge_gaps(const std::vector<int64_t>& missing_timestamps, int64_t interval_ms) {
    std::vector<Gap> gaps;
    if (missing_timestamps.empty()) return gaps;

    Gap current{missing_timestamps[0], missing_timestamps[0]};

    for (size_t i = 1; i < missing_timestamps.size(); i++) {
        if (missing_timestamps[i] - current.end_ts == interval_ms) {
            current.end_ts = missing_timestamps[i];
        } else {
            gaps.push_back(current);
            current = {missing_timestamps[i], missing_timestamps[i]};
        }
    }
    gaps.push_back(current);

    return gaps;
}

// ==================== 阶段3：定向补全 ====================

int fetch_and_fill(
    redisContext* ctx,
    trading::historical_fetcher::HistoricalDataFetcher* fetcher,
    const std::string& exchange,
    const std::string& symbol,
    const std::string& interval,
    const std::vector<Gap>& gaps
) {
    int total_written = 0;
    std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;

    for (const auto& gap : gaps) {
        auto klines = fetcher->fetch_history(symbol, interval, gap.start_ts, gap.end_ts);
        if (klines.empty()) continue;

        // Pipeline ZADD
        for (const auto& kline : klines) {
            json kline_json = {
                {"type", "kline"},
                {"exchange", exchange},
                {"symbol", symbol},
                {"interval", interval},
                {"timestamp", kline.timestamp},
                {"open", kline.open},
                {"high", kline.high},
                {"low", kline.low},
                {"close", kline.close},
                {"volume", kline.volume}
            };
            std::string value = kline_json.dump();
            redisAppendCommand(ctx, "ZADD %s %lld %s",
                key.c_str(), (long long)kline.timestamp, value.c_str());
        }

        // 收集响应
        int written = 0;
        for (size_t i = 0; i < klines.size(); i++) {
            redisReply* reply = nullptr;
            if (redisGetReply(ctx, (void**)&reply) == REDIS_OK && reply) {
                if (reply->type != REDIS_REPLY_ERROR) written++;
                freeReplyObject(reply);
            }
        }

        total_written += written;
    }

    // 设置过期时间
    if (total_written > 0) {
        int expire_seconds = (interval == "1h") ? (180 * 24 * 3600) : (60 * 24 * 3600);
        redisReply* reply = (redisReply*)redisCommand(ctx, "EXPIRE %s %d", key.c_str(), expire_seconds);
        if (reply) freeReplyObject(reply);
    }

    return total_written;
}

// ==================== Redis SCAN 发现币种 ====================

std::vector<std::string> scan_keys(redisContext* ctx, const std::string& pattern) {
    std::vector<std::string> keys;
    unsigned long long cursor = 0;

    do {
        redisReply* reply = (redisReply*)redisCommand(ctx,
            "SCAN %llu MATCH %s COUNT 1000", cursor, pattern.c_str());

        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply) freeReplyObject(reply);
            break;
        }

        cursor = strtoull(reply->element[0]->str, nullptr, 10);

        if (reply->element[1]->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < reply->element[1]->elements; i++) {
                keys.push_back(reply->element[1]->element[i]->str);
            }
        }

        freeReplyObject(reply);
    } while (cursor != 0);

    return keys;
}

// ==================== 获取时间范围 ====================

bool get_ts_range(redisContext* ctx, const std::string& key, int64_t& first_ts, int64_t& last_ts) {
    // 获取第一个元素的score
    redisReply* r1 = (redisReply*)redisCommand(ctx,
        "ZRANGEBYSCORE %s -inf +inf WITHSCORES LIMIT 0 1", key.c_str());
    if (!r1 || r1->type != REDIS_REPLY_ARRAY || r1->elements < 2) {
        if (r1) freeReplyObject(r1);
        return false;
    }
    first_ts = std::stoll(r1->element[1]->str);
    freeReplyObject(r1);

    // 获取最后一个元素的score
    redisReply* r2 = (redisReply*)redisCommand(ctx,
        "ZREVRANGEBYSCORE %s +inf -inf WITHSCORES LIMIT 0 1", key.c_str());
    if (!r2 || r2->type != REDIS_REPLY_ARRAY || r2->elements < 2) {
        if (r2) freeReplyObject(r2);
        return false;
    }
    last_ts = std::stoll(r2->element[1]->str);
    freeReplyObject(r2);

    return true;
}

// ==================== 命令行解析 ====================

Options parse_args(int argc, char* argv[]) {
    Options opts;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--symbol" && i + 1 < argc) {
            opts.symbols.push_back(argv[++i]);
        } else if (arg == "--exchange" && i + 1 < argc) {
            opts.exchange_filter = argv[++i];
        } else if (arg == "--redis-host" && i + 1 < argc) {
            opts.redis_host = argv[++i];
        } else if (arg == "--redis-port" && i + 1 < argc) {
            opts.redis_port = std::stoi(argv[++i]);
        } else if (arg == "--dry-run") {
            opts.dry_run = true;
        } else if (arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "用法: kline_fast_filler [选项]\n"
                      << "  --symbol BTCUSDT     只处理指定币种（可多次指定）\n"
                      << "  --exchange binance   只处理指定交易所\n"
                      << "  --redis-host HOST    Redis地址 (默认: 127.0.0.1)\n"
                      << "  --redis-port PORT    Redis端口 (默认: 6379)\n"
                      << "  --dry-run            只检测不补全\n"
                      << "  --verbose            显示详细输出\n"
                      << "  --help               显示帮助\n";
            exit(0);
        }
    }

    return opts;
}

// ==================== 主程序 ====================

int main(int argc, char* argv[]) {
    auto t_start = std::chrono::steady_clock::now();

    std::cout << "╔════════════════════════════════════════════════════════════╗\n"
              << "║        K线快速缺失检测与补全工具 (kline_fast_filler)       ║\n"
              << "╚════════════════════════════════════════════════════════════╝\n\n";

    Options opts = parse_args(argc, argv);

    std::cout << "[配置]\n"
              << "  Redis: " << opts.redis_host << ":" << opts.redis_port << "\n"
              << "  交易所过滤: " << (opts.exchange_filter.empty() ? "全部" : opts.exchange_filter) << "\n"
              << "  币种过滤: " << (opts.symbols.empty() ? "全部" : std::to_string(opts.symbols.size()) + " 个") << "\n"
              << "  模式: " << (opts.dry_run ? "仅检测 (dry-run)" : "检测+补全") << "\n\n";

    // 连接Redis
    redisContext* ctx = redisConnect(opts.redis_host.c_str(), opts.redis_port);
    if (!ctx || ctx->err) {
        std::cerr << "[错误] Redis连接失败";
        if (ctx) {
            std::cerr << ": " << ctx->errstr;
            redisFree(ctx);
        }
        std::cerr << std::endl;
        return 1;
    }
    std::cout << "[Redis] 已连接 " << opts.redis_host << ":" << opts.redis_port << "\n\n";

    // 使用SCAN发现币种
    std::cout << "[发现] 扫描Redis中的K线数据...\n";
    auto kline_keys = scan_keys(ctx, "kline:*:1m");

    if (kline_keys.empty()) {
        std::cout << "[发现] 未找到任何1m K线数据\n";
        redisFree(ctx);
        return 0;
    }

    // 解析并过滤
    std::vector<SymbolInfo> all_symbols;
    std::set<std::string> symbol_filter_set(opts.symbols.begin(), opts.symbols.end());

    for (const auto& key : kline_keys) {
        // key格式: kline:exchange:symbol:1m
        size_t p1 = key.find(':');
        if (p1 == std::string::npos) continue;
        size_t p2 = key.find(':', p1 + 1);
        if (p2 == std::string::npos) continue;
        size_t p3 = key.find(':', p2 + 1);
        if (p3 == std::string::npos) continue;

        std::string exchange = key.substr(p1 + 1, p2 - p1 - 1);
        std::string symbol = key.substr(p2 + 1, p3 - p2 - 1);

        // 只处理U本位合约
        if (!is_usdt_contract(exchange, symbol)) continue;

        // 交易所过滤
        if (!opts.exchange_filter.empty() && exchange != opts.exchange_filter) continue;

        // 币种过滤
        if (!symbol_filter_set.empty() && symbol_filter_set.find(symbol) == symbol_filter_set.end()) continue;

        all_symbols.push_back({exchange, symbol});
    }

    // 排序
    std::sort(all_symbols.begin(), all_symbols.end(),
        [](const SymbolInfo& a, const SymbolInfo& b) {
            if (a.exchange != b.exchange) return a.exchange < b.exchange;
            return a.symbol < b.symbol;
        });

    std::cout << "[发现] 找到 " << all_symbols.size() << " 个U本位合约\n\n";

    if (all_symbols.empty()) {
        redisFree(ctx);
        return 0;
    }

    // 创建fetcher（公开市场数据不需要API密钥）
    std::unique_ptr<trading::historical_fetcher::OKXHistoricalFetcher> okx_fetcher;
    std::unique_ptr<trading::historical_fetcher::BinanceHistoricalFetcher> binance_fetcher;

    if (!opts.dry_run) {
        okx_fetcher = std::make_unique<trading::historical_fetcher::OKXHistoricalFetcher>("", "", "", false);
        binance_fetcher = std::make_unique<trading::historical_fetcher::BinanceHistoricalFetcher>("", "", false);
    }

    // 统计
    int total_symbols = (int)all_symbols.size();
    int symbols_with_gaps = 0;
    int total_missing = 0;
    int total_filled = 0;
    const std::string interval = "1m";
    int64_t interval_ms = trading::kline_utils::get_interval_milliseconds(interval);

    // 遍历每个币种
    for (int idx = 0; idx < total_symbols; idx++) {
        const auto& info = all_symbols[idx];
        std::string key = "kline:" + info.exchange + ":" + info.symbol + ":" + interval;

        // 获取时间范围
        int64_t first_ts = 0, last_ts = 0;
        if (!get_ts_range(ctx, key, first_ts, last_ts)) {
            if (opts.verbose) {
                std::cout << "[" << (idx + 1) << "/" << total_symbols << "] "
                          << info.exchange << ":" << info.symbol << " - 无数据，跳过\n";
            }
            continue;
        }

        // 阶段1：粗扫描
        auto missing_chunks = pipeline_zcount_scan(ctx, key, first_ts, last_ts, interval_ms);

        if (missing_chunks.empty()) {
            if (opts.verbose) {
                std::cout << "[" << (idx + 1) << "/" << total_symbols << "] "
                          << info.exchange << ":" << info.symbol << " ✓ 完整\n";
            }
            continue;
        }

        // 阶段2：精扫描
        std::vector<int64_t> all_missing;
        for (const auto& chunk : missing_chunks) {
            auto chunk_missing = fine_scan_gaps(ctx, key, chunk, interval_ms);
            all_missing.insert(all_missing.end(), chunk_missing.begin(), chunk_missing.end());
        }

        if (all_missing.empty()) {
            if (opts.verbose) {
                std::cout << "[" << (idx + 1) << "/" << total_symbols << "] "
                          << info.exchange << ":" << info.symbol << " ✓ 完整（精扫描确认）\n";
            }
            continue;
        }

        std::sort(all_missing.begin(), all_missing.end());
        symbols_with_gaps++;
        total_missing += (int)all_missing.size();

        // 合并缺失区间
        auto gaps = merge_gaps(all_missing, interval_ms);

        std::cout << "[" << (idx + 1) << "/" << total_symbols << "] "
                  << info.exchange << ":" << info.symbol
                  << " 缺失 " << all_missing.size() << " 根K线"
                  << "（" << gaps.size() << " 个区间）\n";

        if (opts.verbose) {
            for (size_t i = 0; i < gaps.size(); i++) {
                std::cout << "    区间" << (i + 1) << ": "
                          << trading::kline_utils::format_timestamp(gaps[i].start_ts)
                          << " ~ " << trading::kline_utils::format_timestamp(gaps[i].end_ts)
                          << " (" << gaps[i].count(interval_ms) << "根)\n";
            }
        }

        // 阶段3：补全
        if (!opts.dry_run) {
            trading::historical_fetcher::HistoricalDataFetcher* fetcher = nullptr;
            if (info.exchange == "binance") {
                fetcher = binance_fetcher.get();
            } else if (info.exchange == "okx") {
                fetcher = okx_fetcher.get();
            }

            if (fetcher) {
                int filled = fetch_and_fill(ctx, fetcher, info.exchange, info.symbol, interval, gaps);
                total_filled += filled;
                std::cout << "    ✓ 补全 " << filled << " 根K线\n";
            }
        }
    }

    redisFree(ctx);

    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    // 汇总报告
    std::cout << "\n╔════════════════════════════════════════════════════════════╗\n"
              << "║  汇总报告                                                  ║\n"
              << "╠════════════════════════════════════════════════════════════╣\n"
              << "║  扫描币种: " << total_symbols << "\n"
              << "║  有缺失: " << symbols_with_gaps << "\n"
              << "║  缺失K线总数: " << total_missing << "\n";
    if (!opts.dry_run) {
        std::cout << "║  已补全: " << total_filled << "\n";
    }
    std::cout << "║  耗时: " << std::fixed << std::setprecision(2) << elapsed << " 秒\n"
              << "╚════════════════════════════════════════════════════════════╝\n";

    return 0;
}
