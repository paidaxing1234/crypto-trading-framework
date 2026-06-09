/**
 * @file py_strategy_base.h
 * @brief Python 策略基类 - 模块化设计
 *
 * 组合三个独立模块：
 * 1. MarketDataModule - 行情数据（K线、trades等）
 * 2. TradingModule - 交易操作（下单、撤单）
 * 3. AccountModule - 账户操作（登录、余额、持仓）
 *
 * 历史数据查询使用 server 端的 RedisDataProvider
 *
 * 通过 pybind11 暴露给 Python，策略继承此类实现业务逻辑
 *
 * @author Sequence Team
 * @date 2025-12
 */

#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <fstream>
#include <map>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <algorithm>

// ZMQ
#include <zmq.hpp>

// JSON
#include <nlohmann/json.hpp>

// Python C-API (用于在 C++ 主循环里处理 Ctrl-C 等信号)
#include <Python.h>
// pybind11 (用于调用 Python 方法)
#include <pybind11/pybind11.h>

// 三个独立模块
#include "market_data_module.h"
#include "trading_module.h"
#include "account_module.h"

// Server 端的 Redis 数据提供者（历史数据查询）
#include "../../server/managers/redis_data_provider.h"

namespace py = pybind11;

namespace trading {

// ============================================================
// 定时任务结构
// ============================================================

/**
 * @brief 定时任务信息
 */
struct ScheduledTask {
    std::string function_name;       // Python 方法名（直接调用的函数名）
    int64_t interval_ms;             // 执行间隔（毫秒）
    int64_t next_run_time_ms;        // 下次执行时间（毫秒时间戳）
    int64_t last_run_time_ms;        // 上次执行时间
    bool enabled;                     // 是否启用
    int run_count;                    // 执行次数
    
    ScheduledTask() 
        : interval_ms(0)
        , next_run_time_ms(0)
        , last_run_time_ms(0)
        , enabled(true)
        , run_count(0) {}
};

/**
 * @brief Python 策略基类
 *
 * 通过组合三个模块提供完整的策略基础设施：
 * - 行情数据：订阅、接收、存储K线/trades等
 * - 交易操作：下单、撤单、查询订单
 * - 账户管理：注册、查询余额/持仓
 * - 历史数据：通过 server 端 RedisDataProvider 查询
 */
class PyStrategyBase {
public:
    /**
     * @brief 构造函数
     * @param strategy_id 策略ID
     * @param max_kline_bars K线最大存储数量（默认 7200 = 2小时1s K线）
     * @param max_trades Trades最大存储数量（默认 10000 条）
     * @param max_orderbook_snapshots OrderBook最大存储数量（默认 1000 个快照）
     * @param max_funding_rate_records FundingRate最大存储数量（默认 100 条）
     * @param log_file_path 日志文件路径（空字符串表示不记录到文件）
     */
    explicit PyStrategyBase(const std::string& strategy_id,
                           size_t max_kline_bars = 7200,
                           size_t max_trades = 10000,
                           size_t max_orderbook_snapshots = 1000,
                           size_t max_funding_rate_records = 100,
                           const std::string& log_file_path = "")
        : strategy_id_(strategy_id)
        , running_(false)
        , market_data_(max_kline_bars, max_trades, max_orderbook_snapshots, max_funding_rate_records)
        , python_self_()
        , start_time_(std::chrono::steady_clock::now())
        , log_file_path_(log_file_path) {

        // 打开日志文件（按天分割，追加模式）
        if (!log_file_path_.empty()) {
            // 解析基础路径：去掉扩展名，用于拼接日期
            // 例如 "logs/strategy_a_main.txt" -> base="logs/strategy_a_main", ext=".log"
            auto dot_pos = log_file_path_.rfind('.');
            if (dot_pos != std::string::npos) {
                log_file_base_ = log_file_path_.substr(0, dot_pos);
            } else {
                log_file_base_ = log_file_path_;
            }
            log_file_ext_ = ".log";
            log_current_date_ = get_date_str();
            std::string daily_path = log_file_base_ + "_" + log_current_date_ + log_file_ext_;
            log_file_.open(daily_path, std::ios::app);
            if (!log_file_.is_open()) {
                std::cerr << "[" << strategy_id_ << "] ERROR: 无法打开日志文件: " << daily_path << std::endl;
            }
        }

        // 设置策略ID
        trading_.set_strategy_id(strategy_id);
        account_.set_strategy_id(strategy_id);

        // 设置日志回调
        auto log_cb = [this](const std::string& msg, bool is_error) {
            if (is_error) {
                log_error(msg);
            } else {
                log_info(msg);
            }
        };
        trading_.set_log_callback(log_cb);
        account_.set_log_callback(log_cb);
    }
    
    /**
     * @brief 设置 Python 对象引用（由 pybind11 绑定自动调用）
     * @param self Python 策略对象
     */
    void set_python_self(py::object self) {
        python_self_ = self;
    }
    
    virtual ~PyStrategyBase() {
        stop();
        // 关闭日志文件
        if (log_file_.is_open()) {
            log_file_.close();
        }
    }
    
    // ============================================================
    // 连接管理
    // ============================================================
    
    /**
     * @brief 连接到实盘服务器
     */
    bool connect() {
        try {
            context_ = std::make_unique<zmq::context_t>(1);

            // 行情订阅 (SUB)
            market_sub_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::sub);
            market_sub_->connect(MARKET_DATA_IPC);
            market_sub_->set(zmq::sockopt::subscribe, "");
            market_sub_->set(zmq::sockopt::rcvtimeo, 100);

            // 订单发送 (PUSH)
            order_push_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::push);
            order_push_->connect(ORDER_IPC);

            // 回报订阅 (SUB)
            report_sub_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::sub);
            report_sub_->connect(REPORT_IPC);
            report_sub_->set(zmq::sockopt::subscribe, "");
            report_sub_->set(zmq::sockopt::rcvtimeo, 100);

            // 订阅管理 (PUSH)
            subscribe_push_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::push);
            subscribe_push_->connect(SUBSCRIBE_IPC);

            // 将 socket 传递给各模块
            market_data_.set_sockets(market_sub_.get(), subscribe_push_.get());
            trading_.set_sockets(order_push_.get(), report_sub_.get());
            account_.set_sockets(order_push_.get(), report_sub_.get());

            running_ = true;
            log_info("已连接到实盘服务器");
            return true;

        } catch (const std::exception& e) {
            log_error("连接失败: " + std::string(e.what()));
            return false;
        }
    }
    
    /**
     * @brief 断开连接
     */
    void disconnect() {
        running_ = false;

        // 注意：策略断开连接时不再自动注销账户
        // 账户注销只在用户从前端"注销账户"时才执行
        // 这实现了策略-账户生命周期分离：停止策略 ≠ 注销账户

        if (market_sub_) market_sub_->close();
        if (order_push_) order_push_->close();
        if (report_sub_) report_sub_->close();
        if (subscribe_push_) subscribe_push_->close();
    }
    
    // ============================================================
    // 行情数据模块 API
    // ============================================================
    
    // --- 订阅管理 ---
    
    bool subscribe_kline(const std::string& symbol, const std::string& interval) {
        bool result = market_data_.subscribe_kline(symbol, interval, strategy_id_);
        if (result) {
            log_info("已订阅 " + symbol + " " + interval + " K线");
        }
        return result;
    }
    
    bool unsubscribe_kline(const std::string& symbol, const std::string& interval) {
        return market_data_.unsubscribe_kline(symbol, interval, strategy_id_);
    }
    
    bool subscribe_trades(const std::string& symbol) {
        bool result = market_data_.subscribe_trades(symbol, strategy_id_);
        if (result) {
            log_info("已订阅 " + symbol + " trades");
        }
        return result;
    }
    
    bool unsubscribe_trades(const std::string& symbol) {
        return market_data_.unsubscribe_trades(symbol, strategy_id_);
    }
    
    bool subscribe_orderbook(const std::string& symbol, const std::string& channel = "books5") {
        bool result = market_data_.subscribe_orderbook(symbol, channel, strategy_id_);
        if (result) {
            log_info("已订阅 " + symbol + " " + channel + " 深度");
        }
        return result;
    }
    
    bool unsubscribe_orderbook(const std::string& symbol, const std::string& channel = "books5") {
        return market_data_.unsubscribe_orderbook(symbol, channel, strategy_id_);
    }
    
    bool subscribe_funding_rate(const std::string& symbol) {
        bool result = market_data_.subscribe_funding_rate(symbol, strategy_id_);
        if (result) {
            log_info("已订阅 " + symbol + " 资金费率");
        }
        return result;
    }
    
    bool unsubscribe_funding_rate(const std::string& symbol) {
        return market_data_.unsubscribe_funding_rate(symbol, strategy_id_);
    }
    
    // --- 数据查询 ---
    
    std::vector<KlineBar> get_klines(const std::string& symbol, const std::string& interval) const {
        return market_data_.get_klines(symbol, interval);
    }
    
    std::vector<double> get_closes(const std::string& symbol, const std::string& interval) const {
        return market_data_.get_closes(symbol, interval);
    }
    
    std::vector<double> get_opens(const std::string& symbol, const std::string& interval) const {
        return market_data_.get_opens(symbol, interval);
    }
    
    std::vector<double> get_highs(const std::string& symbol, const std::string& interval) const {
        return market_data_.get_highs(symbol, interval);
    }
    
    std::vector<double> get_lows(const std::string& symbol, const std::string& interval) const {
        return market_data_.get_lows(symbol, interval);
    }
    
    std::vector<double> get_volumes(const std::string& symbol, const std::string& interval) const {
        return market_data_.get_volumes(symbol, interval);
    }
    
    std::vector<KlineBar> get_recent_klines(const std::string& symbol, 
                                            const std::string& interval, 
                                            size_t n) const {
        return market_data_.get_recent_klines(symbol, interval, n);
    }
    
    bool get_last_kline(const std::string& symbol, const std::string& interval, KlineBar& bar) const {
        return market_data_.get_last_kline(symbol, interval, bar);
    }
    
    size_t get_kline_count(const std::string& symbol, const std::string& interval) const {
        return market_data_.get_kline_count(symbol, interval);
    }
    
    // --- Trades 数据查询 ---
    
    std::vector<TradeData> get_trades(const std::string& symbol) const {
        return market_data_.get_trades(symbol);
    }
    
    std::vector<TradeData> get_recent_trades(const std::string& symbol, size_t n) const {
        return market_data_.get_recent_trades(symbol, n);
    }
    
    std::vector<TradeData> get_trades_by_time(const std::string& symbol, int64_t time_ms) const {
        return market_data_.get_trades_by_time(symbol, time_ms);
    }
    
    bool get_last_trade(const std::string& symbol, TradeData& trade) const {
        return market_data_.get_last_trade(symbol, trade);
    }
    
    size_t get_trade_count(const std::string& symbol) const {
        return market_data_.get_trade_count(symbol);
    }
    
    // --- OrderBook 数据查询 ---
    
    std::vector<OrderBookSnapshot> get_orderbooks(const std::string& symbol, 
                                                  const std::string& channel = "books5") const {
        return market_data_.get_orderbooks(symbol, channel);
    }
    
    std::vector<OrderBookSnapshot> get_recent_orderbooks(const std::string& symbol, 
                                                         size_t n,
                                                         const std::string& channel = "books5") const {
        return market_data_.get_recent_orderbooks(symbol, n, channel);
    }
    
    std::vector<OrderBookSnapshot> get_orderbooks_by_time(const std::string& symbol,
                                                          int64_t time_ms,
                                                          const std::string& channel = "books5") const {
        return market_data_.get_orderbooks_by_time(symbol, time_ms, channel);
    }
    
    bool get_last_orderbook(const std::string& symbol, OrderBookSnapshot& snapshot,
                          const std::string& channel = "books5") const {
        return market_data_.get_last_orderbook(symbol, snapshot, channel);
    }
    
    size_t get_orderbook_count(const std::string& symbol, const std::string& channel = "books5") const {
        return market_data_.get_orderbook_count(symbol, channel);
    }
    
    // --- FundingRate 数据查询 ---
    
    std::vector<FundingRateData> get_funding_rates(const std::string& symbol) const {
        return market_data_.get_funding_rates(symbol);
    }
    
    std::vector<FundingRateData> get_recent_funding_rates(const std::string& symbol, size_t n) const {
        return market_data_.get_recent_funding_rates(symbol, n);
    }
    
    std::vector<FundingRateData> get_funding_rates_by_time(const std::string& symbol, int64_t time_ms) const {
        return market_data_.get_funding_rates_by_time(symbol, time_ms);
    }
    
    bool get_last_funding_rate(const std::string& symbol, FundingRateData& fr) const {
        return market_data_.get_last_funding_rate(symbol, fr);
    }
    
    size_t get_funding_rate_count(const std::string& symbol) const {
        return market_data_.get_funding_rate_count(symbol);
    }
    
    // ============================================================
    // 交易模块 API
    // ============================================================

    // --- 最小下单单位管理 ---

    /**
     * @brief 加载交易所最小下单单位配置文件
     * @param exchange 交易所名称 ("okx" 或 "binance")
     * @param config_dir 配置文件目录路径（默认为相对路径）
     * @return 是否加载成功
     */
    bool load_min_order_config(const std::string& exchange,
                               const std::string& config_dir = "../strategies/configs") {
        std::string filename;
        if (exchange == "okx") {
            filename = config_dir + "/okxmin.txt";
        } else if (exchange == "binance") {
            filename = config_dir + "/binancemin.txt";
        } else {
            log_error("[最小下单单位] 不支持的交易所: " + exchange);
            return false;
        }

        std::ifstream file(filename);
        if (!file.is_open()) {
            log_error("[最小下单单位] 无法打开配置文件: " + filename);
            return false;
        }

        std::lock_guard<std::mutex> lock(min_order_mutex_);
        min_order_quantities_[exchange].clear();

        std::string line;
        bool in_data_section = false;
        int loaded_count = 0;

        while (std::getline(file, line)) {
            // 跳过空行
            if (line.empty()) continue;

            // 检测数据分隔线
            if (line.find("----") != std::string::npos) {
                in_data_section = true;
                continue;
            }

            // 跳过说明部分
            if (line.find("====") != std::string::npos ||
                line.find("说明:") != std::string::npos ||
                line.find("-") == 0) {
                in_data_section = false;
                continue;
            }

            // 解析数据行
            if (in_data_section) {
                std::istringstream iss(line);
                std::string symbol;
                double min_qty;

                if (iss >> symbol >> min_qty) {
                    min_order_quantities_[exchange][symbol] = min_qty;
                    loaded_count++;
                }
            }
        }

        file.close();

        if (loaded_count > 0) {
            log_info("[最小下单单位] 成功加载 " + exchange + " 配置: " +
                    std::to_string(loaded_count) + " 个交易对");
            return true;
        } else {
            log_error("[最小下单单位] 未加载到任何数据: " + filename);
            return false;
        }
    }

    /**
     * @brief 获取指定交易对的最小下单单位
     * @param exchange 交易所名称
     * @param symbol 交易对符号
     * @return 最小下单单位，如果未找到返回0
     */
    double get_min_order_quantity(const std::string& exchange, const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(min_order_mutex_);

        auto exchange_it = min_order_quantities_.find(exchange);
        if (exchange_it == min_order_quantities_.end()) {
            return 0.0;
        }

        auto symbol_it = exchange_it->second.find(symbol);
        if (symbol_it == exchange_it->second.end()) {
            return 0.0;
        }

        return symbol_it->second;
    }

    /**
     * @brief 根据USDT金额计算实际下单数量
     * @param exchange 交易所名称 ("okx" 或 "binance")
     * @param symbol 交易对符号
     * @param usdt_amount 下单金额（USDT）
     * @param current_price 当前价格（从K线获取）
     * @return 实际下单数量（OKX返回张数，Binance返回币数），失败返回0
     */
    double calculate_order_quantity(const std::string& exchange,
                                    const std::string& symbol,
                                    double usdt_amount,
                                    double current_price) {
        if (usdt_amount <= 0 || current_price <= 0) {
            log_error("[下单计算] 无效参数: usdt_amount=" + std::to_string(usdt_amount) +
                     ", price=" + std::to_string(current_price));
            return 0.0;
        }

        // 获取最小下单单位
        double min_qty = get_min_order_quantity(exchange, symbol);
        if (min_qty <= 0) {
            log_error("[下单计算] 未找到 " + exchange + ":" + symbol + " 的最小下单单位配置");
            return 0.0;
        }

        // 计算下单数量
        double quantity = 0.0;

        if (exchange == "okx") {
            // OKX: min_qty 是最小下单张数（minSz）
            // API 需要张数，直接计算即可
            const double OKX_CONTRACT_VALUE = 0.01;  // 1张 = 0.01 BTC/ETH

            // 计算需要的币数
            double required_coins = usdt_amount / current_price;

            // 计算需要的张数 = 需要的币数 / 合约面值
            double required_contracts = required_coins / OKX_CONTRACT_VALUE;

            // 四舍五入到最小张数的整数倍
            int units = static_cast<int>(std::round(required_contracts / min_qty));
            if (units < 1) units = 1;

            // 实际下单张数
            quantity = units * min_qty;

            // 实际币数（用于日志）
            double actual_coins = quantity * OKX_CONTRACT_VALUE;

            log_info("[下单计算-OKX] " + symbol +
                    " | 金额:" + std::to_string(usdt_amount) + "U" +
                    " | 价格:" + std::to_string(current_price) +
                    " | 最小张数:" + std::to_string(min_qty) +
                    " | 下单张数:" + std::to_string(quantity) +
                    " | 实际币数:" + std::to_string(actual_coins));

        } else if (exchange == "binance") {
            // Binance: min_qty是最小下单币数，API也需要币数
            double required_coins = usdt_amount / current_price;
            int units = static_cast<int>(std::round(required_coins / min_qty));
            if (units < 1) units = 1;
            quantity = units * min_qty;

            log_info("[下单计算-Binance] " + symbol +
                    " | 金额:" + std::to_string(usdt_amount) + "U" +
                    " | 价格:" + std::to_string(current_price) +
                    " | 最小币数:" + std::to_string(min_qty) +
                    " | 下单币数:" + std::to_string(quantity));
        } else {
            log_error("[下单计算] 不支持的交易所: " + exchange);
            return 0.0;
        }

        return quantity;
    }

    /**
     * @brief 根据USDT金额下市价单（自动计算数量）
     * @param exchange 交易所名称 ("okx" 或 "binance")
     * @param symbol 交易对符号
     * @param side 方向 ("buy" 或 "sell")
     * @param usdt_amount 下单金额（USDT）
     * @param current_price 当前价格（从K线获取）
     * @param pos_side 持仓方向 (OKX: "long"/"short"/"net", Binance: "LONG"/"SHORT"/"BOTH")
     * @return 订单ID，失败返回空字符串
     */
    std::string send_market_order_by_usdt(const std::string& exchange,
                                          const std::string& symbol,
                                          const std::string& side,
                                          double usdt_amount,
                                          double current_price,
                                          const std::string& pos_side = "net") {
        // 计算下单数量
        double quantity = calculate_order_quantity(exchange, symbol, usdt_amount, current_price);
        if (quantity <= 0) {
            log_error("[按金额下单] 计算数量失败");
            return "";
        }

        // 根据交易所发送订单（传递 current_price 和 usdt_amount 用于风控检查）
        if (exchange == "okx") {
            return send_swap_market_order_with_price(symbol, side, quantity, current_price, usdt_amount, pos_side);
        } else if (exchange == "binance") {
            return send_binance_futures_market_order_with_price(symbol, side, quantity, current_price, usdt_amount, pos_side);
        } else {
            log_error("[按金额下单] 不支持的交易所: " + exchange);
            return "";
        }
    }

    /**
     * @brief 根据USDT金额下限价单（自动计算数量）
     * @param exchange 交易所名称 ("okx" 或 "binance")
     * @param symbol 交易对符号
     * @param side 方向 ("buy" 或 "sell")
     * @param usdt_amount 下单金额（USDT）
     * @param price 限价单价格
     * @param current_price 当前价格（用于计算数量）
     * @param pos_side 持仓方向
     * @return 订单ID，失败返回空字符串
     */
    std::string send_limit_order_by_usdt(const std::string& exchange,
                                         const std::string& symbol,
                                         const std::string& side,
                                         double usdt_amount,
                                         double price,
                                         double current_price,
                                         const std::string& pos_side = "net") {
        // 计算下单数量（使用当前价格计算，而不是限价单价格）
        double quantity = calculate_order_quantity(exchange, symbol, usdt_amount, current_price);
        if (quantity <= 0) {
            log_error("[按金额下限价单] 计算数量失败");
            return "";
        }

        // 根据交易所发送订单
        if (exchange == "okx") {
            return send_swap_limit_order(symbol, side, quantity, price, pos_side);
        } else if (exchange == "binance") {
            return send_binance_futures_limit_order(symbol, side, quantity, price, pos_side);
        } else {
            log_error("[按金额下限价单] 不支持的交易所: " + exchange);
            return "";
        }
    }

    // --- 下单接口 ---

    std::string send_swap_market_order(const std::string& symbol,
                                       const std::string& side,
                                       double quantity,
                                       const std::string& pos_side = "net") {
        return trading_.send_swap_market_order(symbol, side, quantity, pos_side);
    }

    /**
     * @brief 发送市价单（带估算价格和订单金额用于风控）
     */
    std::string send_swap_market_order_with_price(const std::string& symbol,
                                                   const std::string& side,
                                                   double quantity,
                                                   double estimated_price,
                                                   double order_value,
                                                   const std::string& pos_side = "net") {
        return trading_.send_swap_market_order_with_price(symbol, side, quantity, estimated_price, order_value, pos_side);
    }

    /**
     * @brief 发送 Binance 期货市价订单（带估算价格和订单金额用于风控）
     */
    std::string send_binance_futures_market_order_with_price(const std::string& symbol,
                                                             const std::string& side,
                                                             double quantity,
                                                             double estimated_price,
                                                             double order_value,
                                                             const std::string& pos_side = "BOTH") {
        return trading_.send_binance_futures_market_order_with_price(symbol, side, quantity, estimated_price, order_value, pos_side);
    }

    /**
     * @brief 发送 Binance 期货市价订单
     * @param symbol 交易对（如 BTCUSDT）
     * @param side "buy" 或 "sell"
     * @param quantity 数量（币数，非张数）
     * @param pos_side 持仓方向 "BOTH"(默认/单向持仓), "LONG", "SHORT"(双向持仓)
     * @return 客户端订单ID
     */
    std::string send_binance_futures_market_order(const std::string& symbol,
                                                  const std::string& side,
                                                  double quantity,
                                                  const std::string& pos_side = "BOTH") {
        return trading_.send_binance_futures_market_order(symbol, side, quantity, pos_side);
    }

    /**
     * @brief 发送 Binance 期货限价订单
     * @param symbol 交易对（如 BTCUSDT）
     * @param side "buy" 或 "sell"
     * @param quantity 数量（币数）
     * @param price 限价
     * @param pos_side 持仓方向 "BOTH"(默认/单向持仓), "LONG", "SHORT"(双向持仓)
     * @return 客户端订单ID
     */
    std::string send_binance_futures_limit_order(const std::string& symbol,
                                                 const std::string& side,
                                                 double quantity,
                                                 double price,
                                                 const std::string& pos_side = "BOTH") {
        return trading_.send_binance_futures_limit_order(symbol, side, quantity, price, pos_side);
    }
    
    std::string send_swap_limit_order(const std::string& symbol,
                                      const std::string& side,
                                      double quantity,
                                      double price,
                                      const std::string& pos_side = "net") {
        return trading_.send_swap_limit_order(symbol, side, quantity, price, pos_side);
    }
    
    /**
     * @brief 发送合约市价订单（带止盈止损）
     */
    std::string send_swap_market_order_with_tp_sl(
        const std::string& symbol,
        const std::string& side,
        double quantity,
        const std::string& tp_trigger_px = "",
        const std::string& tp_ord_px = "",
        const std::string& sl_trigger_px = "",
        const std::string& sl_ord_px = "",
        const std::string& pos_side = "net",
        const std::string& tag = "") {
        return trading_.send_swap_market_order_with_tp_sl(symbol, side, quantity,
                                                          tp_trigger_px, tp_ord_px,
                                                          sl_trigger_px, sl_ord_px,
                                                          pos_side, tag);
    }
    
    /**
     * @brief 发送合约限价订单（带止盈止损）
     */
    std::string send_swap_limit_order_with_tp_sl(
        const std::string& symbol,
        const std::string& side,
        double quantity,
        double price,
        const std::string& tp_trigger_px = "",
        const std::string& tp_ord_px = "",
        const std::string& sl_trigger_px = "",
        const std::string& sl_ord_px = "",
        const std::string& pos_side = "net",
        const std::string& tag = "") {
        return trading_.send_swap_limit_order_with_tp_sl(symbol, side, quantity, price,
                                                         tp_trigger_px, tp_ord_px,
                                                         sl_trigger_px, sl_ord_px,
                                                         pos_side, tag);
    }
    
    /**
     * @brief 发送高级订单类型（post_only, fok, ioc等）
     */
    std::string send_swap_advanced_order(
        const std::string& symbol,
        const std::string& side,
        double quantity,
        double price,
        const std::string& ord_type,  // "post_only", "fok", "ioc"
        const std::string& pos_side = "net",
        const std::string& tag = "") {
        return trading_.send_swap_advanced_order(symbol, side, quantity, price,
                                                 ord_type, pos_side, tag);
    }
    
    /**
     * @brief 批量下单
     * @param orders 订单列表（Python list of dict）
     * @param exchange 交易所名称 ("okx" 或 "binance")
     * @return 订单ID列表
     */
    std::vector<std::string> send_batch_orders(const std::vector<nlohmann::json>& orders,
                                                const std::string& exchange = "okx") {
        return trading_.send_batch_orders(orders, exchange);
    }

    /**
     * @brief 调整杠杆倍数（仅 Binance 合约）
     * @param symbol 交易对（如 BTCUSDT）
     * @param leverage 杠杆倍数（1-125）
     * @param exchange 交易所名称，默认 "binance"
     * @return 是否发送成功
     */
    bool change_leverage(const std::string& symbol, int leverage, const std::string& exchange = "binance") {
        return trading_.change_leverage(symbol, leverage, exchange);
    }

    // --- 撤单接口 ---
    
    bool cancel_order(const std::string& symbol, const std::string& client_order_id) {
        return trading_.cancel_order(symbol, client_order_id);
    }
    
    bool cancel_all_orders(const std::string& symbol = "") {
        return trading_.cancel_all_orders(symbol);
    }
    
    // --- 订单查询 ---
    
    std::vector<OrderInfo> get_active_orders() const {
        return trading_.get_active_orders();
    }
    
    size_t pending_order_count() const {
        return trading_.pending_order_count();
    }
    
    // ============================================================
    // 账户模块 API
    // ============================================================
    
    // --- 注册/注销 ---

    /**
     * @brief 注册 OKX 账户
     */
    bool register_account(const std::string& api_key,
                         const std::string& secret_key,
                         const std::string& passphrase,
                         bool is_testnet = true,
                         const std::string& account_id = "") {
        return account_.register_account(api_key, secret_key, passphrase, is_testnet, account_id);
    }

    /**
     * @brief 注册 Binance 账户
     * @param api_key Binance API Key
     * @param secret_key Binance Secret Key
     * @param is_testnet 是否使用测试网
     * @param account_id 可选: 显式指定账户主键(同一 api_key 多 strategy 复用一个 account)
     * @return 是否发送成功
     */
    bool register_binance_account(const std::string& api_key,
                                  const std::string& secret_key,
                                  bool is_testnet = true,
                                  const std::string& account_id = "") {
        return account_.register_binance_account(api_key, secret_key, is_testnet, account_id);
    }

    bool unregister_account() {
        return account_.unregister_account();
    }

    bool is_account_registered() const {
        return account_.is_registered();
    }
    
    // --- 余额查询 ---
    
    double get_usdt_available() const {
        return account_.get_usdt_available();
    }
    
    double get_total_equity() const {
        return account_.get_total_equity();
    }
    
    std::vector<BalanceInfo> get_all_balances() const {
        return account_.get_all_balances();
    }
    
    // --- 持仓查询 ---
    
    std::vector<PositionInfo> get_all_positions() const {
        return account_.get_all_positions();
    }
    
    std::vector<PositionInfo> get_active_positions() const {
        return account_.get_active_positions();
    }
    
    bool get_position(const std::string& symbol, PositionInfo& position,
                     const std::string& pos_side = "net") const {
        return account_.get_position(symbol, position, pos_side);
    }
    
    // --- 刷新 ---
    
    bool refresh_account() {
        return account_.refresh_account();
    }
    
    bool refresh_positions() {
        return account_.refresh_positions();
    }

    void clear_positions() {
        account_.clear_positions();
    }

    bool is_position_query_done() const {
        return account_.is_position_query_done();
    }

    bool is_position_query_error() const {
        return account_.is_position_query_error();
    }
    
    // ============================================================
    // 定时任务模块 API
    // ============================================================
    
    /**
     * @brief 注册定时任务
     * @param function_name Python 方法名（策略类中定义的方法名，如 "do_buy_order"）
     * @param interval 执行间隔，格式: "30s", "1m", "5m", "1h", "1d", "1w"
     * @param start_time 首次执行时间，格式: "HH:MM"（如 "14:00"），空字符串或"now"表示立即开始
     * @return 是否成功
     * 
     * 注意: function_name 必须是策略类中已定义的方法名，基类会在定时到达时直接调用该方法
     */
    bool schedule_task(const std::string& function_name, 
                       const std::string& interval,
                       const std::string& start_time = "") {
        
        // 解析时间间隔
        int64_t interval_ms = parse_interval(interval);
        if (interval_ms <= 0) {
            log_error("[定时任务] 无效的时间间隔: " + interval);
            return false;
        }
        
        // 计算首次执行时间
        int64_t first_run_time = calculate_first_run_time(start_time, interval_ms);
        
        // 创建任务
        ScheduledTask task;
        task.function_name = function_name;
        task.interval_ms = interval_ms;
        task.next_run_time_ms = first_run_time;
        task.enabled = true;
        task.run_count = 0;
        
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            scheduled_tasks_[function_name] = task;
        }
        
        // 格式化时间用于日志
        std::time_t next_time = first_run_time / 1000;
        std::tm* tm = std::localtime(&next_time);
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
        
        log_info("[定时任务] 已注册: " + function_name + 
                 " | 间隔: " + interval + 
                 " | 首次执行: " + std::string(time_buf));
        
        return true;
    }
    
    /**
     * @brief 取消定时任务
     */
    bool unschedule_task(const std::string& function_name) {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = scheduled_tasks_.find(function_name);
        if (it == scheduled_tasks_.end()) {
            return false;
        }
        scheduled_tasks_.erase(it);
        log_info("[定时任务] 已取消: " + function_name);
        return true;
    }
    
    /**
     * @brief 暂停定时任务
     */
    bool pause_task(const std::string& function_name) {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = scheduled_tasks_.find(function_name);
        if (it == scheduled_tasks_.end()) {
            return false;
        }
        it->second.enabled = false;
        log_info("[定时任务] 已暂停: " + function_name);
        return true;
    }
    
    /**
     * @brief 恢复定时任务
     */
    bool resume_task(const std::string& function_name) {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = scheduled_tasks_.find(function_name);
        if (it == scheduled_tasks_.end()) {
            return false;
        }
        it->second.enabled = true;
        log_info("[定时任务] 已恢复: " + function_name);
        return true;
    }
    
    /**
     * @brief 获取所有定时任务
     */
    std::vector<ScheduledTask> get_scheduled_tasks() const {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        std::vector<ScheduledTask> result;
        for (const auto& pair : scheduled_tasks_) {
            result.push_back(pair.second);
        }
        return result;
    }
    
    /**
     * @brief 获取任务信息
     */
    bool get_task_info(const std::string& function_name, ScheduledTask& task) const {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = scheduled_tasks_.find(function_name);
        if (it == scheduled_tasks_.end()) {
            return false;
        }
        task = it->second;
        return true;
    }
    
    // ============================================================
    // 主循环
    // ============================================================
    
    /**
     * @brief 运行策略（主循环）
     */
    void run() {
        if (!connect()) {
            log_error("连接失败，无法启动策略");
            return;
        }
        
        // 设置内部回调
        setup_callbacks();
        
        // 调用策略初始化
        on_init();

        log_info("策略运行中...");

        // 心跳计时（每5秒发送一次心跳给服务器）
        auto last_heartbeat_time = std::chrono::steady_clock::now();

        try {
            while (running_) {
                // 让 Python 及时处理挂起的信号（例如 Ctrl-C / SIGINT）
                // 否则 run() 长时间停留在 C++ 循环中时，Python 的 signal handler 可能无法及时执行。
                // 注意：PyErr_CheckSignals() 是 Python C API，需要 GIL
                {
                    py::gil_scoped_acquire gil;
                    if (PyErr_CheckSignals() != 0) {
                        running_ = false;
                        break;
                    }
                }

                // 处理行情数据
                market_data_.process_market_data();
                
                // 处理账户回报（必须先处理，因为它会分发所有回报类型）
                process_account_reports();
                
                // 处理订单回报（只处理订单相关的，但可能已经被 process_account_reports 消费了）
                // 注意：如果订单回报在 process_account_reports 中未被处理，这里会处理
                trading_.process_order_reports();
                
                // 处理定时任务
                process_scheduled_tasks();
                
                // 调用策略 tick
                on_tick();

                // 发送心跳（每5秒）
                auto now_hb = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now_hb - last_heartbeat_time).count() >= 5000) {
                    last_heartbeat_time = now_hb;
                    if (order_push_) {
                        try {
                            nlohmann::json hb = {
                                {"type", "heartbeat"},
                                {"strategy_id", strategy_id_}
                            };
                            order_push_->send(zmq::buffer(hb.dump()), zmq::send_flags::dontwait);
                        } catch (...) {
                            // 心跳发送失败不影响策略运行
                        }
                    }
                }

                // 短暂休眠
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        } catch (const std::exception& e) {
            log_error("策略异常: " + std::string(e.what()));
        }
        
        // 调用策略停止
        on_stop();
        
        // 断开连接
        disconnect();
        
        // 打印总结
        print_summary();
    }
    
    /**
     * @brief 停止策略
     */
    void stop() {
        running_ = false;
    }

    /**
     * @brief 手动处理一轮 ZMQ 消息（供 Python 在等待期间调用）
     *
     * 当 Python 策略需要在回调中等待某个条件（如持仓数据到达），
     * 可以调用此方法驱动 ZMQ 消息处理，避免 sleep() 阻塞主循环。
     *
     * 典型用法：
     *   for i in range(20):
     *       self.poll_messages()
     *       time.sleep(0.1)
     *       if self.get_active_positions():
     *           break
     */
    void poll_messages() {
        // 处理行情数据
        market_data_.process_market_data();
        // 处理账户回报（持仓、余额、注册等）
        process_account_reports();
        // 处理订单回报
        trading_.process_order_reports();
        // 处理定时任务
        process_scheduled_tasks();
    }

    // ============================================================
    // 虚函数（供 Python 重写）
    // ============================================================
    
    virtual void on_init() {}
    virtual void on_stop() {}
    virtual void on_tick() {}
    
    /**
     * @brief K线回调
     */
    virtual void on_kline(const std::string& symbol, const std::string& interval,
                         const KlineBar& bar) {
        (void)symbol; (void)interval; (void)bar;
    }
    
    /**
     * @brief Trades回调
     */
    virtual void on_trade(const std::string& symbol, const TradeData& trade) {
        (void)symbol; (void)trade;
    }
    
    /**
     * @brief OrderBook回调
     */
    virtual void on_orderbook(const std::string& symbol, const OrderBookSnapshot& snapshot) {
        (void)symbol; (void)snapshot;
    }
    
    /**
     * @brief FundingRate回调
     */
    virtual void on_funding_rate(const std::string& symbol, const FundingRateData& fr) {
        (void)symbol; (void)fr;
    }
    
    /**
     * @brief 订单回报回调
     */
    virtual void on_order_report(const nlohmann::json& report) {
        (void)report;
    }
    
    /**
     * @brief 账户注册回报回调
     */
    virtual void on_register_report(bool success, const std::string& error_msg) {
        (void)success; (void)error_msg;
    }
    
    /**
     * @brief 持仓更新回调
     */
    virtual void on_position_update(const PositionInfo& position) {
        (void)position;
    }
    
    /**
     * @brief 余额更新回调
     */
    virtual void on_balance_update(const BalanceInfo& balance) {
        (void)balance;
    }

    /**
     * @brief 账户更新回调
     */
    virtual void on_account_update(double total_equity, double margin_ratio) {
        (void)total_equity; (void)margin_ratio;
    }

    // ============================================================
    // 日志
    // ============================================================

    void log_info(const std::string& msg) const {
        std::string log_msg = "[" + strategy_id_ + "] " + msg;

        // 输出到控制台
        std::cout << log_msg << std::endl;

        // 写入日志文件（按天分割）
        if (!log_file_base_.empty()) {
            std::lock_guard<std::mutex> lock(log_mutex_);
            rotate_log_if_needed();
            if (log_file_.is_open()) {
                log_file_ << get_timestamp() << " " << log_msg << std::endl;
                log_file_.flush();
            }
        }
    }

    void log_error(const std::string& msg) const {
        std::string log_msg = "[" + strategy_id_ + "] ERROR: " + msg;

        // 输出到控制台
        std::cerr << log_msg << std::endl;

        // 写入日志文件（按天分割）
        if (!log_file_base_.empty()) {
            std::lock_guard<std::mutex> lock(log_mutex_);
            rotate_log_if_needed();
            if (log_file_.is_open()) {
                log_file_ << get_timestamp() << " " << log_msg << std::endl;
                log_file_.flush();
            }
        }
    }
    
    // ============================================================
    // 属性
    // ============================================================
    
    const std::string& strategy_id() const { return strategy_id_; }
    bool is_running() const { return running_; }
    
    // 统计
    int64_t kline_count() const { return market_data_.total_kline_count(); }
    int64_t order_count() const { return trading_.total_order_count(); }
    int64_t report_count() const { return trading_.total_report_count(); }
    
    // 获取模块引用（高级用法）
    MarketDataModule& market_data() { return market_data_; }
    TradingModule& trading() { return trading_; }
    AccountModule& account() { return account_; }
    server::RedisDataProvider& historical_data() { return historical_data_; }
    const MarketDataModule& market_data() const { return market_data_; }
    const TradingModule& trading() const { return trading_; }
    const AccountModule& account() const { return account_; }
    const server::RedisDataProvider& historical_data() const { return historical_data_; }

    // ============================================================
    // 历史数据模块 API（使用 server 端 RedisDataProvider）
    // ============================================================

    /**
     * @brief 连接到 Redis 历史数据服务
     * @return 是否成功
     */
    bool connect_historical_data() {
        // 从环境变量加载配置
        server::RedisProviderConfig config;
        const char* redis_host = std::getenv("REDIS_HOST");
        const char* redis_port = std::getenv("REDIS_PORT");
        const char* redis_password = std::getenv("REDIS_PASSWORD");

        if (redis_host) config.host = redis_host;
        if (redis_port) config.port = std::stoi(redis_port);
        if (redis_password) config.password = redis_password;

        historical_data_.set_config(config);
        bool result = historical_data_.connect();
        if (result) {
            log_info("已连接到 Redis 历史数据服务");
        } else {
            log_error("连接 Redis 历史数据服务失败");
        }
        return result;
    }

    /**
     * @brief 查询指定时间范围的历史 K 线数据
     * @param symbol 交易对（如 BTC-USDT-SWAP 或 BTCUSDT）
     * @param exchange 交易所（okx/binance）
     * @param interval 时间周期（1s/1m/5m/15m/1h/4h/1d）
     * @param start_time 开始时间戳（毫秒）
     * @param end_time 结束时间戳（毫秒）
     * @return K 线数据列表（按时间升序）
     */
    std::vector<server::KlineBar> get_historical_klines(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval,
        int64_t start_time,
        int64_t end_time
    ) {
        return historical_data_.get_klines(symbol, exchange, interval, start_time, end_time);
    }

    /**
     * @brief 查询最近 N 天的历史 K 线数据
     * @param symbol 交易对
     * @param exchange 交易所
     * @param interval 时间周期
     * @param days 天数（最大 60 天）
     * @return K 线数据列表（按时间升序）
     */
    std::vector<server::KlineBar> get_historical_klines_by_days(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval,
        int days
    ) {
        return historical_data_.get_klines_by_days(symbol, exchange, interval, days);
    }

    /**
     * @brief 查询最近 N 根历史 K 线
     * @param symbol 交易对
     * @param exchange 交易所
     * @param interval 时间周期
     * @param count 数量
     * @return K 线数据列表（按时间升序）
     */
    std::vector<server::KlineBar> get_latest_historical_klines(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval,
        int count
    ) {
        return historical_data_.get_latest_klines(symbol, exchange, interval, count);
    }

    /**
     * @brief 获取 OKX 历史 K 线（便捷方法）
     */
    std::vector<server::KlineBar> get_okx_historical_klines(
        const std::string& symbol,
        const std::string& interval,
        int days
    ) {
        return historical_data_.get_klines_by_days(symbol, "okx", interval, days);
    }

    /**
     * @brief 获取 Binance 历史 K 线（便捷方法）
     */
    std::vector<server::KlineBar> get_binance_historical_klines(
        const std::string& symbol,
        const std::string& interval,
        int days
    ) {
        return historical_data_.get_klines_by_days(symbol, "binance", interval, days);
    }

    /**
     * @brief 获取历史收盘价数组
     */
    std::vector<double> get_historical_closes(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval,
        int days
    ) {
        auto klines = historical_data_.get_klines_by_days(symbol, exchange, interval, days);
        std::vector<double> closes;
        closes.reserve(klines.size());
        for (const auto& k : klines) {
            closes.push_back(k.close);
        }
        return closes;
    }

    /**
     * @brief 获取可用的历史数据交易对列表
     */
    std::vector<std::string> get_available_historical_symbols(const std::string& exchange = "") {
        return historical_data_.get_available_symbols(exchange);
    }

    /**
     * @brief 获取指定交易对的历史数据时间范围
     * @return {earliest_timestamp, latest_timestamp}
     */
    std::pair<int64_t, int64_t> get_historical_data_time_range(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval
    ) {
        return historical_data_.get_data_time_range(symbol, exchange, interval);
    }

    /**
     * @brief 获取指定交易对的历史 K 线数量
     */
    int64_t get_historical_kline_count(
        const std::string& symbol,
        const std::string& exchange,
        const std::string& interval
    ) {
        return historical_data_.get_kline_count(symbol, exchange, interval);
    }

    /**
     * @brief 批量获取多个币种最新K线时间戳（Pipeline，单次Redis往返，<1ms）
     * @param symbols 交易对列表
     * @param exchange 交易所
     * @param interval 时间周期
     * @return {symbol: latest_timestamp_ms} 字典
     */
    std::map<std::string, int64_t> batch_get_latest_kline_timestamps(
        const std::vector<std::string>& symbols,
        const std::string& exchange,
        const std::string& interval
    ) {
        return historical_data_.batch_get_latest_kline_timestamps(symbols, exchange, interval);
    }

    /**
     * @brief 批量获取多个币种最新1根K线数据（Pipeline，单次Redis往返）
     * @param symbols 交易对列表
     * @param exchange 交易所
     * @param interval 时间周期
     * @return {symbol: KlineBar} 字典
     */
    std::map<std::string, server::KlineBar> batch_get_latest_klines(
        const std::vector<std::string>& symbols,
        const std::string& exchange,
        const std::string& interval
    ) {
        return historical_data_.batch_get_latest_klines(symbols, exchange, interval);
    }

    /**
     * @brief Lua脚本批量获取最新时间戳（最快，单次EVALSHA，<0.5ms）
     */
    std::map<std::string, int64_t> lua_batch_get_latest_timestamps(
        const std::vector<std::string>& symbols,
        const std::string& exchange,
        const std::string& interval
    ) {
        return historical_data_.lua_batch_get_latest_timestamps(symbols, exchange, interval);
    }

protected:
    // IPC 地址 (与 ZmqServer 保持一致)
    static constexpr const char* MARKET_DATA_IPC = "ipc:///tmp/seq_md.ipc";
    static constexpr const char* ORDER_IPC = "ipc:///tmp/seq_order.ipc";
    static constexpr const char* REPORT_IPC = "ipc:///tmp/seq_report.ipc";
    static constexpr const char* SUBSCRIBE_IPC = "ipc:///tmp/seq_subscribe.ipc";

private:
    void setup_callbacks() {
        // 设置 K 线回调
        market_data_.set_kline_callback(
            [this](const std::string& symbol, const std::string& interval, const KlineBar& bar) {
                on_kline(symbol, interval, bar);
            }
        );
        
        // 设置 trades 回调
        market_data_.set_trades_callback(
            [this](const std::string& symbol, const TradeData& trade) {
                on_trade(symbol, trade);
            }
        );
        
        // 设置 orderbook 回调
        market_data_.set_orderbook_callback(
            [this](const std::string& symbol, const OrderBookSnapshot& snapshot) {
                on_orderbook(symbol, snapshot);
            }
        );
        
        // 设置 funding_rate 回调
        market_data_.set_funding_rate_callback(
            [this](const std::string& symbol, const FundingRateData& fr) {
                on_funding_rate(symbol, fr);
            }
        );
        
        // 设置订单回报回调
        trading_.set_order_report_callback(
            [this](const nlohmann::json& report) {
                on_order_report(report);
            }
        );
        
        // 设置账户回调（register_callback_ 不在此设置，由 handle_register_report 直接触发，避免重复）
        account_.set_position_update_callback(
            [this](const PositionInfo& position) {
                on_position_update(position);
            }
        );
        
        account_.set_balance_update_callback(
            [this](const BalanceInfo& balance) {
                on_balance_update(balance);
            }
        );
    }
    
    /**
     * @brief 处理账户回报（需要单独处理，因为共享 report_sub_）
     * 
     * 统一处理所有回报类型，确保注册回报等账户相关消息不被遗漏
     */
    void process_account_reports() {
        if (!report_sub_) {
            return;
        }

        zmq::message_t message;
        while (report_sub_->recv(message, zmq::recv_flags::dontwait)) {
            try {
                std::string msg_str(static_cast<char*>(message.data()), message.size());

                // 消息格式: topic|json_data
                size_t sep_pos = msg_str.find('|');
                std::string json_str;
                if (sep_pos != std::string::npos) {
                    json_str = msg_str.substr(sep_pos + 1);
                } else {
                    json_str = msg_str;
                }

                auto report = nlohmann::json::parse(json_str);
                std::string report_type = report.value("type", "");

                // 过滤 strategy_id，只处理属于当前策略的回报
                std::string report_strategy_id = report.value("strategy_id", "");

                if (!report_strategy_id.empty() && report_strategy_id != strategy_id_) {
                    // 不是当前策略的回报，跳过
                    continue;
                }

                // 分发给各模块处理
                if (report_type == "order_update" ||
                    report_type == "order_report" ||
                    report_type == "order_response") {
                    // order_update 消息结构: {"type": "order_update", "exchange": "binance", "data": {...}}
                    // 需要提取 data 字段并添加 exchange 信息
                    nlohmann::json order_data;
                    if (report.contains("data")) {
                        // 从 data 字段提取实际订单数据
                        order_data = report["data"];
                        // 将外层的 exchange 字段添加到订单数据中
                        if (report.contains("exchange")) {
                            order_data["exchange"] = report["exchange"];
                        }
                        // 保留 type 字段
                        order_data["type"] = report_type;
                        // 保留 strategy_id 字段
                        if (report.contains("strategy_id")) {
                            order_data["strategy_id"] = report["strategy_id"];
                        }
                    } else {
                        // 如果没有 data 字段，直接使用原始 report
                        order_data = report;
                    }
                    trading_.process_single_order_report(order_data);
                }
                else if (report_type == "register_report" ||
                         report_type == "unregister_report") {
                    handle_register_report(report);
                }
                else if (report_type == "account_update") {
                    handle_account_update(report);
                }
                else if (report_type == "position_update") {
                    handle_position_update(report);
                }
                else if (report_type == "balance_update") {
                    handle_balance_update(report);
                }
                else if (report_type == "batch_report") {
                    // 批量订单回报：将每个订单结果转换为单独的 on_order_report 回调
                    handle_batch_report(report);
                }

            } catch (const std::exception& e) {
                log_error("[回报处理] 解析失败: " + std::string(e.what()));
            }
        }
    }

    void handle_register_report(const nlohmann::json& report) {
        std::string status = report.value("status", "");

        // 让 AccountModule 更新内部状态（不设置 register_callback_，避免重复触发）
        account_.handle_register_report_public(report);

        // 直接触发 Python 回调
        if (status == "registered") {
            on_register_report(true, "");
        } else if (status == "unregistered") {
            log_info("[账户注销] ✓ 已注销");
        } else {
            std::string error_msg = report.value("error_msg", "未知错误");
            log_error("[账户注册] ✗ 失败: " + error_msg);
            on_register_report(false, error_msg);
        }
    }

    void handle_account_update(const nlohmann::json& report) {
        // 调用 AccountModule 处理账户更新
        if (report.contains("data")) {
            // 先让 AccountModule 更新内部状态
            account_.handle_account_update(report);

            // 获取更新后的账户概要
            auto summary = account_.get_account_summary();

            // 触发 Python 回调
            {
                py::gil_scoped_acquire gil;
                on_account_update(summary.total_equity, summary.margin_ratio);
            }

            log_info("[账户更新] 总权益: " + std::to_string(summary.total_equity) +
                    " USDT, 保证金率: " + std::to_string(summary.margin_ratio));
        }
    }

    void handle_batch_report(const nlohmann::json& report) {
        // 批量订单回报处理
        // 服务端返回格式: {"type": "batch_report", "strategy_id": "...", "batch_id": "...",
        //                 "status": "accepted/partial/rejected", "results": [...],
        //                 "success_count": N, "fail_count": M}

        std::string batch_id = report.value("batch_id", "");
        std::string status = report.value("status", "");
        int success_count = report.value("success_count", 0);
        int fail_count = report.value("fail_count", 0);

        log_info("[批量订单回报] batch_id=" + batch_id + " status=" + status +
                 " 成功=" + std::to_string(success_count) + " 失败=" + std::to_string(fail_count));

        // 遍历每个订单结果，触发 on_order_report 回调
        if (report.contains("results") && report["results"].is_array()) {
            for (const auto& result : report["results"]) {
                nlohmann::json order_report;
                order_report["type"] = "order_report";
                order_report["batch_id"] = batch_id;
                order_report["symbol"] = result.value("symbol", "");
                order_report["side"] = result.value("side", "");
                order_report["client_order_id"] = result.value("clientOrderId", "");

                // ★ 优先检查是否是错误响应（包含code字段表示API错误）
                // 错误响应格式：{"code":-4003,"msg":"Quantity less than or equal to zero."}
                if (result.contains("code") && result["code"].is_number()) {
                    // 这是一个API错误响应
                    order_report["status"] = "rejected";
                    order_report["error_msg"] = result.value("msg", "Unknown error");
                    order_report["filled_quantity"] = "0";
                    order_report["filled_price"] = "0";
                    order_report["exchange_order_id"] = "";
                } else {
                    // 正常的订单响应（包含orderId和status字段）
                    // 注意: 必须用 int64 默认值 —— 币安 orderId 已超 int32 (如 2484760410), int 会截断
                    order_report["exchange_order_id"] = std::to_string(result.value("orderId", 0LL));

                    // Binance批量下单响应字段：origQty（原始数量）, executedQty（已成交数量）, avgPrice（成交均价）
                    // 对于市价单，立即返回时executedQty和avgPrice可能为0，订单会很快成交
                    // 我们使用origQty作为下单数量，executedQty作为已成交数量
                    std::string orig_qty = result.value("origQty", "0");
                    std::string executed_qty = result.value("executedQty", "0");
                    std::string avg_price = result.value("avgPrice", "0");

                    // 使用已成交数量，如果为0则使用原始数量（表示订单已提交但尚未成交）
                    order_report["filled_quantity"] = (executed_qty != "0" && executed_qty != "0.0" && executed_qty != "0.00") ? executed_qty : orig_qty;
                    order_report["filled_price"] = avg_price;

                    std::string order_status = result.value("status", "");

                    // 转换为小写以便不区分大小写比较
                    std::string status_lower = order_status;
                    std::transform(status_lower.begin(), status_lower.end(), status_lower.begin(), ::tolower);

                    // accepted, new, filled, partially_filled 都表示订单成功
                    if (status_lower == "accepted" || status_lower == "new" ||
                        status_lower == "filled" || status_lower == "partially_filled") {
                        order_report["status"] = "filled";  // 订单已接受/成交
                    } else if (status_lower == "rejected" || status_lower == "expired" ||
                               status_lower == "canceled" || status_lower == "cancelled") {
                        // 订单被拒绝、过期或取消
                        order_report["status"] = "rejected";

                        // 尝试获取错误原因，可能在 error_msg, msg 或 rejectReason 字段中
                        std::string error_msg = result.value("error_msg", "");
                        if (error_msg.empty()) {
                            error_msg = result.value("msg", "");
                        }
                        if (error_msg.empty()) {
                            error_msg = result.value("rejectReason", "");
                        }
                        if (error_msg.empty()) {
                            error_msg = "Order status: " + order_status;
                        }
                        order_report["error_msg"] = error_msg;
                    } else {
                        // 其他未知状态
                        order_report["status"] = "rejected";
                        order_report["error_msg"] = "Unknown order status: " + order_status;
                    }
                }

                // 触发 Python 回调
                {
                    py::gil_scoped_acquire gil;
                    on_order_report(order_report);
                }
            }
        }
    }

    void handle_position_update(const nlohmann::json& report) {
        if (!report.contains("data")) return;

        // 先让 AccountModule 更新内部 positions_ 状态
        account_.handle_position_update_public(report);

        // 然后触发 Python 回调
        for (const auto& pos_data : report["data"]) {
            PositionInfo position;
            position.symbol = pos_data.value("instId", "");
            position.pos_side = pos_data.value("posSide", "net");
            position.quantity = std::stod(pos_data.value("pos", "0"));
            position.avg_price = std::stod(pos_data.value("avgPx", "0"));
            position.unrealized_pnl = std::stod(pos_data.value("upl", "0"));

            if (!position.symbol.empty()) {
                on_position_update(position);
            }
        }
    }
    
    void handle_balance_update(const nlohmann::json& report) {
        if (!report.contains("data")) return;

        // 先让 AccountModule 处理余额更新
        account_.handle_balance_update(report);

        // 然后触发 Python 回调
        for (const auto& bal_data : report["data"]) {
            BalanceInfo balance;
            balance.currency = bal_data.value("ccy", "");
            balance.available = std::stod(bal_data.value("availBal", "0"));
            balance.frozen = std::stod(bal_data.value("frozenBal", "0"));
            balance.total = std::stod(bal_data.value("cashBal", "0"));

            if (!balance.currency.empty()) {
                py::gil_scoped_acquire gil;
                on_balance_update(balance);
            }
        }
    }
    
    void print_summary() {
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time_).count();

        log_info("[退出] 运行 " + std::to_string(elapsed) + "s | K线: " +
                 std::to_string(kline_count()) + " | 订单: " +
                 std::to_string(order_count()) + " | 回报: " +
                 std::to_string(report_count()));
    }
    
    // ============================================================
    // 定时任务辅助方法
    // ============================================================
    
    /**
     * @brief 解析时间间隔字符串
     * @param interval 格式: "30s", "1m", "5m", "1h", "4h", "1d", "1w"
     * @return 毫秒数，失败返回 -1
     */
    int64_t parse_interval(const std::string& interval) {
        if (interval.empty()) return -1;
        
        // 解析数字和单位
        size_t num_end = 0;
        int64_t value = 0;
        try {
            value = std::stoll(interval, &num_end);
        } catch (...) {
            return -1;
        }
        
        if (num_end >= interval.size()) return -1;
        
        std::string unit = interval.substr(num_end);
        
        // 转换为毫秒
        if (unit == "s" || unit == "S") {
            return value * 1000;
        } else if (unit == "m" || unit == "M") {
            return value * 60 * 1000;
        } else if (unit == "h" || unit == "H") {
            return value * 60 * 60 * 1000;
        } else if (unit == "d" || unit == "D") {
            return value * 24 * 60 * 60 * 1000;
        } else if (unit == "w" || unit == "W") {
            return value * 7 * 24 * 60 * 60 * 1000;
        }
        
        return -1;
    }
    
    /**
     * @brief 计算首次执行时间
     * @param start_time "HH:MM" 格式，或空字符串/"now"表示立即
     * @param interval_ms 时间间隔（毫秒）
     * @return 首次执行的毫秒时间戳
     */
    int64_t calculate_first_run_time(const std::string& start_time, int64_t interval_ms) {
        int64_t now_ms = current_timestamp_ms();
        
        // 空字符串或"now"表示立即执行
        if (start_time.empty() || start_time == "now" || start_time == "NOW") {
            return now_ms;
        }
        
        // 解析 "HH:MM" 格式
        int hour = 0, minute = 0;
        char sep;
        std::istringstream iss(start_time);
        if (!(iss >> hour >> sep >> minute) || sep != ':') {
            log_error("[定时任务] 无效的开始时间格式: " + start_time + " (应为 HH:MM)");
            return now_ms;  // 解析失败，立即开始
        }
        
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            log_error("[定时任务] 无效的时间值: " + start_time);
            return now_ms;
        }
        
        // 获取当前时间
        std::time_t now_time = now_ms / 1000;
        std::tm* tm = std::localtime(&now_time);
        
        // 设置目标时间（今天的 HH:MM:00）
        std::tm target_tm = *tm;
        target_tm.tm_hour = hour;
        target_tm.tm_min = minute;
        target_tm.tm_sec = 0;
        
        std::time_t target_time = std::mktime(&target_tm);
        int64_t target_ms = target_time * 1000;
        
        // 如果目标时间已过，根据间隔计算下一次
        if (target_ms <= now_ms) {
            // 计算需要多少个间隔才能超过当前时间
            int64_t diff = now_ms - target_ms;
            int64_t intervals_passed = diff / interval_ms + 1;
            target_ms += intervals_passed * interval_ms;
        }
        
        return target_ms;
    }
    
    /**
     * @brief 处理定时任务（在主循环中调用）
     */
    void process_scheduled_tasks() {
        int64_t now_ms = current_timestamp_ms();
        
        std::vector<std::string> functions_to_call;
        
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            for (auto& pair : scheduled_tasks_) {
                auto& task = pair.second;
                if (task.enabled && now_ms >= task.next_run_time_ms) {
                    functions_to_call.push_back(task.function_name);
                    
                    // 更新任务状态
                    task.last_run_time_ms = now_ms;
                    task.next_run_time_ms = now_ms + task.interval_ms;
                    task.run_count++;
                }
            }
        }
        
        // 执行任务（在锁外执行，避免死锁）
        for (const auto& function_name : functions_to_call) {
            try {
                // 格式化下次执行时间
                ScheduledTask task;
                {
                    std::lock_guard<std::mutex> lock(tasks_mutex_);
                    task = scheduled_tasks_[function_name];
                }
                
                std::time_t next_time = task.next_run_time_ms / 1000;
                std::tm* tm = std::localtime(&next_time);
                char time_buf[64];
                std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm);
                
                log_info("[定时任务] 执行: " + function_name + 
                        " | 第 " + std::to_string(task.run_count) + " 次" +
                        " | 下次: " + std::string(time_buf));
                
                // 直接调用 Python 方法（需要获取 GIL）
                {
                    py::gil_scoped_acquire gil;
                    if (!python_self_.is_none()) {
                        // 检查方法是否存在
                        if (py::hasattr(python_self_, function_name.c_str())) {
                            // 获取方法并调用
                            py::object method = python_self_.attr(function_name.c_str());
                            method();  // 调用方法（无参数）
                        } else {
                            log_error("[定时任务] 方法不存在: " + function_name);
                        }
                    } else {
                        log_error("[定时任务] Python 对象未设置，无法调用方法: " + function_name);
                    }
                }
                
            } catch (py::error_already_set& e) {
                log_error("[定时任务] Python 调用失败: " + function_name + " - " + std::string(e.what()));
                e.restore();
            } catch (const std::exception& e) {
                log_error("[定时任务] 执行失败: " + function_name + " - " + e.what());
            }
        }
    }
    
    static int64_t current_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    /**
     * @brief 获取格式化的时间戳字符串
     * @return 格式: [YYYY-MM-DD HH:MM:SS.mmm]
     */
    static std::string get_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        auto timer = std::chrono::system_clock::to_time_t(now);
        std::tm bt = *std::localtime(&timer);

        std::ostringstream oss;
        oss << "[" << std::put_time(&bt, "%Y-%m-%d %H:%M:%S");
        oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << "]";
        return oss.str();
    }

    /**
     * @brief 获取当前日期字符串
     * @return 格式: YYYYMMDD
     */
    static std::string get_date_str() {
        auto now = std::chrono::system_clock::now();
        auto timer = std::chrono::system_clock::to_time_t(now);
        std::tm bt = *std::localtime(&timer);
        std::ostringstream oss;
        oss << std::put_time(&bt, "%Y%m%d");
        return oss.str();
    }

    /**
     * @brief 检查日期是否变化，如果跨天则切换到新日志文件
     * @note 调用前必须持有 log_mutex_
     */
    void rotate_log_if_needed() const {
        if (log_file_base_.empty()) return;
        std::string today = get_date_str();
        if (today != log_current_date_) {
            if (log_file_.is_open()) {
                log_file_.close();
            }
            log_current_date_ = today;
            std::string daily_path = log_file_base_ + "_" + log_current_date_ + log_file_ext_;
            log_file_.open(daily_path, std::ios::app);
        }
    }

private:
    // 策略配置
    std::string strategy_id_;
    std::atomic<bool> running_;

    // ZMQ
    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> market_sub_;
    std::unique_ptr<zmq::socket_t> order_push_;
    std::unique_ptr<zmq::socket_t> report_sub_;
    std::unique_ptr<zmq::socket_t> subscribe_push_;

    // 三个独立模块
    MarketDataModule market_data_;
    TradingModule trading_;
    AccountModule account_;

    // 历史数据查询（使用 server 端的 RedisDataProvider）
    server::RedisDataProvider historical_data_;

    // 定时任务
    std::map<std::string, ScheduledTask> scheduled_tasks_;
    mutable std::mutex tasks_mutex_;

    // Python 对象引用（用于直接调用 Python 方法）
    py::object python_self_;

    // 时间
    std::chrono::steady_clock::time_point start_time_;

    // 日志文件（按天分割）
    std::string log_file_path_;          // 原始配置路径
    mutable std::string log_file_base_;  // 基础路径（不含日期和扩展名）
    mutable std::string log_file_ext_;   // 扩展名（.log）
    mutable std::string log_current_date_; // 当前日志文件对应的日期 YYYYMMDD
    mutable std::ofstream log_file_;
    mutable std::mutex log_mutex_;

    // 最小下单单位配置 (exchange -> symbol -> min_quantity)
    std::map<std::string, std::map<std::string, double>> min_order_quantities_;
    mutable std::mutex min_order_mutex_;
};

} // namespace trading
