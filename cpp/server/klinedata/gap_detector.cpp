#include "gap_detector.h"
#include "kline_utils.h"
#include <iostream>
#include <algorithm>
#include <chrono>

namespace trading {
namespace gap_detector {

GapDetector::GapDetector(const std::string& redis_host, int redis_port)
    : redis_host_(redis_host), redis_port_(redis_port), context_(nullptr) {
}

GapDetector::~GapDetector() {
    if (context_) {
        redisFree(context_);
        context_ = nullptr;
    }
}

bool GapDetector::connect() {
    context_ = redisConnect(redis_host_.c_str(), redis_port_);

    if (context_ == nullptr || context_->err) {
        if (context_) {
            std::cerr << "[GapDetector] Redis连接错误: " << context_->errstr << std::endl;
            redisFree(context_);
            context_ = nullptr;
        } else {
            std::cerr << "[GapDetector] Redis连接错误: 无法分配上下文" << std::endl;
        }
        return false;
    }

    std::cout << "[GapDetector] 已连接到Redis " << redis_host_ << ":" << redis_port_ << std::endl;
    return true;
}

std::vector<Gap> GapDetector::detect_gaps(const std::string& symbol, const std::string& interval) {
    std::vector<Gap> gaps;

    if (!context_) {
        std::cerr << "[GapDetector] 未连接到Redis" << std::endl;
        return gaps;
    }

    std::string key = "kline:" + symbol + ":" + interval;

    // 获取所有K线（只需要时间戳）
    redisReply* reply = (redisReply*)redisCommand(context_, "ZRANGE %s 0 -1", key.c_str());

    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return gaps;
    }

    if (reply->elements == 0) {
        freeReplyObject(reply);
        return gaps;  // 没有数据，返回空
    }

    // 解析所有时间戳
    std::vector<int64_t> timestamps;
    for (size_t i = 0; i < reply->elements; i++) {
        try {
            nlohmann::json kline_json = nlohmann::json::parse(reply->element[i]->str);
            int64_t timestamp = kline_json.value("timestamp", 0LL);
            if (timestamp > 0) {
                timestamps.push_back(timestamp);
            }
        } catch (const std::exception& e) {
            std::cerr << "[GapDetector] 解析K线JSON失败: " << e.what() << std::endl;
        }
    }

    freeReplyObject(reply);

    if (timestamps.empty()) {
        return gaps;
    }

    // 排序时间戳
    std::sort(timestamps.begin(), timestamps.end());

    std::cout << "[GapDetector] 共有 " << timestamps.size() << " 个时间戳" << std::endl;

    // 打印最后10个时间戳用于调试
    if (timestamps.size() > 0) {
        std::cout << "[GapDetector] 最后10个时间戳:" << std::endl;
        size_t start_idx = timestamps.size() > 10 ? timestamps.size() - 10 : 0;
        for (size_t i = start_idx; i < timestamps.size(); i++) {
            std::cout << "  " << (i + 1) << ". " << kline_utils::format_timestamp(timestamps[i]) << std::endl;
        }
    }

    // 获取周期毫秒数
    int64_t interval_ms = kline_utils::get_interval_milliseconds(interval);

    // 0. 检测数据开始之前的缺口
    std::cout << "[GapDetector] 检测数据开始之前的缺口..." << std::endl;
    int64_t first_ts = timestamps.front();

    // 根据周期检测开始前的缺口
    int64_t days_to_check = 0;
    if (interval == "1m" || interval == "5m" || interval == "15m" || interval == "30m") {
        days_to_check = 60;  // 1m-30m: 60天
    } else if (interval == "1h" || interval == "1H") {
        days_to_check = 180;  // 1h: 180天（6个月）
    }

    if (days_to_check > 0) {
        // 计算N天前的时间戳
        int64_t current_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        int64_t target_days_ago = current_time - (days_to_check * 24LL * 60 * 60 * 1000);

        // 对齐到周期边界
        int64_t aligned_start = (target_days_ago / interval_ms) * interval_ms;

        // 如果当前数据起始时间晚于目标时间，则检测缺口
        if (first_ts > aligned_start) {
            Gap gap;
            gap.start_ts = aligned_start;
            gap.end_ts = first_ts - interval_ms;
            gaps.push_back(gap);

            std::cout << "[GapDetector] 检测到数据开始前的缺口: "
                      << kline_utils::format_timestamp(gap.start_ts)
                      << " ~ " << kline_utils::format_timestamp(gap.end_ts)
                      << " (从60天前开始)" << std::endl;
        }
    }

    // 1. 检测已有数据中间的缺失
    std::cout << "[GapDetector] 开始检测历史缺失..." << std::endl;
    for (size_t i = 0; i < timestamps.size() - 1; i++) {
        int64_t current_ts = timestamps[i];
        int64_t next_ts = timestamps[i + 1];
        int64_t expected_next = current_ts + interval_ms;

        // 调试：打印前10个检查
        if (i < 10) {
            std::cout << "[GapDetector] 检查 " << (i + 1) << ": "
                      << kline_utils::format_timestamp(current_ts)
                      << " -> " << kline_utils::format_timestamp(next_ts)
                      << " (期望: " << kline_utils::format_timestamp(expected_next) << ")"
                      << std::endl;
        }

        // 如果下一个时间戳不是期望的，说明有缺失
        if (next_ts > expected_next) {
            Gap gap;
            gap.start_ts = expected_next;
            gap.end_ts = next_ts - interval_ms;
            gaps.push_back(gap);

            // 调试输出
            std::cout << "[GapDetector] 检测到历史缺失: "
                      << kline_utils::format_timestamp(current_ts)
                      << " -> " << kline_utils::format_timestamp(next_ts)
                      << " (补全: " << kline_utils::format_timestamp(gap.start_ts)
                      << " ~ " << kline_utils::format_timestamp(gap.end_ts) << ")"
                      << std::endl;
        }
    }
    std::cout << "[GapDetector] 历史缺失检测完成，发现 " << gaps.size() << " 个缺失段" << std::endl;

    // 2. 检测从最新K线到当前时间的缺失
    int64_t last_ts = timestamps.back();
    int64_t current_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // 对齐到K线周期边界（向下取整）
    int64_t aligned_current_time = (current_time / interval_ms) * interval_ms;

    // 计算期望的下一根K线时间
    int64_t expected_next = last_ts + interval_ms;

    // 如果当前时间已经超过了期望的下一根K线时间，说明有缺失
    // 注意：我们补全到 aligned_current_time - interval_ms，因为当前K线可能还未完成
    if (aligned_current_time > last_ts) {
        Gap gap;
        gap.start_ts = expected_next;
        gap.end_ts = aligned_current_time - interval_ms;  // 不包含当前未完成的K线

        // 只有当缺失至少1根K线时才添加
        if (gap.end_ts >= gap.start_ts) {
            gaps.push_back(gap);
        }
    }

    return gaps;
}

bool GapDetector::get_time_range(const std::string& symbol, const std::string& interval,
                                  int64_t& first_ts, int64_t& last_ts) {
    if (!context_) {
        return false;
    }

    std::string key = "kline:" + symbol + ":" + interval;

    // 获取第一个K线
    redisReply* reply = (redisReply*)redisCommand(context_, "ZRANGE %s 0 0", key.c_str());

    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) {
        if (reply) freeReplyObject(reply);
        return false;
    }

    try {
        nlohmann::json first_kline = nlohmann::json::parse(reply->element[0]->str);
        first_ts = first_kline.value("timestamp", 0LL);
    } catch (const std::exception& e) {
        freeReplyObject(reply);
        return false;
    }

    freeReplyObject(reply);

    // 获取最后一个K线
    reply = (redisReply*)redisCommand(context_, "ZRANGE %s -1 -1", key.c_str());

    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) {
        if (reply) freeReplyObject(reply);
        return false;
    }

    try {
        nlohmann::json last_kline = nlohmann::json::parse(reply->element[0]->str);
        last_ts = last_kline.value("timestamp", 0LL);
    } catch (const std::exception& e) {
        freeReplyObject(reply);
        return false;
    }

    freeReplyObject(reply);
    return true;
}

int GapDetector::get_kline_count(const std::string& symbol, const std::string& interval) {
    if (!context_) {
        return 0;
    }

    std::string key = "kline:" + symbol + ":" + interval;

    redisReply* reply = (redisReply*)redisCommand(context_, "ZCARD %s", key.c_str());

    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        if (reply) freeReplyObject(reply);
        return 0;
    }

    int count = reply->integer;
    freeReplyObject(reply);

    return count;
}

} // namespace gap_detector
} // namespace trading
