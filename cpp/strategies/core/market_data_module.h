/**
 * @file market_data_module.h
 * @brief 行情数据模块 - K线、Trades等行情数据的接收和存储
 * 
 * 功能:
 * 1. K线数据订阅/取消订阅
 * 2. K线数据存储（环形缓冲区，支持2小时数据）
 * 3. Trades数据订阅
 * 4. Ticker数据订阅（预留）
 * 5. OrderBook数据订阅（预留）
 * 
 * @author Sequence Team
 * @date 2025-12
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <functional>
#include <chrono>

#include <zmq.hpp>
#include <nlohmann/json.hpp>

namespace trading {

// ============================================================
// K线数据结构
// ============================================================

/**
 * @brief 单根 K 线数据
 */
struct KlineBar {
    int64_t timestamp;     // 毫秒时间戳
    double open;
    double high;
    double low;
    double close;
    double volume;
    
    KlineBar() : timestamp(0), open(0), high(0), low(0), close(0), volume(0) {}
    KlineBar(int64_t ts, double o, double h, double l, double c, double v)
        : timestamp(ts), open(o), high(h), low(l), close(c), volume(v) {}
};

/**
 * @brief 逐笔成交数据
 */
struct TradeData {
    int64_t timestamp;     // 毫秒时间戳
    std::string trade_id;
    double price;
    double quantity;
    std::string side;      // "buy" or "sell"
    
    TradeData() : timestamp(0), price(0), quantity(0) {}
    TradeData(int64_t ts, const std::string& id, double p, double qty, const std::string& s)
        : timestamp(ts), trade_id(id), price(p), quantity(qty), side(s) {}
};

/**
 * @brief 深度数据（订单簿快照）
 */
struct OrderBookSnapshot {
    int64_t timestamp;     // 毫秒时间戳
    std::vector<std::pair<double, double>> bids;  // [(price, size), ...] 按价格从高到低
    std::vector<std::pair<double, double>> asks;  // [(price, size), ...] 按价格从低到高
    double best_bid_price;  // 最优买价
    double best_bid_size;   // 最优买量
    double best_ask_price;  // 最优卖价
    double best_ask_size;   // 最优卖量
    double mid_price;       // 中间价
    double spread;          // 价差
    
    OrderBookSnapshot() : timestamp(0), best_bid_price(0), best_bid_size(0),
                          best_ask_price(0), best_ask_size(0), mid_price(0), spread(0) {}
};

/**
 * @brief 资金费率数据
 */
struct FundingRateData {
    int64_t timestamp;          // 数据更新时间（毫秒）
    double funding_rate;        // 当前资金费率
    double next_funding_rate;    // 下一期预测资金费率
    int64_t funding_time;       // 资金费时间（毫秒）
    int64_t next_funding_time;   // 下一期资金费时间（毫秒）
    double min_funding_rate;    // 资金费率下限
    double max_funding_rate;    // 资金费率上限
    double interest_rate;        // 利率
    double impact_value;         // 深度加权金额
    double premium;              // 溢价指数
    double sett_funding_rate;    // 结算资金费率
    std::string method;          // 资金费收取逻辑
    std::string formula_type;   // 公式类型
    std::string sett_state;      // 结算状态
    
    FundingRateData() : timestamp(0), funding_rate(0), next_funding_rate(0),
                         funding_time(0), next_funding_time(0), min_funding_rate(0),
                         max_funding_rate(0), interest_rate(0), impact_value(0),
                         premium(0), sett_funding_rate(0) {}
};


// ============================================================
// K线缓冲区（环形缓冲区）
// ============================================================

/**
 * @brief 单个币种的 K 线缓冲区
 */
class KlineBuffer {
public:
    explicit KlineBuffer(size_t max_bars = 7200)
        : max_bars_(max_bars)
        , data_(max_bars)
        , head_(0)
        , size_(0) {}
    
    /**
     * @brief 更新 K 线数据
     * @return true 如果追加了新 K 线, false 如果更新了现有 K 线
     */
    bool update(int64_t timestamp, double open, double high, 
                double low, double close, double volume) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (size_ == 0) {
            data_[0] = KlineBar(timestamp, open, high, low, close, volume);
            size_ = 1;
            return true;
        }
        
        size_t last_idx = (head_ + size_ - 1) % max_bars_;
        
        if (data_[last_idx].timestamp == timestamp) {
            data_[last_idx] = KlineBar(timestamp, open, high, low, close, volume);
            return false;
        } else {
            if (size_ < max_bars_) {
                size_t new_idx = (head_ + size_) % max_bars_;
                data_[new_idx] = KlineBar(timestamp, open, high, low, close, volume);
                size_++;
            } else {
                data_[head_] = KlineBar(timestamp, open, high, low, close, volume);
                head_ = (head_ + 1) % max_bars_;
            }
            return true;
        }
    }
    
    std::vector<KlineBar> get_all() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<KlineBar> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_bars_]);
        }
        return result;
    }
    
    std::vector<double> get_closes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<double> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_bars_].close);
        }
        return result;
    }
    
    std::vector<double> get_opens() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<double> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_bars_].open);
        }
        return result;
    }
    
    std::vector<double> get_highs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<double> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_bars_].high);
        }
        return result;
    }
    
    std::vector<double> get_lows() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<double> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_bars_].low);
        }
        return result;
    }
    
    std::vector<double> get_volumes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<double> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_bars_].volume);
        }
        return result;
    }
    
    std::vector<int64_t> get_timestamps() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int64_t> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_bars_].timestamp);
        }
        return result;
    }
    
    bool get_last(KlineBar& bar) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return false;
        bar = data_[(head_ + size_ - 1) % max_bars_];
        return true;
    }
    
    bool get_at(size_t index, KlineBar& bar) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= size_) return false;
        bar = data_[(head_ + index) % max_bars_];
        return true;
    }
    
    std::vector<KlineBar> get_recent(size_t n) const {
        std::lock_guard<std::mutex> lock(mutex_);
        n = std::min(n, size_);
        std::vector<KlineBar> result;
        result.reserve(n);
        for (size_t i = size_ - n; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_bars_]);
        }
        return result;
    }
    
    size_t size() const { 
        std::lock_guard<std::mutex> lock(mutex_);
        return size_; 
    }
    
    size_t max_size() const { return max_bars_; }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        size_ = 0;
    }

private:
    size_t max_bars_;
    std::vector<KlineBar> data_;
    size_t head_;
    size_t size_;
    mutable std::mutex mutex_;
};

// ============================================================
// Trades 缓冲区（环形缓冲区）
// ============================================================

/**
 * @brief 单个币种的 Trades 缓冲区
 */
class TradeBuffer {
public:
    explicit TradeBuffer(size_t max_trades = 10000)
        : max_trades_(max_trades)
        , data_(max_trades)
        , head_(0)
        , size_(0) {}
    
    /**
     * @brief 添加新的成交数据
     */
    void add(int64_t timestamp, const std::string& trade_id,
             double price, double quantity, const std::string& side) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (size_ < max_trades_) {
            size_t new_idx = (head_ + size_) % max_trades_;
            data_[new_idx] = TradeData(timestamp, trade_id, price, quantity, side);
            size_++;
        } else {
            data_[head_] = TradeData(timestamp, trade_id, price, quantity, side);
            head_ = (head_ + 1) % max_trades_;
        }
    }
    
    std::vector<TradeData> get_all() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TradeData> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_trades_]);
        }
        return result;
    }
    
    /**
     * @brief 获取最近 N 条成交
     */
    std::vector<TradeData> get_recent(size_t n) const {
        std::lock_guard<std::mutex> lock(mutex_);
        n = std::min(n, size_);
        std::vector<TradeData> result;
        result.reserve(n);
        for (size_t i = size_ - n; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_trades_]);
        }
        return result;
    }
    
    /**
     * @brief 获取最近 N 毫秒内的成交
     */
    std::vector<TradeData> get_recent_by_time(int64_t time_ms) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return {};
        
        int64_t now = current_timestamp_ms();
        int64_t cutoff = now - time_ms;
        
        std::vector<TradeData> result;
        for (size_t i = 0; i < size_; ++i) {
            size_t idx = (head_ + i) % max_trades_;
            if (data_[idx].timestamp >= cutoff) {
                result.push_back(data_[idx]);
            }
        }
        return result;
    }
    
    bool get_last(TradeData& trade) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return false;
        trade = data_[(head_ + size_ - 1) % max_trades_];
        return true;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        size_ = 0;
    }
    
    static int64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

private:
    size_t max_trades_;
    std::vector<TradeData> data_;
    size_t head_;
    size_t size_;
    mutable std::mutex mutex_;
};

// ============================================================
// OrderBook 缓冲区（环形缓冲区）
// ============================================================

/**
 * @brief 单个币种的 OrderBook 缓冲区
 */
class OrderBookBuffer {
public:
    explicit OrderBookBuffer(size_t max_snapshots = 1000)
        : max_snapshots_(max_snapshots)
        , data_(max_snapshots)
        , head_(0)
        , size_(0) {}
    
    /**
     * @brief 添加新的深度快照
     */
    void add(int64_t timestamp, const std::vector<std::pair<double, double>>& bids,
             const std::vector<std::pair<double, double>>& asks,
             double best_bid_price, double best_bid_size,
             double best_ask_price, double best_ask_size,
             double mid_price, double spread) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        OrderBookSnapshot snapshot;
        snapshot.timestamp = timestamp;
        snapshot.bids = bids;
        snapshot.asks = asks;
        snapshot.best_bid_price = best_bid_price;
        snapshot.best_bid_size = best_bid_size;
        snapshot.best_ask_price = best_ask_price;
        snapshot.best_ask_size = best_ask_size;
        snapshot.mid_price = mid_price;
        snapshot.spread = spread;
        
        if (size_ < max_snapshots_) {
            size_t new_idx = (head_ + size_) % max_snapshots_;
            data_[new_idx] = snapshot;
            size_++;
        } else {
            data_[head_] = snapshot;
            head_ = (head_ + 1) % max_snapshots_;
        }
    }
    
    std::vector<OrderBookSnapshot> get_all() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<OrderBookSnapshot> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_snapshots_]);
        }
        return result;
    }
    
    /**
     * @brief 获取最近 N 个快照
     */
    std::vector<OrderBookSnapshot> get_recent(size_t n) const {
        std::lock_guard<std::mutex> lock(mutex_);
        n = std::min(n, size_);
        std::vector<OrderBookSnapshot> result;
        result.reserve(n);
        for (size_t i = size_ - n; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_snapshots_]);
        }
        return result;
    }
    
    /**
     * @brief 获取最近 N 毫秒内的快照
     */
    std::vector<OrderBookSnapshot> get_recent_by_time(int64_t time_ms) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return {};
        
        int64_t now = current_timestamp_ms();
        int64_t cutoff = now - time_ms;
        
        std::vector<OrderBookSnapshot> result;
        for (size_t i = 0; i < size_; ++i) {
            size_t idx = (head_ + i) % max_snapshots_;
            if (data_[idx].timestamp >= cutoff) {
                result.push_back(data_[idx]);
            }
        }
        return result;
    }
    
    bool get_last(OrderBookSnapshot& snapshot) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return false;
        snapshot = data_[(head_ + size_ - 1) % max_snapshots_];
        return true;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        size_ = 0;
    }
    
    static int64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

private:
    size_t max_snapshots_;
    std::vector<OrderBookSnapshot> data_;
    size_t head_;
    size_t size_;
    mutable std::mutex mutex_;
};

// ============================================================
// FundingRate 缓冲区（环形缓冲区）
// ============================================================

/**
 * @brief 单个币种的 FundingRate 缓冲区
 */
class FundingRateBuffer {
public:
    explicit FundingRateBuffer(size_t max_records = 100)
        : max_records_(max_records)
        , data_(max_records)
        , head_(0)
        , size_(0) {}
    
    /**
     * @brief 添加新的资金费率数据
     */
    void add(const FundingRateData& fr) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (size_ < max_records_) {
            size_t new_idx = (head_ + size_) % max_records_;
            data_[new_idx] = fr;
            size_++;
        } else {
            data_[head_] = fr;
            head_ = (head_ + 1) % max_records_;
        }
    }
    
    std::vector<FundingRateData> get_all() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<FundingRateData> result;
        result.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_records_]);
        }
        return result;
    }
    
    /**
     * @brief 获取最近 N 条记录
     */
    std::vector<FundingRateData> get_recent(size_t n) const {
        std::lock_guard<std::mutex> lock(mutex_);
        n = std::min(n, size_);
        std::vector<FundingRateData> result;
        result.reserve(n);
        for (size_t i = size_ - n; i < size_; ++i) {
            result.push_back(data_[(head_ + i) % max_records_]);
        }
        return result;
    }
    
    /**
     * @brief 获取最近 N 毫秒内的记录
     */
    std::vector<FundingRateData> get_recent_by_time(int64_t time_ms) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return {};
        
        int64_t now = current_timestamp_ms();
        int64_t cutoff = now - time_ms;
        
        std::vector<FundingRateData> result;
        for (size_t i = 0; i < size_; ++i) {
            size_t idx = (head_ + i) % max_records_;
            if (data_[idx].timestamp >= cutoff) {
                result.push_back(data_[idx]);
            }
        }
        return result;
    }
    
    bool get_last(FundingRateData& fr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return false;
        fr = data_[(head_ + size_ - 1) % max_records_];
        return true;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        size_ = 0;
    }
    
    static int64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

private:
    size_t max_records_;
    std::vector<FundingRateData> data_;
    size_t head_;
    size_t size_;
    mutable std::mutex mutex_;
};


// ============================================================
// K线管理器（多币种）
// ============================================================

class KlineManager {
public:
    explicit KlineManager(size_t max_bars = 7200, const std::string& interval = "1s")
        : max_bars_(max_bars)
        , interval_(interval) {
        // 计算间隔毫秒数
        if (interval == "1s") interval_ms_ = 1000;
        else if (interval == "1m") interval_ms_ = 60000;
        else if (interval == "3m") interval_ms_ = 180000;
        else if (interval == "5m") interval_ms_ = 300000;
        else if (interval == "15m") interval_ms_ = 900000;
        else if (interval == "30m") interval_ms_ = 1800000;
        else if (interval == "1H") interval_ms_ = 3600000;
        else if (interval == "4H") interval_ms_ = 14400000;
        else if (interval == "1D") interval_ms_ = 86400000;
        else interval_ms_ = 60000;
    }
    
    bool update(const std::string& symbol, int64_t timestamp,
                double open, double high, double low, double close, double volume) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) {
            buffers_[symbol] = std::make_unique<KlineBuffer>(max_bars_);
            it = buffers_.find(symbol);
        }
        return it->second->update(timestamp, open, high, low, close, volume);
    }
    
    std::vector<KlineBar> get_all(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return {};
        return it->second->get_all();
    }
    
    std::vector<double> get_closes(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return {};
        return it->second->get_closes();
    }
    
    std::vector<double> get_opens(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return {};
        return it->second->get_opens();
    }
    
    std::vector<double> get_highs(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return {};
        return it->second->get_highs();
    }
    
    std::vector<double> get_lows(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return {};
        return it->second->get_lows();
    }
    
    std::vector<double> get_volumes(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return {};
        return it->second->get_volumes();
    }
    
    std::vector<int64_t> get_timestamps(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return {};
        return it->second->get_timestamps();
    }
    
    bool get_last(const std::string& symbol, KlineBar& bar) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return false;
        return it->second->get_last(bar);
    }
    
    std::vector<KlineBar> get_recent(const std::string& symbol, size_t n) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return {};
        return it->second->get_recent(n);
    }
    
    size_t get_bar_count(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffers_.find(symbol);
        if (it == buffers_.end()) return 0;
        return it->second->size();
    }
    
    std::vector<std::string> get_symbols() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        result.reserve(buffers_.size());
        for (const auto& pair : buffers_) {
            result.push_back(pair.first);
        }
        return result;
    }
    
    const std::string& interval() const { return interval_; }
    int64_t interval_ms() const { return interval_ms_; }
    size_t max_bars() const { return max_bars_; }

private:
    size_t max_bars_;
    std::string interval_;
    int64_t interval_ms_;
    std::map<std::string, std::unique_ptr<KlineBuffer>> buffers_;
    mutable std::mutex mutex_;
};


// ============================================================
// 行情数据模块
// ============================================================

/**
 * @brief 行情数据模块
 * 
 * 负责：
 * - 订阅/取消订阅K线、trades等行情数据
 * - 接收和存储行情数据
 * - 提供行情数据查询接口
 */
class MarketDataModule {
public:
    // K线回调类型
    using KlineCallback = std::function<void(const std::string&, const std::string&, const KlineBar&)>;
    // Trades回调类型
    using TradesCallback = std::function<void(const std::string&, const TradeData&)>;
    
    /**
     * @brief 构造函数
     * @param max_kline_bars K线最大存储数量（默认7200=2小时1s K线）
     * @param max_trades Trades最大存储数量（默认10000条）
     * @param max_orderbook_snapshots OrderBook最大存储数量（默认1000个快照）
     * @param max_funding_rate_records FundingRate最大存储数量（默认100条）
     */
    explicit MarketDataModule(size_t max_kline_bars = 7200,
                              size_t max_trades = 10000,
                              size_t max_orderbook_snapshots = 1000,
                              size_t max_funding_rate_records = 100)
        : max_kline_bars_(max_kline_bars)
        , max_trades_(max_trades)
        , max_orderbook_snapshots_(max_orderbook_snapshots)
        , max_funding_rate_records_(max_funding_rate_records)
        , kline_count_(0)
        , trade_count_(0)
        , orderbook_count_(0)
        , funding_rate_count_(0) {}
    
    // ==================== 初始化 ====================
    
    /**
     * @brief 设置 ZMQ socket（由策略基类调用）
     */
    void set_sockets(zmq::socket_t* market_sub, zmq::socket_t* subscribe_push) {
        market_sub_ = market_sub;
        subscribe_push_ = subscribe_push;
    }
    
    // ==================== 订阅管理 ====================
    
    /**
     * @brief 订阅 K 线数据
     */
    bool subscribe_kline(const std::string& symbol, const std::string& interval,
                        const std::string& strategy_id) {
        if (!subscribe_push_) {
            return false;
        }
        
        // 创建或更新 K 线管理器
        {
            std::lock_guard<std::mutex> lock(kline_managers_mutex_);
            if (kline_managers_.find(interval) == kline_managers_.end()) {
                kline_managers_[interval] = std::make_unique<KlineManager>(max_kline_bars_, interval);
            }
        }
        
        // 记录订阅
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            subscribed_klines_[symbol].insert(interval);
        }
        
        nlohmann::json request = {
            {"action", "subscribe"},
            {"channel", "kline"},
            {"symbol", symbol},
            {"interval", interval},
            {"strategy_id", strategy_id},
            {"timestamp", current_timestamp_ms()}
        };
        
        try {
            std::string msg = request.dump();
            subscribe_push_->send(zmq::buffer(msg), zmq::send_flags::none);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    /**
     * @brief 取消订阅 K 线数据
     */
    bool unsubscribe_kline(const std::string& symbol, const std::string& interval,
                          const std::string& strategy_id) {
        if (!subscribe_push_) return false;
        
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            auto it = subscribed_klines_.find(symbol);
            if (it != subscribed_klines_.end()) {
                it->second.erase(interval);
            }
        }
        
        nlohmann::json request = {
            {"action", "unsubscribe"},
            {"channel", "kline"},
            {"symbol", symbol},
            {"interval", interval},
            {"strategy_id", strategy_id},
            {"timestamp", current_timestamp_ms()}
        };
        
        try {
            std::string msg = request.dump();
            subscribe_push_->send(zmq::buffer(msg), zmq::send_flags::none);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    /**
     * @brief 订阅 Trades 数据
     */
    bool subscribe_trades(const std::string& symbol, const std::string& strategy_id) {
        if (!subscribe_push_) return false;
        
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            subscribed_trades_.insert(symbol);
        }
        
        nlohmann::json request = {
            {"action", "subscribe"},
            {"channel", "trades"},
            {"symbol", symbol},
            {"strategy_id", strategy_id},
            {"timestamp", current_timestamp_ms()}
        };
        
        try {
            std::string msg = request.dump();
            subscribe_push_->send(zmq::buffer(msg), zmq::send_flags::none);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    /**
     * @brief 取消订阅 Trades 数据
     */
    bool unsubscribe_trades(const std::string& symbol, const std::string& strategy_id) {
        if (!subscribe_push_) return false;
        
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            subscribed_trades_.erase(symbol);
        }
        
        nlohmann::json request = {
            {"action", "unsubscribe"},
            {"channel", "trades"},
            {"symbol", symbol},
            {"strategy_id", strategy_id},
            {"timestamp", current_timestamp_ms()}
        };
        
        try {
            std::string msg = request.dump();
            subscribe_push_->send(zmq::buffer(msg), zmq::send_flags::none);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    /**
     * @brief 订阅深度数据（OrderBook）
     * @param symbol 交易对
     * @param channel 深度频道类型: "books5"(默认), "books", "bbo-tbt", "books-l2-tbt", "books50-l2-tbt", "books-elp"
     * @param strategy_id 策略ID
     */
    bool subscribe_orderbook(const std::string& symbol, const std::string& channel,
                            const std::string& strategy_id) {
        if (!subscribe_push_) return false;
        
        // 创建或更新 OrderBook 缓冲区
        {
            std::lock_guard<std::mutex> lock(orderbook_buffers_mutex_);
            std::string key = symbol + "_" + channel;
            if (orderbook_buffers_.find(key) == orderbook_buffers_.end()) {
                orderbook_buffers_[key] = std::make_unique<OrderBookBuffer>(max_orderbook_snapshots_);
            }
        }
        
        // 记录订阅
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            subscribed_orderbooks_[symbol].insert(channel);
        }
        
        nlohmann::json request = {
            {"action", "subscribe"},
            {"channel", channel == "orderbook" ? "books5" : channel},
            {"symbol", symbol},
            {"strategy_id", strategy_id},
            {"timestamp", current_timestamp_ms()}
        };
        
        try {
            std::string msg = request.dump();
            subscribe_push_->send(zmq::buffer(msg), zmq::send_flags::none);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    /**
     * @brief 取消订阅深度数据
     */
    bool unsubscribe_orderbook(const std::string& symbol, const std::string& channel,
                              const std::string& strategy_id) {
        if (!subscribe_push_) return false;
        
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            auto it = subscribed_orderbooks_.find(symbol);
            if (it != subscribed_orderbooks_.end()) {
                it->second.erase(channel);
            }
        }
        
        nlohmann::json request = {
            {"action", "unsubscribe"},
            {"channel", channel == "orderbook" ? "books5" : channel},
            {"symbol", symbol},
            {"strategy_id", strategy_id},
            {"timestamp", current_timestamp_ms()}
        };
        
        try {
            std::string msg = request.dump();
            subscribe_push_->send(zmq::buffer(msg), zmq::send_flags::none);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    /**
     * @brief 订阅资金费率数据
     * @param symbol 交易对（如 BTC-USDT-SWAP）
     * @param strategy_id 策略ID
     */
    bool subscribe_funding_rate(const std::string& symbol, const std::string& strategy_id) {
        if (!subscribe_push_) return false;
        
        // 创建或更新 FundingRate 缓冲区
        {
            std::lock_guard<std::mutex> lock(funding_rate_buffers_mutex_);
            if (funding_rate_buffers_.find(symbol) == funding_rate_buffers_.end()) {
                funding_rate_buffers_[symbol] = std::make_unique<FundingRateBuffer>(max_funding_rate_records_);
            }
        }
        
        // 记录订阅
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            subscribed_funding_rates_.insert(symbol);
        }
        
        nlohmann::json request = {
            {"action", "subscribe"},
            {"channel", "funding_rate"},
            {"symbol", symbol},
            {"strategy_id", strategy_id},
            {"timestamp", current_timestamp_ms()}
        };
        
        try {
            std::string msg = request.dump();
            subscribe_push_->send(zmq::buffer(msg), zmq::send_flags::none);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    /**
     * @brief 取消订阅资金费率数据
     */
    bool unsubscribe_funding_rate(const std::string& symbol, const std::string& strategy_id) {
        if (!subscribe_push_) return false;
        
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            subscribed_funding_rates_.erase(symbol);
        }
        
        nlohmann::json request = {
            {"action", "unsubscribe"},
            {"channel", "funding_rate"},
            {"symbol", symbol},
            {"strategy_id", strategy_id},
            {"timestamp", current_timestamp_ms()}
        };
        
        try {
            std::string msg = request.dump();
            subscribe_push_->send(zmq::buffer(msg), zmq::send_flags::none);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    
    // ==================== 数据处理 ====================
    
    /**
     * @brief 处理行情数据（主循环调用）
     */
    void process_market_data() {
        if (!market_sub_) return;

        zmq::message_t message;
        while (market_sub_->recv(message, zmq::recv_flags::dontwait)) {
            try {
                std::string msg_str(static_cast<char*>(message.data()), message.size());

                // 消息格式: topic|json_data
                // 需要分离主题和JSON数据
                std::string json_str = msg_str;
                size_t pipe_pos = msg_str.find('|');
                if (pipe_pos != std::string::npos) {
                    // 有主题前缀，提取JSON部分
                    json_str = msg_str.substr(pipe_pos + 1);
                }

                auto data = nlohmann::json::parse(json_str);

                std::string msg_type = data.value("type", "");

                if (msg_type == "kline") {
                    handle_kline(data);
                } else if (msg_type == "trades" || msg_type == "trade") {
                    handle_trades(data);
                } else if (msg_type == "orderbook") {
                    handle_orderbook(data);
                } else if (msg_type == "funding_rate") {
                    handle_funding_rate(data);
                }

            } catch (const std::exception&) {
                // 忽略解析错误
            }
        }
    }
    
    // ==================== K线数据查询 ====================
    
    /**
     * @brief 获取所有 K 线数据
     */
    std::vector<KlineBar> get_klines(const std::string& symbol, const std::string& interval) const {
        std::lock_guard<std::mutex> lock(kline_managers_mutex_);
        auto it = kline_managers_.find(interval);
        if (it == kline_managers_.end()) return {};
        return it->second->get_all(symbol);
    }
    
    /**
     * @brief 获取收盘价数组
     */
    std::vector<double> get_closes(const std::string& symbol, const std::string& interval) const {
        std::lock_guard<std::mutex> lock(kline_managers_mutex_);
        auto it = kline_managers_.find(interval);
        if (it == kline_managers_.end()) return {};
        return it->second->get_closes(symbol);
    }
    
    /**
     * @brief 获取开盘价数组
     */
    std::vector<double> get_opens(const std::string& symbol, const std::string& interval) const {
        std::lock_guard<std::mutex> lock(kline_managers_mutex_);
        auto it = kline_managers_.find(interval);
        if (it == kline_managers_.end()) return {};
        return it->second->get_opens(symbol);
    }
    
    /**
     * @brief 获取最高价数组
     */
    std::vector<double> get_highs(const std::string& symbol, const std::string& interval) const {
        std::lock_guard<std::mutex> lock(kline_managers_mutex_);
        auto it = kline_managers_.find(interval);
        if (it == kline_managers_.end()) return {};
        return it->second->get_highs(symbol);
    }
    
    /**
     * @brief 获取最低价数组
     */
    std::vector<double> get_lows(const std::string& symbol, const std::string& interval) const {
        std::lock_guard<std::mutex> lock(kline_managers_mutex_);
        auto it = kline_managers_.find(interval);
        if (it == kline_managers_.end()) return {};
        return it->second->get_lows(symbol);
    }
    
    /**
     * @brief 获取成交量数组
     */
    std::vector<double> get_volumes(const std::string& symbol, const std::string& interval) const {
        std::lock_guard<std::mutex> lock(kline_managers_mutex_);
        auto it = kline_managers_.find(interval);
        if (it == kline_managers_.end()) return {};
        return it->second->get_volumes(symbol);
    }
    
    /**
     * @brief 获取最近 n 根 K 线
     */
    std::vector<KlineBar> get_recent_klines(const std::string& symbol, 
                                            const std::string& interval, 
                                            size_t n) const {
        std::lock_guard<std::mutex> lock(kline_managers_mutex_);
        auto it = kline_managers_.find(interval);
        if (it == kline_managers_.end()) return {};
        return it->second->get_recent(symbol, n);
    }
    
    /**
     * @brief 获取最后一根 K 线
     */
    bool get_last_kline(const std::string& symbol, const std::string& interval, KlineBar& bar) const {
        std::lock_guard<std::mutex> lock(kline_managers_mutex_);
        auto it = kline_managers_.find(interval);
        if (it == kline_managers_.end()) return false;
        return it->second->get_last(symbol, bar);
    }
    
    /**
     * @brief 获取 K 线数量
     */
    size_t get_kline_count(const std::string& symbol, const std::string& interval) const {
        std::lock_guard<std::mutex> lock(kline_managers_mutex_);
        auto it = kline_managers_.find(interval);
        if (it == kline_managers_.end()) return 0;
        return it->second->get_bar_count(symbol);
    }
    
    /**
     * @brief 获取已订阅的币种列表
     */
    std::vector<std::string> get_subscribed_symbols() const {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::vector<std::string> result;
        for (const auto& pair : subscribed_klines_) {
            result.push_back(pair.first);
        }
        return result;
    }
    
    // ==================== Trades 数据查询 ====================
    
    /**
     * @brief 获取所有成交数据
     */
    std::vector<TradeData> get_trades(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(trade_buffers_mutex_);
        auto it = trade_buffers_.find(symbol);
        if (it == trade_buffers_.end()) return {};
        return it->second->get_all();
    }
    
    /**
     * @brief 获取最近 N 条成交
     */
    std::vector<TradeData> get_recent_trades(const std::string& symbol, size_t n) const {
        std::lock_guard<std::mutex> lock(trade_buffers_mutex_);
        auto it = trade_buffers_.find(symbol);
        if (it == trade_buffers_.end()) return {};
        return it->second->get_recent(n);
    }
    
    /**
     * @brief 获取最近 N 毫秒内的成交
     */
    std::vector<TradeData> get_trades_by_time(const std::string& symbol, int64_t time_ms) const {
        std::lock_guard<std::mutex> lock(trade_buffers_mutex_);
        auto it = trade_buffers_.find(symbol);
        if (it == trade_buffers_.end()) return {};
        return it->second->get_recent_by_time(time_ms);
    }
    
    /**
     * @brief 获取最后一条成交
     */
    bool get_last_trade(const std::string& symbol, TradeData& trade) const {
        std::lock_guard<std::mutex> lock(trade_buffers_mutex_);
        auto it = trade_buffers_.find(symbol);
        if (it == trade_buffers_.end()) return false;
        return it->second->get_last(trade);
    }
    
    /**
     * @brief 获取成交数量
     */
    size_t get_trade_count(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(trade_buffers_mutex_);
        auto it = trade_buffers_.find(symbol);
        if (it == trade_buffers_.end()) return 0;
        return it->second->size();
    }
    
    // ==================== OrderBook 数据查询 ====================
    
    /**
     * @brief 获取所有深度快照
     */
    std::vector<OrderBookSnapshot> get_orderbooks(const std::string& symbol, 
                                                  const std::string& channel = "books5") const {
        std::lock_guard<std::mutex> lock(orderbook_buffers_mutex_);
        std::string key = symbol + "_" + channel;
        auto it = orderbook_buffers_.find(key);
        if (it == orderbook_buffers_.end()) return {};
        return it->second->get_all();
    }
    
    /**
     * @brief 获取最近 N 个快照
     */
    std::vector<OrderBookSnapshot> get_recent_orderbooks(const std::string& symbol, 
                                                         size_t n,
                                                         const std::string& channel = "books5") const {
        std::lock_guard<std::mutex> lock(orderbook_buffers_mutex_);
        std::string key = symbol + "_" + channel;
        auto it = orderbook_buffers_.find(key);
        if (it == orderbook_buffers_.end()) return {};
        return it->second->get_recent(n);
    }
    
    /**
     * @brief 获取最近 N 毫秒内的快照
     */
    std::vector<OrderBookSnapshot> get_orderbooks_by_time(const std::string& symbol,
                                                          int64_t time_ms,
                                                          const std::string& channel = "books5") const {
        std::lock_guard<std::mutex> lock(orderbook_buffers_mutex_);
        std::string key = symbol + "_" + channel;
        auto it = orderbook_buffers_.find(key);
        if (it == orderbook_buffers_.end()) return {};
        return it->second->get_recent_by_time(time_ms);
    }
    
    /**
     * @brief 获取最后一个快照
     */
    bool get_last_orderbook(const std::string& symbol, OrderBookSnapshot& snapshot,
                          const std::string& channel = "books5") const {
        std::lock_guard<std::mutex> lock(orderbook_buffers_mutex_);
        std::string key = symbol + "_" + channel;
        auto it = orderbook_buffers_.find(key);
        if (it == orderbook_buffers_.end()) return false;
        return it->second->get_last(snapshot);
    }
    
    /**
     * @brief 获取快照数量
     */
    size_t get_orderbook_count(const std::string& symbol, const std::string& channel = "books5") const {
        std::lock_guard<std::mutex> lock(orderbook_buffers_mutex_);
        std::string key = symbol + "_" + channel;
        auto it = orderbook_buffers_.find(key);
        if (it == orderbook_buffers_.end()) return 0;
        return it->second->size();
    }
    
    // ==================== FundingRate 数据查询 ====================
    
    /**
     * @brief 获取所有资金费率数据
     */
    std::vector<FundingRateData> get_funding_rates(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(funding_rate_buffers_mutex_);
        auto it = funding_rate_buffers_.find(symbol);
        if (it == funding_rate_buffers_.end()) return {};
        return it->second->get_all();
    }
    
    /**
     * @brief 获取最近 N 条记录
     */
    std::vector<FundingRateData> get_recent_funding_rates(const std::string& symbol, size_t n) const {
        std::lock_guard<std::mutex> lock(funding_rate_buffers_mutex_);
        auto it = funding_rate_buffers_.find(symbol);
        if (it == funding_rate_buffers_.end()) return {};
        return it->second->get_recent(n);
    }
    
    /**
     * @brief 获取最近 N 毫秒内的记录
     */
    std::vector<FundingRateData> get_funding_rates_by_time(const std::string& symbol, int64_t time_ms) const {
        std::lock_guard<std::mutex> lock(funding_rate_buffers_mutex_);
        auto it = funding_rate_buffers_.find(symbol);
        if (it == funding_rate_buffers_.end()) return {};
        return it->second->get_recent_by_time(time_ms);
    }
    
    /**
     * @brief 获取最后一条记录
     */
    bool get_last_funding_rate(const std::string& symbol, FundingRateData& fr) const {
        std::lock_guard<std::mutex> lock(funding_rate_buffers_mutex_);
        auto it = funding_rate_buffers_.find(symbol);
        if (it == funding_rate_buffers_.end()) return false;
        return it->second->get_last(fr);
    }
    
    /**
     * @brief 获取记录数量
     */
    size_t get_funding_rate_count(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(funding_rate_buffers_mutex_);
        auto it = funding_rate_buffers_.find(symbol);
        if (it == funding_rate_buffers_.end()) return 0;
        return it->second->size();
    }
    
    // ==================== 回调设置 ====================
    
    // OrderBook 回调类型
    using OrderBookCallback = std::function<void(const std::string&, const OrderBookSnapshot&)>;
    // FundingRate 回调类型
    using FundingRateCallback = std::function<void(const std::string&, const FundingRateData&)>;
    
    void set_kline_callback(KlineCallback callback) {
        kline_callback_ = std::move(callback);
    }
    
    void set_trades_callback(TradesCallback callback) {
        trades_callback_ = std::move(callback);
    }
    
    void set_orderbook_callback(OrderBookCallback callback) {
        orderbook_callback_ = std::move(callback);
    }
    
    void set_funding_rate_callback(FundingRateCallback callback) {
        funding_rate_callback_ = std::move(callback);
    }
    
    // ==================== 统计 ====================
    
    int64_t total_kline_count() const { return kline_count_.load(); }
    int64_t total_trade_count() const { return trade_count_.load(); }
    int64_t total_orderbook_count() const { return orderbook_count_.load(); }
    int64_t total_funding_rate_count() const { return funding_rate_count_.load(); }

private:
    void handle_kline(const nlohmann::json& data) {
        std::string symbol = data.value("symbol", "");
        std::string interval = data.value("interval", "");
        
        if (symbol.empty() || interval.empty()) return;
        
        // 检查是否订阅
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            auto it = subscribed_klines_.find(symbol);
            if (it == subscribed_klines_.end() || it->second.find(interval) == it->second.end()) {
                return;
            }
        }
        
        KlineBar bar;
        bar.timestamp = data.value("timestamp", 0LL);
        bar.open = data.value("open", 0.0);
        bar.high = data.value("high", 0.0);
        bar.low = data.value("low", 0.0);
        bar.close = data.value("close", 0.0);
        bar.volume = data.value("volume", 0.0);
        
        // 存储
        {
            std::lock_guard<std::mutex> lock(kline_managers_mutex_);
            auto it = kline_managers_.find(interval);
            if (it != kline_managers_.end()) {
                bool is_new = it->second->update(symbol, bar.timestamp, bar.open, bar.high, 
                                                  bar.low, bar.close, bar.volume);
                if (is_new) {
                    kline_count_++;
                }
            }
        }
        
        // 回调
        if (kline_callback_) {
            kline_callback_(symbol, interval, bar);
        }
    }
    
    void handle_trades(const nlohmann::json& data) {
        std::string symbol = data.value("symbol", "");
        
        // 检查是否订阅
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            if (subscribed_trades_.find(symbol) == subscribed_trades_.end()) {
                return;
            }
        }
        
        TradeData trade;
        trade.timestamp = data.value("timestamp", 0LL);
        trade.trade_id = data.value("trade_id", "");
        trade.price = data.value("price", 0.0);
        trade.quantity = data.value("quantity", 0.0);
        trade.side = data.value("side", "");
        
        // 存储
        {
            std::lock_guard<std::mutex> lock(trade_buffers_mutex_);
            auto it = trade_buffers_.find(symbol);
            if (it == trade_buffers_.end()) {
                trade_buffers_[symbol] = std::make_unique<TradeBuffer>(max_trades_);
                it = trade_buffers_.find(symbol);
            }
            it->second->add(trade.timestamp, trade.trade_id, trade.price, 
                          trade.quantity, trade.side);
            trade_count_++;
        }
        
        // 回调
        if (trades_callback_) {
            trades_callback_(symbol, trade);
        }
    }
    
    void handle_orderbook(const nlohmann::json& data) {
        std::string symbol = data.value("symbol", "");
        std::string channel = data.value("channel", "books5");  // 从消息中获取 channel
        
        // 检查是否订阅
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            auto it = subscribed_orderbooks_.find(symbol);
            if (it == subscribed_orderbooks_.end() || 
                it->second.find(channel) == it->second.end()) {
                return;
            }
        }
        
        // 解析 bids 和 asks
        std::vector<std::pair<double, double>> bids;
        std::vector<std::pair<double, double>> asks;
        
        if (data.contains("bids") && data["bids"].is_array()) {
            for (const auto& bid : data["bids"]) {
                if (bid.is_array() && bid.size() >= 2) {
                    double price = bid[0].is_number() ? bid[0].get<double>() : std::stod(bid[0].get<std::string>());
                    double size = bid[1].is_number() ? bid[1].get<double>() : std::stod(bid[1].get<std::string>());
                    if (size > 0) {
                        bids.emplace_back(price, size);
                    }
                }
            }
        }
        
        if (data.contains("asks") && data["asks"].is_array()) {
            for (const auto& ask : data["asks"]) {
                if (ask.is_array() && ask.size() >= 2) {
                    double price = ask[0].is_number() ? ask[0].get<double>() : std::stod(ask[0].get<std::string>());
                    double size = ask[1].is_number() ? ask[1].get<double>() : std::stod(ask[1].get<std::string>());
                    if (size > 0) {
                        asks.emplace_back(price, size);
                    }
                }
            }
        }
        
        // 从消息中获取最优买卖价（如果存在）
        double best_bid_price = data.value("best_bid_price", 0.0);
        double best_bid_size = data.value("best_bid_size", 0.0);
        double best_ask_price = data.value("best_ask_price", 0.0);
        double best_ask_size = data.value("best_ask_size", 0.0);
        double mid_price = data.value("mid_price", 0.0);
        double spread = data.value("spread", 0.0);
        
        // 如果没有从消息中获取到，则计算
        if (best_bid_price == 0 && !bids.empty()) {
            best_bid_price = bids[0].first;
            best_bid_size = bids[0].second;
        }
        if (best_ask_price == 0 && !asks.empty()) {
            best_ask_price = asks[0].first;
            best_ask_size = asks[0].second;
        }
        if (mid_price == 0 && best_bid_price > 0 && best_ask_price > 0) {
            mid_price = (best_bid_price + best_ask_price) / 2.0;
            spread = best_ask_price - best_bid_price;
        }
        
        int64_t timestamp = data.value("timestamp", current_timestamp_ms());
        
        // 存储（使用正确的 channel）
        {
            std::lock_guard<std::mutex> lock(orderbook_buffers_mutex_);
            std::string key = symbol + "_" + channel;
            auto it = orderbook_buffers_.find(key);
            if (it == orderbook_buffers_.end()) {
                orderbook_buffers_[key] = std::make_unique<OrderBookBuffer>(max_orderbook_snapshots_);
                it = orderbook_buffers_.find(key);
            }
            it->second->add(timestamp, bids, asks, best_bid_price, best_bid_size,
                           best_ask_price, best_ask_size, mid_price, spread);
            orderbook_count_++;
        }
        
        // 回调
        if (orderbook_callback_) {
            OrderBookSnapshot snapshot;
            snapshot.timestamp = timestamp;
            snapshot.bids = bids;
            snapshot.asks = asks;
            snapshot.best_bid_price = best_bid_price;
            snapshot.best_bid_size = best_bid_size;
            snapshot.best_ask_price = best_ask_price;
            snapshot.best_ask_size = best_ask_size;
            snapshot.mid_price = mid_price;
            snapshot.spread = spread;
            orderbook_callback_(symbol, snapshot);
        }
    }
    
    void handle_funding_rate(const nlohmann::json& data) {
        std::string symbol = data.value("symbol", "");
        
        // 检查是否订阅
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            if (subscribed_funding_rates_.find(symbol) == subscribed_funding_rates_.end()) {
                return;
            }
        }
        
        FundingRateData fr;
        fr.timestamp = data.value("timestamp", current_timestamp_ms());
        fr.funding_rate = data.value("funding_rate", 0.0);
        fr.next_funding_rate = data.value("next_funding_rate", 0.0);
        fr.funding_time = data.value("funding_time", 0LL);
        fr.next_funding_time = data.value("next_funding_time", 0LL);
        fr.min_funding_rate = data.value("min_funding_rate", 0.0);
        fr.max_funding_rate = data.value("max_funding_rate", 0.0);
        fr.interest_rate = data.value("interest_rate", 0.0);
        fr.impact_value = data.value("impact_value", 0.0);
        fr.premium = data.value("premium", 0.0);
        fr.sett_funding_rate = data.value("sett_funding_rate", 0.0);
        fr.method = data.value("method", "");
        fr.formula_type = data.value("formula_type", "");
        fr.sett_state = data.value("sett_state", "");
        
        // 存储
        {
            std::lock_guard<std::mutex> lock(funding_rate_buffers_mutex_);
            auto it = funding_rate_buffers_.find(symbol);
            if (it == funding_rate_buffers_.end()) {
                funding_rate_buffers_[symbol] = std::make_unique<FundingRateBuffer>(max_funding_rate_records_);
                it = funding_rate_buffers_.find(symbol);
            }
            it->second->add(fr);
            funding_rate_count_++;
        }
        
        // 回调
        if (funding_rate_callback_) {
            funding_rate_callback_(symbol, fr);
        }
    }
    
    static int64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

private:
    size_t max_kline_bars_;
    size_t max_trades_;
    size_t max_orderbook_snapshots_;
    size_t max_funding_rate_records_;
    
    // ZMQ sockets（由策略基类设置）
    zmq::socket_t* market_sub_ = nullptr;
    zmq::socket_t* subscribe_push_ = nullptr;
    
    // K线管理器
    std::map<std::string, std::unique_ptr<KlineManager>> kline_managers_;
    mutable std::mutex kline_managers_mutex_;
    
    // Trades 缓冲区
    std::map<std::string, std::unique_ptr<TradeBuffer>> trade_buffers_;
    mutable std::mutex trade_buffers_mutex_;
    
    // OrderBook 缓冲区
    std::map<std::string, std::unique_ptr<OrderBookBuffer>> orderbook_buffers_;  // key = symbol_channel
    mutable std::mutex orderbook_buffers_mutex_;
    
    // FundingRate 缓冲区
    std::map<std::string, std::unique_ptr<FundingRateBuffer>> funding_rate_buffers_;
    mutable std::mutex funding_rate_buffers_mutex_;
    
    // 订阅记录
    std::map<std::string, std::set<std::string>> subscribed_klines_;  // symbol -> intervals
    std::set<std::string> subscribed_trades_;
    std::map<std::string, std::set<std::string>> subscribed_orderbooks_;  // symbol -> channels
    std::set<std::string> subscribed_funding_rates_;
    mutable std::mutex subscriptions_mutex_;
    
    // 回调
    KlineCallback kline_callback_;
    TradesCallback trades_callback_;
    OrderBookCallback orderbook_callback_;
    FundingRateCallback funding_rate_callback_;
    
    // 统计
    std::atomic<int64_t> kline_count_;
    std::atomic<int64_t> trade_count_;
    std::atomic<int64_t> orderbook_count_;
    std::atomic<int64_t> funding_rate_count_;
};

} // namespace trading

