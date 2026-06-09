#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace trading {
namespace kline_utils {

/**
 * @brief K线数据结构
 */
struct Kline {
    int64_t timestamp;      // 时间戳（毫秒）
    double open;            // 开盘价
    double high;            // 最高价
    double low;             // 最低价
    double close;           // 收盘价
    double volume;          // 成交量
    double amount;          // 成交额（仅Binance）
    double buy_amount;      // 主动买入成交额（仅Binance）
    int64_t trades;         // 笔数（仅Binance）

    // 转换为JSON
    nlohmann::json to_json() const;
};

/**
 * @brief 获取K线周期的毫秒数
 *
 * @param interval K线周期字符串（如 "5m", "1H"）
 * @return int64_t 周期对应的毫秒数
 */
int64_t get_interval_milliseconds(const std::string& interval);

/**
 * @brief 将时间戳对齐到周期边界
 *
 * @param timestamp 原始时间戳（毫秒）
 * @param period_ms 周期毫秒数
 * @return int64_t 对齐后的时间戳
 */
int64_t align_timestamp(int64_t timestamp, int64_t period_ms);

/**
 * @brief 解析OKX K线数据
 *
 * OKX返回格式: [timestamp, open, high, low, close, volume, ...]
 *
 * @param candle_data OKX返回的K线数组
 * @return Kline 解析后的K线结构
 */
Kline parse_okx_candle(const nlohmann::json& candle_data);

/**
 * @brief 解析Binance K线数据
 *
 * Binance返回格式: [timestamp, open, high, low, close, volume, ...]
 *
 * @param kline_data Binance返回的K线数组
 * @return Kline 解析后的K线结构
 */
Kline parse_binance_kline(const nlohmann::json& kline_data);

/**
 * @brief 格式化时间戳为可读字符串
 *
 * @param timestamp_ms 时间戳（毫秒）
 * @return std::string 格式化后的时间字符串（YYYY-MM-DD HH:MM:SS）
 */
std::string format_timestamp(int64_t timestamp_ms);

/**
 * @brief 交易对信息（exchange + symbol）
 */
struct SymbolInfo {
    std::string exchange;
    std::string symbol;
};

/**
 * @brief 检查是否为U本位合约
 * OKX: 包含 -USDT-SWAP; Binance: 以 USDT 结尾
 */
bool is_usdt_contract(const std::string& exchange, const std::string& symbol);

} // namespace kline_utils
} // namespace trading
