#pragma once

#include <string>
#include <vector>
#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>

namespace trading {
namespace gap_detector {

/**
 * @brief 缺失段结构
 */
struct Gap {
    int64_t start_ts;  // 缺失开始时间（毫秒）
    int64_t end_ts;    // 缺失结束时间（毫秒）

    // 计算缺失的K线数量
    int count(int64_t interval_ms) const {
        return static_cast<int>((end_ts - start_ts) / interval_ms) + 1;
    }
};

/**
 * @brief 缺失检测器类
 */
class GapDetector {
public:
    /**
     * @brief 构造函数
     *
     * @param redis_host Redis主机地址
     * @param redis_port Redis端口
     */
    GapDetector(const std::string& redis_host, int redis_port);

    ~GapDetector();

    /**
     * @brief 连接到Redis
     *
     * @return true 连接成功
     * @return false 连接失败
     */
    bool connect();

    /**
     * @brief 检测指定symbol和interval的K线缺失
     *
     * @param symbol 交易对符号（如 "BTC-USDT-SWAP"）
     * @param interval K线周期（如 "5m", "1H"）
     * @return std::vector<Gap> 缺失段列表
     */
    std::vector<Gap> detect_gaps(const std::string& symbol, const std::string& interval);

    /**
     * @brief 获取Redis中K线的时间范围
     *
     * @param symbol 交易对符号
     * @param interval K线周期
     * @param[out] first_ts 最早的K线时间戳
     * @param[out] last_ts 最新的K线时间戳
     * @return true 成功获取
     * @return false 失败（可能没有数据）
     */
    bool get_time_range(const std::string& symbol, const std::string& interval,
                        int64_t& first_ts, int64_t& last_ts);

    /**
     * @brief 获取Redis中K线的数量
     *
     * @param symbol 交易对符号
     * @param interval K线周期
     * @return int K线数量
     */
    int get_kline_count(const std::string& symbol, const std::string& interval);

private:
    std::string redis_host_;
    int redis_port_;
    redisContext* context_;
};

} // namespace gap_detector
} // namespace trading
