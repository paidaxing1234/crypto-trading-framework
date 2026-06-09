#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "gap_detector.h"
#include "historical_data_fetcher.h"
#include "kline_utils.h"
#include <hiredis/hiredis.h>

using json = nlohmann::json;
using trading::kline_utils::SymbolInfo;
using trading::kline_utils::is_usdt_contract;

// ==================== 配置 ====================

struct Config {
    // Redis配置
    static std::string redis_host;
    static int redis_port;
    static std::string redis_password;

    // 交易对列表
    static std::vector<std::string> symbols;

    // K线周期
    static std::vector<std::string> intervals;

    // 聚合配置: {目标周期, {基础周期, 聚合倍数}}
    static std::map<std::string, std::pair<std::string, int>> aggregated_intervals;

    // 过期时间（秒）
    static int expire_seconds_1m_to_30m;  // 1min、5min、15min、30min：2个月
    static int expire_seconds_1h;         // 1H：6个月

    // 测试网配置
    static bool is_testnet;
};

// 静态成员初始化
std::string Config::redis_host = "127.0.0.1";
int Config::redis_port = 6379;
std::string Config::redis_password = "";

// 从Redis动态获取，不再硬编码
std::vector<std::string> Config::symbols = {};

std::vector<std::string> Config::intervals = {"1m", "1h"};  // 拉取1分钟和1小时K线

// ==================== 全市场合约配置 ====================
// 补全Redis中所有已存在的U本位合约K线数据
// 不使用白名单，自动处理所有交易所的全市场合约

std::map<std::string, std::pair<std::string, int>> Config::aggregated_intervals = {
    {"5m", {"1m", 5}},       // 5个1分钟 -> 5分钟
    {"15m", {"1m", 15}},     // 15个1分钟 -> 15分钟
    {"30m", {"1m", 30}},     // 30个1分钟 -> 30分钟
    // 1h 直接从API拉取（需要6个月数据）
    {"4h", {"1m", 240}},     // 240个1分钟 -> 4小时
    {"8h", {"1m", 480}}      // 480个1分钟 -> 8小时
};

int Config::expire_seconds_1m_to_30m = 60 * 24 * 60 * 60;  // 2个月
int Config::expire_seconds_1h = 180 * 24 * 60 * 60;  // 6个月
bool Config::is_testnet = false;  // 默认使用主网获取历史K线数据

// ==================== Redis写入器 ====================

class RedisWriter {
public:
    RedisWriter(const std::string& host, int port) : host_(host), port_(port), context_(nullptr) {}

    ~RedisWriter() {
        if (context_) {
            redisFree(context_);
        }
    }

    bool connect() {
        context_ = redisConnect(host_.c_str(), port_);

        if (context_ == nullptr || context_->err) {
            if (context_) {
                std::cerr << "[RedisWriter] 连接错误: " << context_->errstr << std::endl;
                redisFree(context_);
                context_ = nullptr;
            }
            return false;
        }

        std::cout << "[RedisWriter] 已连接到Redis " << host_ << ":" << port_ << std::endl;
        return true;
    }

    bool write_kline(const std::string& exchange, const std::string& symbol, const std::string& interval,
                     const trading::kline_utils::Kline& kline, bool is_aggregated = false) {
        if (!context_) return false;

        // Match data_recorder format: kline:exchange:symbol:interval
        std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;

        // Create JSON with all required fields to match data_recorder format
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

        // ZADD添加到有序集合
        redisReply* reply = (redisReply*)redisCommand(
            context_, "ZADD %s %lld %s",
            key.c_str(), (long long)kline.timestamp, value.c_str()
        );

        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            if (reply) freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);

        // 设置过期时间
        int expire_seconds;
        if (interval == "1h") {
            expire_seconds = Config::expire_seconds_1h;  // 1h：6个月
        } else {
            // 1min、5min、15min、30min、4h、8h：都是2个月
            expire_seconds = Config::expire_seconds_1m_to_30m;
        }

        reply = (redisReply*)redisCommand(
            context_, "EXPIRE %s %d", key.c_str(), expire_seconds
        );
        if (reply) freeReplyObject(reply);

        return true;
    }

    int write_klines_batch(const std::string& exchange, const std::string& symbol, const std::string& interval,
                           const std::vector<trading::kline_utils::Kline>& klines,
                           bool is_aggregated = false) {
        if (!context_ || klines.empty()) return 0;

        std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;

        // 使用Pipeline批量写入
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

            // 使用append模式批量发送命令
            redisAppendCommand(context_, "ZADD %s %lld %s",
                key.c_str(), (long long)kline.timestamp, value.c_str());
        }

        // 批量获取响应
        int count = 0;
        for (size_t i = 0; i < klines.size(); i++) {
            redisReply* reply = nullptr;
            if (redisGetReply(context_, (void**)&reply) == REDIS_OK) {
                if (reply && reply->type != REDIS_REPLY_ERROR) {
                    count++;
                }
                if (reply) freeReplyObject(reply);
            }
        }

        // 设置过期时间（只需要设置一次）
        int expire_seconds = (interval == "1h") ? Config::expire_seconds_1h : Config::expire_seconds_1m_to_30m;
        redisReply* expire_reply = (redisReply*)redisCommand(context_, "EXPIRE %s %d", key.c_str(), expire_seconds);
        if (expire_reply) freeReplyObject(expire_reply);

        return count;
    }

private:
    std::string host_;
    int port_;
    redisContext* context_;
};

// ==================== K线聚合器（简化版） ====================

class SimpleAggregator {
public:
    static trading::kline_utils::Kline aggregate(
        const std::vector<trading::kline_utils::Kline>& klines,
        int64_t aligned_timestamp
    ) {
        trading::kline_utils::Kline result;
        result.timestamp = aligned_timestamp;
        result.open = klines[0].open;
        result.close = klines.back().close;
        result.high = klines[0].high;
        result.low = klines[0].low;
        result.volume = 0.0;

        for (const auto& k : klines) {
            result.high = std::max(result.high, k.high);
            result.low = std::min(result.low, k.low);
            result.volume += k.volume;
        }

        return result;
    }
};

// ==================== 主程序 ====================

// 从Redis key中提取exchange信息
std::string extract_exchange_from_key(const std::string& key) {
    // key格式: kline:exchange:symbol:interval
    // 例如: kline:okx:BTC-USDT-SWAP:1m 或 kline:binance:BTCUSDT:1m
    size_t first_colon = key.find(':');
    if (first_colon == std::string::npos) return "";

    size_t second_colon = key.find(':', first_colon + 1);
    if (second_colon == std::string::npos) return "";

    return key.substr(first_colon + 1, second_colon - first_colon - 1);
}

// 从Redis key中提取symbol信息
std::string extract_symbol_from_key(const std::string& key) {
    // key格式: kline:exchange:symbol:interval
    size_t first_colon = key.find(':');
    if (first_colon == std::string::npos) return "";

    size_t second_colon = key.find(':', first_colon + 1);
    if (second_colon == std::string::npos) return "";

    size_t third_colon = key.find(':', second_colon + 1);
    if (third_colon == std::string::npos) return "";

    return key.substr(second_colon + 1, third_colon - second_colon - 1);
}

bool is_okx_symbol(const std::string& symbol) {
    // OKX符号格式：BTC-USDT-SWAP, BTC-USDT, ETH-USD-SWAP等
    return symbol.find("-SWAP") != std::string::npos ||
           symbol.find("-USDT") != std::string::npos ||
           symbol.find("-USD") != std::string::npos;
}

void fill_gaps_for_symbol(
    const std::string& exchange,
    const std::string& symbol,
    const std::string& interval,
    trading::gap_detector::GapDetector& detector,
    trading::historical_fetcher::HistoricalDataFetcher* fetcher,
    RedisWriter& writer
) {
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "[GapFiller] 检查 " << exchange << ":" << symbol << ":" << interval << std::endl;

    // 使用完整的key格式检测缺失: kline:exchange:symbol:interval
    std::string full_key = exchange + ":" + symbol;
    auto gaps = detector.detect_gaps(full_key, interval);

    if (gaps.empty()) {
        std::cout << "[GapFiller] ✓ 无缺失" << std::endl;
        return;
    }

    int64_t interval_ms = trading::kline_utils::get_interval_milliseconds(interval);

    std::cout << "[GapFiller] 发现 " << gaps.size() << " 个缺失段" << std::endl;

    int total_filled = 0;

    for (size_t i = 0; i < gaps.size(); i++) {
        const auto& gap = gaps[i];
        int gap_count = gap.count(interval_ms);

        std::cout << "[GapFiller]   缺失" << (i + 1) << ": "
                  << trading::kline_utils::format_timestamp(gap.start_ts)
                  << " ~ " << trading::kline_utils::format_timestamp(gap.end_ts)
                  << " (" << gap_count << "根)" << std::endl;

        // 确定API符号格式
        std::string api_symbol = symbol;
        bool is_okx = (exchange == "okx");

        if (!is_okx) {
            // Binance符号已经是正确格式(BTCUSDT)，不需要转换
            std::cout << "[GapFiller]   Binance符号: " << api_symbol << std::endl;
        }

        // 拉取历史数据
        auto klines = fetcher->fetch_history(api_symbol, interval, gap.start_ts, gap.end_ts);

        if (klines.empty()) {
            std::cerr << "[GapFiller]   ✗ 拉取失败" << std::endl;
            continue;
        }

        // 写入Redis（使用完整的key格式）
        int written = writer.write_klines_batch(exchange, symbol, interval, klines, false);
        total_filled += written;

        std::cout << "[GapFiller]   ✓ 拉取并写入 " << written << " 根K线" << std::endl;
    }

    std::cout << "[GapFiller] " << exchange << ":" << symbol << ":" << interval << " 补全完成，共 " << total_filled << " 根" << std::endl;
}

/**
 * @brief 去除指定key中的重复K线数据（使用连接池）
 *
 * 对于相同时间戳的K线，只保留一条（保留最后一条）
 *
 * @return 删除的重复数据数量
 */
int deduplicate_klines(redisContext* context, const std::string& exchange, const std::string& symbol, const std::string& interval) {
    if (!context) return 0;

    std::string key = "kline:" + exchange + ":" + symbol + ":" + interval;

    // 获取所有数据（带分数）
    redisReply* reply = (redisReply*)redisCommand(context, "ZRANGE %s 0 -1 WITHSCORES", key.c_str());

    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) {
        if (reply) freeReplyObject(reply);
        return 0;
    }

    // 按时间戳分组，找出重复的
    std::map<int64_t, std::vector<std::string>> timestamp_groups;

    // WITHSCORES 返回的是 value, score, value, score... 的格式
    for (size_t i = 0; i < reply->elements; i += 2) {
        std::string value = reply->element[i]->str;
        int64_t timestamp = std::stoll(reply->element[i + 1]->str);
        timestamp_groups[timestamp].push_back(std::move(value));
    }

    freeReplyObject(reply);

    // 统计重复数量
    int duplicates_count = 0;
    for (const auto& [ts, values] : timestamp_groups) {
        if (values.size() > 1) {
            duplicates_count += values.size() - 1;
        }
    }

    if (duplicates_count == 0) {
        return 0;
    }

    std::cout << "[Deduplicator] " << exchange << ":" << symbol << ":" << interval
              << " 发现 " << duplicates_count << " 条重复数据，开始去重..." << std::endl;

    // 删除整个key
    redisCommand(context, "DEL %s", key.c_str());

    // 使用Pipeline批量重新插入去重后的数据
    for (const auto& [timestamp, values] : timestamp_groups) {
        const std::string& value = values.back();
        redisAppendCommand(context, "ZADD %s %lld %s",
            key.c_str(), (long long)timestamp, value.c_str());
    }

    // 批量获取响应
    for (size_t i = 0; i < timestamp_groups.size(); i++) {
        redisReply* add_reply = nullptr;
        redisGetReply(context, (void**)&add_reply);
        if (add_reply) freeReplyObject(add_reply);
    }

    std::cout << "[Deduplicator] ✓ 已删除 " << duplicates_count << " 条重复数据" << std::endl;
    return duplicates_count;
}

void aggregate_filled_klines(
    redisContext* context,
    const std::string& exchange,
    const std::string& symbol,
    const std::string& target_interval,
    const std::string& base_interval,
    int multiplier,
    RedisWriter& writer
) {
    if (!context) return;

    std::cout << "\n[Aggregator] 聚合 " << exchange << ":" << symbol << " " << base_interval << " -> " << target_interval << std::endl;

    std::string full_key = exchange + ":" + symbol;
    std::string base_key = "kline:" + full_key + ":" + base_interval;
    std::string target_key = "kline:" + full_key + ":" + target_interval;

    // 🆕 步骤1: 获取目标周期已存在的时间戳，用于去重
    std::set<int64_t> existing_timestamps;
    redisReply* existing_reply = (redisReply*)redisCommand(context, "ZRANGE %s 0 -1 WITHSCORES", target_key.c_str());
    if (existing_reply && existing_reply->type == REDIS_REPLY_ARRAY) {
        // WITHSCORES 返回的是 value, score, value, score... 的格式
        for (size_t i = 1; i < existing_reply->elements; i += 2) {
            int64_t ts = std::stoll(existing_reply->element[i]->str);
            existing_timestamps.insert(ts);
        }
        freeReplyObject(existing_reply);
    }

    if (!existing_timestamps.empty()) {
        std::cout << "[Aggregator] 目标周期已有 " << existing_timestamps.size() << " 根K线，将只聚合缺失部分" << std::endl;
    }

    // 步骤2: 读取基础K线
    redisReply* reply = (redisReply*)redisCommand(context, "ZRANGE %s 0 -1", base_key.c_str());

    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return;
    }

    // 解析所有K线（预分配内存）
    std::vector<trading::kline_utils::Kline> base_klines;
    base_klines.reserve(reply->elements);

    for (size_t i = 0; i < reply->elements; i++) {
        try {
            json kline_json = json::parse(reply->element[i]->str);
            trading::kline_utils::Kline kline;
            kline.timestamp = kline_json["timestamp"];
            kline.open = kline_json["open"];
            kline.high = kline_json["high"];
            kline.low = kline_json["low"];
            kline.close = kline_json["close"];
            kline.volume = kline_json["volume"];
            base_klines.push_back(std::move(kline));
        } catch (const std::exception& e) {
            std::cerr << "[Aggregator] 解析K线失败: " << e.what() << std::endl;
        }
    }

    freeReplyObject(reply);

    if (base_klines.empty()) {
        std::cout << "[Aggregator] 没有基础K线数据" << std::endl;
        return;
    }

    // 步骤3: 按周期分组并聚合
    int64_t base_period_ms = trading::kline_utils::get_interval_milliseconds(base_interval);
    int64_t target_period_ms = base_period_ms * multiplier;

    std::map<int64_t, std::vector<trading::kline_utils::Kline>> groups;

    for (auto& kline : base_klines) {
        int64_t aligned_ts = trading::kline_utils::align_timestamp(kline.timestamp, target_period_ms);
        groups[aligned_ts].push_back(std::move(kline));
    }

    // 步骤4: 对每个分组去重（同一时间戳只保留最后一条）
    for (auto& [aligned_ts, klines] : groups) {
        std::map<int64_t, trading::kline_utils::Kline> dedup_map;
        for (auto& kline : klines) {
            dedup_map[kline.timestamp] = std::move(kline);  // 相同时间戳会被覆盖
        }

        // 替换为去重后的K线
        klines.clear();
        klines.reserve(dedup_map.size());
        for (auto& [ts, kline] : dedup_map) {
            klines.push_back(std::move(kline));
        }
    }

    // 步骤5: 聚合并写入（只写入不存在的时间戳）
    int aggregated_count = 0;
    int skipped_count = 0;
    int incomplete_count = 0;
    for (auto& [aligned_ts, klines] : groups) {  // 改为非const引用
        // 检查该时间戳是否已存在
        if (existing_timestamps.find(aligned_ts) != existing_timestamps.end()) {
            skipped_count++;
            continue;  // 跳过已存在的时间戳
        }

        // 只要有足够的K线就聚合（>= multiplier）
        // 注意：去重后可能不足multiplier根，这种情况跳过
        if (klines.size() >= static_cast<size_t>(multiplier)) {
            // 按时间戳排序，确保顺序正确
            std::sort(klines.begin(), klines.end(),
                [](const trading::kline_utils::Kline& a, const trading::kline_utils::Kline& b) {
                    return a.timestamp < b.timestamp;
                });

            // 只取前multiplier根进行聚合
            std::vector<trading::kline_utils::Kline> klines_to_aggregate;
            klines_to_aggregate.reserve(multiplier);
            for (size_t i = 0; i < static_cast<size_t>(multiplier) && i < klines.size(); i++) {
                klines_to_aggregate.push_back(klines[i]);
            }

            auto aggregated = SimpleAggregator::aggregate(klines_to_aggregate, aligned_ts);
            if (writer.write_kline(exchange, symbol, target_interval, aggregated, true)) {
                aggregated_count++;
            }
        } else {
            // 基础K线不足，无法聚合
            incomplete_count++;
        }
    }

    std::cout << "[Aggregator] 生成 " << aggregated_count << " 根新 " << target_interval << " K线";
    if (skipped_count > 0) {
        std::cout << "，跳过 " << skipped_count << " 根已存在的K线";
    }
    if (incomplete_count > 0) {
        std::cout << "，跳过 " << incomplete_count << " 个基础K线不足的时间段";
    }
    std::cout << std::endl;
}

// ==================== 配置加载 ====================

/**
 * @brief 加载账户配置（可选，公开市场数据不需要API密钥）
 */
bool load_config(const std::string& config_file = "accounts.json") {
    std::cout << "[配置] 加载配置文件: " << config_file << std::endl;

    // 尝试从多个位置加载配置文件
    std::vector<std::string> config_paths = {
        config_file,
        "server/" + config_file,
        "../server/" + config_file,
        "../../server/" + config_file
    };

    json config;
    bool loaded = false;

    for (const auto& path : config_paths) {
        std::ifstream file(path);
        if (file.is_open()) {
            try {
                file >> config;
                loaded = true;
                std::cout << "[配置] 成功加载: " << path << std::endl;
                break;
            } catch (const std::exception& e) {
                std::cerr << "[配置] 解析失败: " << e.what() << std::endl;
            }
        }
    }

    if (!loaded) {
        std::cout << "[配置] 未找到配置文件，将使用公开市场数据端点（不需要API密钥）" << std::endl;
    }

    // 从配置文件或环境变量加载 testnet 配置
    // 注意：对于公开市场数据（K线历史），建议使用主网以获取完整数据
    if (loaded && config.contains("default") && config["default"].is_object()) {
        auto& def = config["default"];
        // 不从配置文件读取 is_testnet，保持默认值 false（主网）
        // Config::is_testnet = def.value("is_testnet", false);
    }

    const char* testnet_env = std::getenv("TESTNET");
    if (testnet_env) {
        Config::is_testnet = (std::string(testnet_env) == "1" || std::string(testnet_env) == "true");
        std::cout << "[配置] 环境变量覆盖: TESTNET=" << testnet_env << std::endl;
    }

    // 打印配置状态
    std::cout << "\n[配置] 运行模式: " << (Config::is_testnet ? "模拟盘/测试网" : "实盘/主网") << std::endl;
    std::cout << "[配置] 说明: K线历史数据通过公开市场数据端点获取，不需要API密钥" << std::endl;
    std::cout << "[配置] 建议: 使用主网端点以获取完整的历史K线数据" << std::endl;
    std::cout << std::endl;

    return true;
}

int main(int argc, char* argv[]) {
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║        K线缺失数据自动补全工具                              ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    // 加载配置（从文件或环境变量）
    if (!load_config()) {
        return 1;
    }

    // 打印配置信息
    std::cout << "[配置]" << std::endl;
    std::cout << "  Redis: " << Config::redis_host << ":" << Config::redis_port << std::endl;
    std::cout << "  1min~30min K线过期: " << Config::expire_seconds_1m_to_30m / (24 * 3600) << " 天" << std::endl;
    std::cout << "  1H K线过期: " << Config::expire_seconds_1h / (24 * 3600) << " 天" << std::endl;
    std::cout << std::endl;

    // 连接Redis
    trading::gap_detector::GapDetector detector(Config::redis_host, Config::redis_port);
    if (!detector.connect()) {
        std::cerr << "[GapFiller] Redis连接失败" << std::endl;
        return 1;
    }

    RedisWriter writer(Config::redis_host, Config::redis_port);
    if (!writer.connect()) {
        std::cerr << "[GapFiller] Redis写入器连接失败" << std::endl;
        return 1;
    }

    // 从Redis动态获取所有的kline keys
    std::cout << "[初始化] 从Redis获取所有K线数据..." << std::endl;
    redisContext* context = redisConnect(Config::redis_host.c_str(), Config::redis_port);
    if (!context || context->err) {
        std::cerr << "[GapFiller] Redis连接失败" << std::endl;
        return 1;
    }

    // 使用SCAN命令获取所有kline:*:1m的keys
    std::vector<std::string> kline_keys;
    redisReply* reply = (redisReply*)redisCommand(context, "KEYS kline:*:1m");
    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < reply->elements; i++) {
            kline_keys.push_back(reply->element[i]->str);
        }
        freeReplyObject(reply);
    }
    redisFree(context);

    if (kline_keys.empty()) {
        std::cout << "[GapFiller] Redis中没有找到任何1min K线数据" << std::endl;
        std::cout << "[GapFiller] 请先运行trading_server_full和data_recorder收集数据" << std::endl;
        return 0;
    }

    std::cout << "[初始化] 找到 " << kline_keys.size() << " 个币种的1min K线数据" << std::endl;

    // 解析keys，提取exchange和symbol信息
    std::vector<SymbolInfo> symbols;
    int filtered_count = 0;

    for (const auto& key : kline_keys) {
        // key格式: kline:exchange:symbol:1m
        std::string exchange = extract_exchange_from_key(key);
        std::string symbol = extract_symbol_from_key(key);

        if (!exchange.empty() && !symbol.empty()) {
            // 只处理U本位合约
            if (is_usdt_contract(exchange, symbol)) {
                symbols.push_back({exchange, symbol});
                std::cout << "  ✓ " << exchange << ":" << symbol << " (U本位合约)" << std::endl;
            } else {
                filtered_count++;
                std::cout << "  ✗ " << exchange << ":" << symbol << " (非U本位合约，跳过)" << std::endl;
            }
        }
    }

    std::cout << "\n[过滤结果] U本位合约: " << symbols.size() << " 个币种" << std::endl;
    std::cout << "[过滤结果] 已过滤: " << filtered_count << " 个币种" << std::endl;

    if (symbols.empty()) {
        std::cerr << "[GapFiller] 无法解析任何有效的symbol信息" << std::endl;
        return 1;
    }

    // 排序：先 Binance 后 OKX，按首字母排序，中文合约放最后
    std::sort(symbols.begin(), symbols.end(),
        [](const SymbolInfo& a, const SymbolInfo& b) {
            // 1. 先按交易所排序：binance < okx
            if (a.exchange != b.exchange) {
                return a.exchange < b.exchange;
            }

            // 2. 同一交易所内，检查是否包含中文字符
            auto has_chinese = [](const std::string& s) {
                for (unsigned char c : s) {
                    if (c > 127) return true;  // 简单判断：非ASCII即为中文
                }
                return false;
            };

            bool a_has_chinese = has_chinese(a.symbol);
            bool b_has_chinese = has_chinese(b.symbol);

            // 中文合约放最后
            if (a_has_chinese != b_has_chinese) {
                return !a_has_chinese;  // 没有中文的排前面
            }

            // 3. 都是英文或都是中文，按字母顺序
            return a.symbol < b.symbol;
        });

    std::cout << "\n[排序结果] 按 Binance -> OKX，首字母排序，中文合约最后" << std::endl;

    // 创建历史数据拉取器（公开市场数据不需要API密钥）
    std::unique_ptr<trading::historical_fetcher::OKXHistoricalFetcher> okx_fetcher;
    std::unique_ptr<trading::historical_fetcher::BinanceHistoricalFetcher> binance_fetcher;

    // OKX: 使用空凭证创建（公开市场数据端点不需要认证）
    okx_fetcher = std::make_unique<trading::historical_fetcher::OKXHistoricalFetcher>(
        "", "", "", Config::is_testnet
    );

    // Binance: 使用空凭证创建（公开市场数据端点不需要认证）
    binance_fetcher = std::make_unique<trading::historical_fetcher::BinanceHistoricalFetcher>(
        "", "", Config::is_testnet
    );

    std::cout << "\n[开始补全] 开始检测并补全缺失的K线数据..." << std::endl;

    // 创建共享的Redis连接用于去重和聚合操作
    redisContext* shared_context = redisConnect(Config::redis_host.c_str(), Config::redis_port);
    if (!shared_context || shared_context->err) {
        std::cerr << "[GapFiller] 创建共享Redis连接失败" << std::endl;
        return 1;
    }

    // 对每个symbol按照流程处理：去重1m → 补全1m → 去重其他周期 → 聚合其他周期
    for (const auto& info : symbols) {
        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "[处理] " << info.exchange << ":" << info.symbol << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

        // 步骤1: 检测并删除基础K线的重复数据（1m和1h）
        std::cout << "\n[步骤1/4] 检测并删除基础K线的重复数据..." << std::endl;
        int total_duplicates_base = 0;
        for (const auto& interval : Config::intervals) {
            int dup_count = deduplicate_klines(shared_context, info.exchange, info.symbol, interval);
            if (dup_count > 0) {
                std::cout << "[步骤1/4] ✓ 删除了 " << dup_count << " 条重复的" << interval << "K线" << std::endl;
                total_duplicates_base += dup_count;
            } else {
                std::cout << "[步骤1/4] ✓ " << interval << "K线无重复" << std::endl;
            }
        }
        if (total_duplicates_base == 0) {
            std::cout << "[步骤1/4] ✓ 所有基础K线无重复" << std::endl;
        }

        // 步骤2: 拉取缺失的基础K线（1m和1h）
        std::cout << "\n[步骤2/4] 拉取缺失的基础K线..." << std::endl;

        // 选择对应的拉取器
        trading::historical_fetcher::HistoricalDataFetcher* fetcher = nullptr;
        if (info.exchange == "okx") {
            fetcher = okx_fetcher.get();
        } else if (info.exchange == "binance") {
            fetcher = binance_fetcher.get();
        } else {
            std::cerr << "[GapFiller] 未知的交易所: " << info.exchange << std::endl;
            continue;
        }

        // 补全基础K线（1m和1h）
        for (const auto& interval : Config::intervals) {
            fill_gaps_for_symbol(info.exchange, info.symbol, interval, detector, fetcher, writer);
        }
        std::cout << "[步骤2/4] ✓ 基础K线补全完成（1m和1h）" << std::endl;

        // 步骤3: 去重其他周期的现有数据
        std::cout << "\n[步骤3/4] 检测并删除其他周期K线的重复数据..." << std::endl;
        int total_duplicates = 0;
        for (const auto& [target_interval, config] : Config::aggregated_intervals) {
            int dup_count = deduplicate_klines(shared_context, info.exchange, info.symbol, target_interval);
            total_duplicates += dup_count;
        }
        if (total_duplicates > 0) {
            std::cout << "[步骤3/4] ✓ 删除了 " << total_duplicates << " 条重复的K线" << std::endl;
        } else {
            std::cout << "[步骤3/4] ✓ 其他周期K线无重复" << std::endl;
        }

        // 步骤4: 从1分钟K线聚合生成其他周期
        std::cout << "\n[步骤4/4] 从1分钟K线聚合生成其他周期..." << std::endl;
        for (const auto& [target_interval, config] : Config::aggregated_intervals) {
            const auto& [base_interval, multiplier] = config;
            aggregate_filled_klines(shared_context, info.exchange, info.symbol, target_interval, base_interval, multiplier, writer);
        }
        std::cout << "[步骤4/4] ✓ 聚合完成" << std::endl;
    }

    // 释放共享连接
    redisFree(shared_context);

    std::cout << "\n╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║        K线补全完成！                                        ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;

    return 0;
}
