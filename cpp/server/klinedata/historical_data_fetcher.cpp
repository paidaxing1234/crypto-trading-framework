#include "historical_data_fetcher.h"
#include <iostream>
#include <thread>
#include <chrono>

namespace trading {
namespace historical_fetcher {

// ==================== OKX历史数据拉取器 ====================

OKXHistoricalFetcher::OKXHistoricalFetcher(
    const std::string& api_key,
    const std::string& secret_key,
    const std::string& passphrase,
    bool is_testnet
) {
    api_ = std::make_unique<okx::OKXRestAPI>(api_key, secret_key, passphrase, is_testnet);
}

std::vector<kline_utils::Kline> OKXHistoricalFetcher::fetch_history(
    const std::string& symbol,
    const std::string& interval,
    int64_t start_ts,
    int64_t end_ts
) {
    std::vector<kline_utils::Kline> klines;

    int64_t interval_ms = kline_utils::get_interval_milliseconds(interval);

    std::cout << "[OKXFetcher] 开始拉取 " << symbol << ":" << interval
              << " 从 " << kline_utils::format_timestamp(start_ts)
              << " 到 " << kline_utils::format_timestamp(end_ts) << std::endl;

    int total_fetched = 0;
    int retry_count = 0;
    const int max_retries = 2;  // 最多重试2次空数据

    // OKX API要求使用大写的时间周期（1H, 4H, 8H），但Redis key使用小写（1h, 4h, 8h）
    std::string okx_interval = interval;
    if (interval == "1h") okx_interval = "1H";
    else if (interval == "4h") okx_interval = "4H";
    else if (interval == "8h") okx_interval = "8H";

    // OKX限制：尝试每次拉取1000根K线（官方文档说最多100，但尝试更大值）
    // OKX API参数说明（实际测试结果）：
    // - after: 请求此时间戳之前的数据（更早的数据，时间戳 < after）
    // - before: 请求此时间戳之后的数据（更新的数据，时间戳 > before）
    // - 返回数据按时间戳降序排列（从新到旧）
    //
    // 策略：从end_ts开始向前拉取，使用after参数
    // 注意：加一个周期以确保包含end_ts本身（因为after参数获取的是此时间之前的数据）
    int64_t current_end = end_ts + interval_ms;
    int64_t min_ts = end_ts + interval_ms;  // 记录已拉取的最小时间戳

    while (current_end > start_ts && retry_count < max_retries) {
        try {
            std::cout << "[OKXFetcher] 请求参数: after=" << current_end
                      << " (" << kline_utils::format_timestamp(current_end) << ")" << std::endl;

            // 调用OKX API（使用history-candles端点获取历史K线数据）
            nlohmann::json response = api_->get_history_candles(
                symbol,
                okx_interval,    // 使用OKX格式的周期（大写）
                current_end,    // after（获取此时间之前的K线）
                0,              // before（不使用）
                1000            // limit（尝试1000，如果API拒绝会返回错误）
            );

            // 检查响应
            if (response.contains("code") && response["code"].get<std::string>() != "0") {
                std::string error_msg = response.contains("msg") ? response["msg"].get<std::string>() : "未知错误";
                std::cerr << "[OKXFetcher] API错误: " << error_msg << std::endl;
                std::cerr << "[OKXFetcher] 完整响应: " << response.dump() << std::endl;
                break;
            }

            if (!response.contains("data") || !response["data"].is_array()) {
                std::cerr << "[OKXFetcher] 响应格式错误" << std::endl;
                std::cerr << "[OKXFetcher] 完整响应: " << response.dump() << std::endl;
                break;
            }

            auto data = response["data"];

            if (data.empty()) {
                retry_count++;
                std::cout << "[OKXFetcher] 没有更多数据 (重试 " << retry_count << "/" << max_retries << ")" << std::endl;
                if (retry_count >= max_retries) {
                    std::cout << "[OKXFetcher] 连续 " << max_retries << " 次返回空数据，可能合约未上线或数据不存在，停止拉取" << std::endl;
                    break;
                }
                // 尝试向前移动时间窗口（移动到start_ts附近重试）
                // 如果当前位置距离start_ts还很远，直接跳到start_ts + 100个interval的位置
                int64_t distance_to_start = current_end - start_ts;
                if (distance_to_start > interval_ms * 200) {
                    // 距离很远，直接跳到start_ts附近
                    current_end = start_ts + interval_ms * 100;
                    std::cout << "[OKXFetcher] 距离起始时间较远，跳转到 " << kline_utils::format_timestamp(current_end) << std::endl;
                } else {
                    // 距离较近，小步移动
                    current_end -= interval_ms * 10;
                }
                continue;
            }

            // 重置重试计数器
            retry_count = 0;

            // 解析K线数据（OKX返回的数据是降序：从新到旧）
            int batch_count = 0;
            std::cout << "[OKXFetcher] API返回 " << data.size() << " 根K线" << std::endl;

            // 打印第一根K线的完整数据以了解格式
            if (!data.empty() && total_fetched == 0) {
                std::cout << "[OKXFetcher] 第一根K线原始数据: " << data[0].dump() << std::endl;
            }

            int64_t batch_min_ts = current_end;  // 记录本批最小时间戳

            for (const auto& candle : data) {
                kline_utils::Kline kline = kline_utils::parse_okx_candle(candle);

                // 调试：打印前3根K线的时间戳
                if (batch_count < 3) {
                    std::cout << "[OKXFetcher]   K线时间: " << kline_utils::format_timestamp(kline.timestamp)
                              << " (范围: " << kline_utils::format_timestamp(start_ts)
                              << " ~ " << kline_utils::format_timestamp(end_ts) << ")" << std::endl;
                }

                // 只保留在时间范围内的K线
                if (kline.timestamp >= start_ts && kline.timestamp <= end_ts) {
                    klines.push_back(kline);
                    batch_count++;
                }

                // 记录最小时间戳（因为数据是降序，最后一个就是最小的）
                if (kline.timestamp < batch_min_ts) {
                    batch_min_ts = kline.timestamp;
                }
            }

            total_fetched += batch_count;
            std::cout << "[OKXFetcher] 本批拉取 " << batch_count << " 根，累计 " << total_fetched << " 根" << std::endl;

            // 更新current_end为最小时间戳（继续向前拉取更早的数据）
            if (batch_min_ts < min_ts) {
                min_ts = batch_min_ts;
                current_end = batch_min_ts;
            } else {
                // 如果没有更早的数据了，停止
                std::cout << "[OKXFetcher] 已到达数据起点，停止拉取" << std::endl;
                break;
            }

            // 如果已经拉取到开始时间之前，停止
            if (current_end <= start_ts) {
                break;
            }

            // 避免触发限速（OKX: 20次/2秒，建议100ms间隔）
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        } catch (const std::exception& e) {
            std::cerr << "[OKXFetcher] 拉取失败: " << e.what() << std::endl;
            break;
        }
    }

    // OKX返回的数据是降序，需要排序为升序
    std::sort(klines.begin(), klines.end(),
              [](const kline_utils::Kline& a, const kline_utils::Kline& b) {
                  return a.timestamp < b.timestamp;
              });

    std::cout << "[OKXFetcher] 拉取完成，共 " << klines.size() << " 根K线" << std::endl;

    return klines;
}

// ==================== Binance历史数据拉取器 ====================

BinanceHistoricalFetcher::BinanceHistoricalFetcher(
    const std::string& api_key,
    const std::string& secret_key,
    bool is_testnet
) {
    api_ = std::make_unique<binance::BinanceRestAPI>(
        api_key,
        secret_key,
        binance::MarketType::FUTURES,  // U本位合约
        is_testnet
    );
}

std::vector<kline_utils::Kline> BinanceHistoricalFetcher::fetch_history(
    const std::string& symbol,
    const std::string& interval,
    int64_t start_ts,
    int64_t end_ts
) {
    std::vector<kline_utils::Kline> klines;

    int64_t interval_ms = kline_utils::get_interval_milliseconds(interval);
    int64_t current_start = start_ts;

    std::cout << "[BinanceFetcher] 开始拉取 " << symbol << ":" << interval
              << " 从 " << kline_utils::format_timestamp(start_ts)
              << " 到 " << kline_utils::format_timestamp(end_ts) << std::endl;
    std::cout << "[BinanceFetcher] 时间戳: start=" << start_ts << ", end=" << end_ts << std::endl;

    int total_fetched = 0;
    int retry_count = 0;
    const int max_retries = 2;  // 最多重试2次空数据

    // Binance限制：每次最多1500根K线，从旧到新拉取
    // 注意：当start_ts == end_ts时，需要包含这个时间点
    while (current_start <= end_ts) {
        try {
            std::cout << "[BinanceFetcher] 请求参数: symbol=" << symbol
                      << ", interval=" << interval
                      << ", startTime=" << current_start
                      << ", endTime=" << end_ts
                      << ", limit=1500" << std::endl;

            // 调用Binance API
            nlohmann::json response = api_->get_klines(
                symbol,
                interval,
                current_start,
                end_ts,
                1500
            );

            std::cout << "[BinanceFetcher] API响应类型: " << (response.is_array() ? "array" : "other") << std::endl;
            if (response.is_array()) {
                std::cout << "[BinanceFetcher] API返回 " << response.size() << " 根K线" << std::endl;
            } else {
                std::cout << "[BinanceFetcher] API响应内容: " << response.dump() << std::endl;
            }

            if (!response.is_array()) {
                std::cerr << "[BinanceFetcher] 响应格式错误" << std::endl;
                std::cerr << "[BinanceFetcher] 完整响应: " << response.dump() << std::endl;
                break;
            }

            if (response.empty()) {
                retry_count++;
                std::cout << "[BinanceFetcher] 没有更多数据 (重试 " << retry_count << "/" << max_retries << ")" << std::endl;
                if (retry_count >= max_retries) {
                    std::cerr << "[BinanceFetcher] 连续 " << max_retries << " 次返回空数据，可能合约未上线或数据不存在，停止拉取" << std::endl;
                    break;
                }
                // 尝试向前移动时间窗口（但不要移动太多，避免跳过有效数据）
                current_start += interval_ms * 10;  // 减少移动步长，从100改为10
                continue;
            }

            // 重置重试计数器
            retry_count = 0;

            // 解析K线数据
            int batch_count = 0;
            int64_t max_timestamp = current_start;

            for (const auto& kline_data : response) {
                kline_utils::Kline kline = kline_utils::parse_binance_kline(kline_data);

                // 调试：打印前3根K线的时间戳
                if (batch_count < 3) {
                    std::cout << "[BinanceFetcher]   K线时间: " << kline_utils::format_timestamp(kline.timestamp)
                              << " (ts=" << kline.timestamp << ")"
                              << " (范围: " << start_ts << " ~ " << end_ts << ")" << std::endl;
                }

                // 只保留在时间范围内的K线
                if (kline.timestamp >= start_ts && kline.timestamp <= end_ts) {
                    klines.push_back(kline);
                    batch_count++;
                    std::cout << "[BinanceFetcher]   ✓ 添加K线: " << kline_utils::format_timestamp(kline.timestamp) << std::endl;
                } else {
                    std::cout << "[BinanceFetcher]   ✗ 跳过K线: " << kline_utils::format_timestamp(kline.timestamp)
                              << " (不在范围内)" << std::endl;
                }

                // 记录最大时间戳
                if (kline.timestamp > max_timestamp) {
                    max_timestamp = kline.timestamp;
                }
            }

            // 更新current_start为最大时间戳 + interval
            current_start = max_timestamp + interval_ms;

            total_fetched += batch_count;
            std::cout << "[BinanceFetcher] 本批拉取 " << batch_count << " 根，累计 " << total_fetched << " 根" << std::endl;
            std::cout << "[BinanceFetcher] 下次起始时间: " << current_start << " (" << kline_utils::format_timestamp(current_start) << ")" << std::endl;

            // 如果本批没有拉取到任何数据（可能是时间范围外或API返回空），增加重试计数
            if (batch_count == 0) {
                retry_count++;
                std::cout << "[BinanceFetcher] 本批无有效数据 (重试 " << retry_count << "/" << max_retries << ")" << std::endl;
                if (retry_count >= max_retries) {
                    std::cerr << "[BinanceFetcher] 连续 " << max_retries << " 次无有效数据，停止拉取" << std::endl;
                    break;
                }
                // 尝试向前移动时间窗口
                current_start += interval_ms * 100;
                continue;
            }

            // 重置重试计数器
            retry_count = 0;

            // 如果已经拉取到结束时间之后，停止
            if (current_start > end_ts) {
                break;
            }

            // 避免触发限速（Binance: 1200次/分钟，建议50ms间隔）
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        } catch (const std::exception& e) {
            std::cerr << "[BinanceFetcher] 拉取失败: " << e.what() << std::endl;
            break;
        }
    }

    std::cout << "[BinanceFetcher] 拉取完成，共 " << klines.size() << " 根K线" << std::endl;

    return klines;
}

} // namespace historical_fetcher
} // namespace trading
