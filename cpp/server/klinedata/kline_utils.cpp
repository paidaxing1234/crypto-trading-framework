#include "kline_utils.h"
#include <sstream>
#include <iomanip>
#include <ctime>

namespace trading {
namespace kline_utils {

nlohmann::json Kline::to_json() const {
    nlohmann::json j = {
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

int64_t get_interval_milliseconds(const std::string& interval) {
    if (interval == "1s") return 1000;
    if (interval == "1m") return 60 * 1000;
    if (interval == "5m") return 5 * 60 * 1000;
    if (interval == "15m") return 15 * 60 * 1000;
    if (interval == "30m") return 30 * 60 * 1000;
    if (interval == "1H" || interval == "1h") return 60 * 60 * 1000;
    if (interval == "4H" || interval == "4h") return 4 * 60 * 60 * 1000;
    if (interval == "1D" || interval == "1d") return 24 * 60 * 60 * 1000;

    // 默认返回1分钟
    return 60 * 1000;
}

int64_t align_timestamp(int64_t timestamp, int64_t period_ms) {
    return (timestamp / period_ms) * period_ms;
}

Kline parse_okx_candle(const nlohmann::json& candle_data) {
    Kline kline;

    // OKX格式: [timestamp, open, high, low, close, volume, volumeCcy, volumeCcyQuote, confirm]
    kline.timestamp = std::stoll(candle_data[0].get<std::string>());
    kline.open = std::stod(candle_data[1].get<std::string>());
    kline.high = std::stod(candle_data[2].get<std::string>());
    kline.low = std::stod(candle_data[3].get<std::string>());
    kline.close = std::stod(candle_data[4].get<std::string>());
    kline.volume = std::stod(candle_data[5].get<std::string>());

    return kline;
}

Kline parse_binance_kline(const nlohmann::json& kline_data) {
    Kline kline;

    // Binance格式: [openTime, open, high, low, close, volume, closeTime, quoteAssetVol, trades, takerBuyBaseVol, takerBuyQuoteVol, ...]
    kline.timestamp = kline_data[0].get<int64_t>();
    kline.open = std::stod(kline_data[1].get<std::string>());
    kline.high = std::stod(kline_data[2].get<std::string>());
    kline.low = std::stod(kline_data[3].get<std::string>());
    kline.close = std::stod(kline_data[4].get<std::string>());
    kline.volume = std::stod(kline_data[5].get<std::string>());
    kline.trades = 0;
    if (kline_data.size() > 10) {
        kline.amount = std::stod(kline_data[7].get<std::string>());
        kline.buy_amount = std::stod(kline_data[10].get<std::string>());
        // 索引 8 是 number_of_trades, REST 返回的是数字而非字符串
        if (kline_data[8].is_number()) {
            kline.trades = kline_data[8].get<int64_t>();
        } else if (kline_data[8].is_string()) {
            kline.trades = std::stoll(kline_data[8].get<std::string>());
        }
    }

    return kline;
}

std::string format_timestamp(int64_t timestamp_ms) {
    time_t seconds = timestamp_ms / 1000;
    struct tm* timeinfo = localtime(&seconds);

    std::ostringstream oss;
    oss << std::put_time(timeinfo, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

bool is_usdt_contract(const std::string& exchange, const std::string& symbol) {
    if (exchange == "okx") {
        return symbol.find("-USDT-SWAP") != std::string::npos;
    } else if (exchange == "binance") {
        return symbol.length() > 4 && symbol.substr(symbol.length() - 4) == "USDT";
    }
    return false;
}

} // namespace kline_utils
} // namespace trading
