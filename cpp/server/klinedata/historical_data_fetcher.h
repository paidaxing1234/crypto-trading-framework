#pragma once

#include <string>
#include <vector>
#include <memory>
#include "kline_utils.h"
#include "../../adapters/okx/okx_rest_api.h"
#include "../../adapters/binance/binance_rest_api.h"

namespace trading {
namespace historical_fetcher {

/**
 * @brief 历史数据拉取器基类
 */
class HistoricalDataFetcher {
public:
    virtual ~HistoricalDataFetcher() = default;

    /**
     * @brief 拉取历史K线数据
     *
     * @param symbol 交易对符号
     * @param interval K线周期
     * @param start_ts 开始时间戳（毫秒）
     * @param end_ts 结束时间戳（毫秒）
     * @return std::vector<kline_utils::Kline> K线数据列表
     */
    virtual std::vector<kline_utils::Kline> fetch_history(
        const std::string& symbol,
        const std::string& interval,
        int64_t start_ts,
        int64_t end_ts
    ) = 0;
};

/**
 * @brief OKX历史数据拉取器
 */
class OKXHistoricalFetcher : public HistoricalDataFetcher {
public:
    /**
     * @brief 构造函数
     *
     * @param api_key API密钥
     * @param secret_key 密钥
     * @param passphrase 口令
     * @param is_testnet 是否为测试网
     */
    OKXHistoricalFetcher(
        const std::string& api_key,
        const std::string& secret_key,
        const std::string& passphrase,
        bool is_testnet = false
    );

    std::vector<kline_utils::Kline> fetch_history(
        const std::string& symbol,
        const std::string& interval,
        int64_t start_ts,
        int64_t end_ts
    ) override;

private:
    std::unique_ptr<okx::OKXRestAPI> api_;
};

/**
 * @brief Binance历史数据拉取器
 */
class BinanceHistoricalFetcher : public HistoricalDataFetcher {
public:
    /**
     * @brief 构造函数
     *
     * @param api_key API密钥
     * @param secret_key 密钥
     * @param is_testnet 是否为测试网
     */
    BinanceHistoricalFetcher(
        const std::string& api_key,
        const std::string& secret_key,
        bool is_testnet = false
    );

    std::vector<kline_utils::Kline> fetch_history(
        const std::string& symbol,
        const std::string& interval,
        int64_t start_ts,
        int64_t end_ts
    ) override;

private:
    std::unique_ptr<binance::BinanceRestAPI> api_;
};

} // namespace historical_fetcher
} // namespace trading
