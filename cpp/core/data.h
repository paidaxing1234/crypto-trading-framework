#pragma once

#include "event.h"
#include <string>
#include <memory>
#include <vector>
#include <optional>
#include <cstdio>

namespace trading {

/**
 * @brief 数据事件基类
 * 
 * 用于封装各类市场数据：
 * - ticker: 行情数据
 * - trades: 成交数据
 * - orderbook: 订单簿数据
 * - kline: K线数据
 */
class Data : public Event {
public:
    using Ptr = std::shared_ptr<Data>;
    
    Data(
        const std::string& name,
        const std::string& symbol = "",
        const std::string& exchange = "okx"
    ) 
        : name_(name)
        , symbol_(symbol)
        , exchange_(exchange) {
    }
    
    virtual ~Data() noexcept = default;
    
    virtual std::string type_name() const override {
        return "Data";
    }
    
    const std::string& name() const { return name_; }
    const std::string& symbol() const { return symbol_; }
    const std::string& exchange() const { return exchange_; }

protected:
    std::string name_;      // 数据类型名称
    std::string symbol_;    // 交易对
    std::string exchange_;  // 交易所
};

/**
 * @brief 行情快照数据
 * 
 * 包含最新价、买卖价、成交量等
 */
class TickerData : public Data {
public:
    using Ptr = std::shared_ptr<TickerData>;
    
    TickerData(
        const std::string& symbol,
        double last_price,
        const std::string& exchange = "okx"
    ) 
        : Data("ticker", symbol, exchange)
        , last_price_(last_price) {
    }
    
    virtual ~TickerData() noexcept override = default;
    
    virtual std::string type_name() const override {
        return "TickerData";
    }
    
    // Getters
    double last_price() const { return last_price_; }
    std::optional<double> bid_price() const { return bid_price_; }
    std::optional<double> ask_price() const { return ask_price_; }
    std::optional<double> bid_size() const { return bid_size_; }
    std::optional<double> ask_size() const { return ask_size_; }
    std::optional<double> volume_24h() const { return volume_24h_; }
    std::optional<double> high_24h() const { return high_24h_; }
    std::optional<double> low_24h() const { return low_24h_; }
    std::optional<double> open_24h() const { return open_24h_; }
    
    // Setters
    void set_bid_price(double price) { bid_price_ = price; }
    void set_ask_price(double price) { ask_price_ = price; }
    void set_bid_size(double size) { bid_size_ = size; }
    void set_ask_size(double size) { ask_size_ = size; }
    void set_volume_24h(double volume) { volume_24h_ = volume; }
    void set_high_24h(double high) { high_24h_ = high; }
    void set_low_24h(double low) { low_24h_ = low; }
    void set_open_24h(double open) { open_24h_ = open; }
    
    // 计算属性
    std::optional<double> mid_price() const {
        if (bid_price_ && ask_price_) {
            return (*bid_price_ + *ask_price_) / 2.0;
        }
        return last_price_;
    }
    
    std::optional<double> spread() const {
        if (bid_price_ && ask_price_) {
            return *ask_price_ - *bid_price_;
        }
        return std::nullopt;
    }
    
    std::string to_string() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "TickerData(symbol=%s, last=%.2f, bid=%.2f, ask=%.2f, ts=%lld)",
            symbol_.c_str(),
            last_price_,
            bid_price_.value_or(0.0),
            ask_price_.value_or(0.0),
            (long long)timestamp_
        );
        return std::string(buf);
    }

private:
    double last_price_;                  // 最新价
    std::optional<double> bid_price_;    // 买一价
    std::optional<double> ask_price_;    // 卖一价
    std::optional<double> bid_size_;     // 买一量
    std::optional<double> ask_size_;     // 卖一量
    std::optional<double> volume_24h_;   // 24小时成交量
    std::optional<double> high_24h_;     // 24小时最高价
    std::optional<double> low_24h_;      // 24小时最低价
    std::optional<double> open_24h_;     // 24小时开盘价
};

/**
 * @brief 逐笔成交数据
 */
class TradeData : public Data {
public:
    using Ptr = std::shared_ptr<TradeData>;
    
    TradeData(
        const std::string& symbol,
        const std::string& trade_id,
        double price,
        double quantity,
        const std::string& exchange = "okx"
    ) 
        : Data("trade", symbol, exchange)
        , trade_id_(trade_id)
        , price_(price)
        , quantity_(quantity) {
    }
    
    virtual ~TradeData() noexcept override = default;
    
    virtual std::string type_name() const override {
        return "TradeData";
    }
    
    // Getters
    const std::string& trade_id() const { return trade_id_; }
    double price() const { return price_; }
    double quantity() const { return quantity_; }
    std::optional<std::string> side() const { return side_; }
    std::optional<bool> is_buyer_maker() const { return is_buyer_maker_; }
    
    // Setters
    void set_side(const std::string& side) { side_ = side; }
    void set_is_buyer_maker(bool is_maker) { is_buyer_maker_ = is_maker; }
    
    std::string to_string() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "TradeData(symbol=%s, price=%.2f, qty=%.4f, side=%s, ts=%lld)",
            symbol_.c_str(),
            price_,
            quantity_,
            side_.value_or("").c_str(),
            (long long)timestamp_
        );
        return std::string(buf);
    }

private:
    std::string trade_id_;                // 成交ID
    double price_;                        // 成交价
    double quantity_;                     // 成交量
    std::optional<std::string> side_;     // 成交方向
    std::optional<bool> is_buyer_maker_;  // 是否买方为Maker
};

/**
 * @brief 订单簿数据
 */
class OrderBookData : public Data {
public:
    using Ptr = std::shared_ptr<OrderBookData>;
    using PriceLevel = std::pair<double, double>;  // (price, size)
    
    OrderBookData(
        const std::string& symbol,
        const std::vector<PriceLevel>& bids,
        const std::vector<PriceLevel>& asks,
        const std::string& exchange = "okx"
    ) 
        : Data("orderbook", symbol, exchange)
        , bids_(bids)
        , asks_(asks) {
    }
    
    virtual ~OrderBookData() noexcept override = default;
    
    virtual std::string type_name() const override {
        return "OrderBookData";
    }
    
    // Getters
    const std::vector<PriceLevel>& bids() const { return bids_; }
    const std::vector<PriceLevel>& asks() const { return asks_; }
    
    // 最优买卖价
    std::optional<PriceLevel> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<PriceLevel>(bids_[0]);
    }
    
    std::optional<PriceLevel> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<PriceLevel>(asks_[0]);
    }
    
    // 中间价
    std::optional<double> mid_price() const {
        auto bid = best_bid();
        auto ask = best_ask();
        if (bid && ask) {
            return (bid->first + ask->first) / 2.0;
        }
        return std::nullopt;
    }
    
    // 价差
    std::optional<double> spread() const {
        auto bid = best_bid();
        auto ask = best_ask();
        if (bid && ask) {
            return ask->first - bid->first;
        }
        return std::nullopt;
    }
    
    std::string to_string() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "OrderBookData(symbol=%s, bids_depth=%zu, asks_depth=%zu, ts=%lld)",
            symbol_.c_str(),
            bids_.size(),
            asks_.size(),
            (long long)timestamp_
        );
        return std::string(buf);
    }

private:
    std::vector<PriceLevel> bids_;  // 买盘 [(price, size), ...] 按价格从高到低
    std::vector<PriceLevel> asks_;  // 卖盘 [(price, size), ...] 按价格从低到高
};

/**
 * @brief K线数据
 */
class KlineData : public Data {
public:
    using Ptr = std::shared_ptr<KlineData>;
    
    KlineData(
        const std::string& symbol,
        const std::string& interval,
        double open,
        double high,
        double low,
        double close,
        double volume,
        const std::string& exchange = "okx"
    ) 
        : Data("kline", symbol, exchange)
        , interval_(interval)
        , open_(open)
        , high_(high)
        , low_(low)
        , close_(close)
        , volume_(volume) {
    }
    
    virtual ~KlineData() noexcept override = default;
    
    virtual std::string type_name() const override {
        return "KlineData";
    }
    
    // Getters
    const std::string& interval() const { return interval_; }
    double open() const { return open_; }
    double high() const { return high_; }
    double low() const { return low_; }
    double close() const { return close_; }
    double volume() const { return volume_; }
    std::optional<double> turnover() const { return turnover_; }
    bool is_confirmed() const { return confirm_; }  // K线是否已完结
    
    // Setter
    void set_turnover(double turnover) { turnover_ = turnover; }
    void set_confirmed(bool confirmed) { confirm_ = confirmed; }
    
    std::string to_string() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "KlineData(symbol=%s, interval=%s, O=%.2f, H=%.2f, L=%.2f, C=%.2f, V=%.2f, ts=%lld)",
            symbol_.c_str(),
            interval_.c_str(),
            open_,
            high_,
            low_,
            close_,
            volume_,
            (long long)timestamp_
        );
        return std::string(buf);
    }

private:
    std::string interval_;           // 时间间隔（如 '1m', '5m', '1h'）
    double open_;                    // 开盘价
    double high_;                    // 最高价
    double low_;                     // 最低价
    double close_;                   // 收盘价
    double volume_;                  // 成交量
    std::optional<double> turnover_; // 成交额
    bool confirm_ = false;           // K线是否已完结（0:未完结, 1:已完结）
};

} // namespace trading

