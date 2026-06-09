/**
 * @file py_strategy_bindings.cpp
 * @brief pybind11 绑定 - 将 C++ 策略基类及模块暴露给 Python
 * 
 * 模块化设计：
 * - MarketDataModule: 行情数据模块
 * - TradingModule: 交易模块
 * - AccountModule: 账户模块
 * - StrategyBase: 策略基类（组合三个模块）
 * 
 * @author Sequence Team
 * @date 2025-12
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include "py_strategy_base.h"

namespace py = pybind11;
using namespace trading;


// ============================================================
// nlohmann::json 与 Python dict 的转换
// ============================================================

namespace pybind11 { namespace detail {

template <>
struct type_caster<nlohmann::json> {
public:
    PYBIND11_TYPE_CASTER(nlohmann::json, _("json"));
    
    // Python -> C++
    bool load(handle src, bool) {
        try {
            auto json_module = py::module::import("json");
            std::string json_str = json_module.attr("dumps")(src).cast<std::string>();
            value = nlohmann::json::parse(json_str);
            return true;
        } catch (...) {
            return false;
        }
    }
    
    // C++ -> Python
    static handle cast(const nlohmann::json& src, return_value_policy, handle) {
        auto json_module = py::module::import("json");
        return json_module.attr("loads")(src.dump()).release();
    }
};

}} // namespace pybind11::detail


// ============================================================
// PyStrategyBase 的 trampoline 类（允许 Python 继承）
// ============================================================

class PyStrategyTrampoline : public PyStrategyBase {
public:
    using PyStrategyBase::PyStrategyBase;
    
    // 注意：run() 使用 gil_scoped_release 释放了 GIL，
    // 因此所有 Python 回调都需要使用 gil_scoped_acquire 重新获取 GIL
    
    void on_init() override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_init);
    }
    
    void on_stop() override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_stop);
    }
    
    void on_tick() override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_tick);
    }
    
    void on_kline(const std::string& symbol, const std::string& interval,
                 const KlineBar& bar) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_kline, symbol, interval, bar);
    }
    
    void on_trade(const std::string& symbol, const TradeData& trade) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_trade, symbol, trade);
    }
    
    void on_orderbook(const std::string& symbol, const OrderBookSnapshot& snapshot) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_orderbook, symbol, snapshot);
    }
    
    void on_funding_rate(const std::string& symbol, const FundingRateData& fr) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_funding_rate, symbol, fr);
    }
    
    void on_order_report(const nlohmann::json& report) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_order_report, report);
    }
    
    void on_register_report(bool success, const std::string& error_msg) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_register_report, success, error_msg);
    }
    
    void on_position_update(const PositionInfo& position) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_position_update, position);
    }

    void on_balance_update(const BalanceInfo& balance) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_balance_update, balance);
    }

    void on_account_update(double total_equity, double margin_ratio) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE(void, PyStrategyBase, on_account_update, total_equity, margin_ratio);
    }
};


// ============================================================
// Python 模块定义
// ============================================================

PYBIND11_MODULE(strategy_base, m) {
    m.doc() = R"doc(
        策略基类模块 - 模块化设计
        
        提供三个独立的功能模块：
        1. MarketDataModule - 行情数据（K线、trades等）
        2. TradingModule - 交易操作（下单、撤单）
        3. AccountModule - 账户操作（登录、余额、持仓）
        
        以及一个组合三者的策略基类 StrategyBase
        
        使用方法：
        
            from strategy_base import StrategyBase, KlineBar
            
            class MyStrategy(StrategyBase):
                def on_init(self):
                    self.subscribe_kline("BTC-USDT-SWAP", "1s")
                
                def on_kline(self, symbol, interval, bar):
                    print(f"K线: {symbol} close={bar.close}")
                
                def on_order_report(self, report):
                    print(f"订单回报: {report}")
            
            strategy = MyStrategy("my_strategy")
            strategy.register_account(api_key, secret_key, passphrase)
            strategy.run()
    )doc";
    
    // ==================== KlineBar ====================
    py::class_<KlineBar>(m, "KlineBar", "K线数据结构")
        .def(py::init<>())
        .def(py::init<int64_t, double, double, double, double, double>(),
             py::arg("timestamp"), py::arg("open"), py::arg("high"),
             py::arg("low"), py::arg("close"), py::arg("volume"))
        .def_readwrite("timestamp", &KlineBar::timestamp, "时间戳（毫秒）")
        .def_readwrite("open", &KlineBar::open, "开盘价")
        .def_readwrite("high", &KlineBar::high, "最高价")
        .def_readwrite("low", &KlineBar::low, "最低价")
        .def_readwrite("close", &KlineBar::close, "收盘价")
        .def_readwrite("volume", &KlineBar::volume, "成交量")
        .def("__repr__", [](const KlineBar& bar) {
            return "KlineBar(ts=" + std::to_string(bar.timestamp) + 
                   ", c=" + std::to_string(bar.close) + ")";
        });
    
    // ==================== TradeData ====================
    py::class_<TradeData>(m, "TradeData", "逐笔成交数据")
        .def(py::init<>())
        .def_readwrite("timestamp", &TradeData::timestamp, "时间戳（毫秒）")
        .def_readwrite("trade_id", &TradeData::trade_id, "成交ID")
        .def_readwrite("price", &TradeData::price, "成交价格")
        .def_readwrite("quantity", &TradeData::quantity, "成交数量")
        .def_readwrite("side", &TradeData::side, "买卖方向");
    
    // ==================== OrderBookSnapshot ====================
    py::class_<OrderBookSnapshot>(m, "OrderBookSnapshot", "深度快照数据")
        .def(py::init<>())
        .def_readwrite("timestamp", &OrderBookSnapshot::timestamp, "时间戳（毫秒）")
        .def_readwrite("bids", &OrderBookSnapshot::bids, "买盘 [(price, size), ...]")
        .def_readwrite("asks", &OrderBookSnapshot::asks, "卖盘 [(price, size), ...]")
        .def_readwrite("best_bid_price", &OrderBookSnapshot::best_bid_price, "最优买价")
        .def_readwrite("best_bid_size", &OrderBookSnapshot::best_bid_size, "最优买量")
        .def_readwrite("best_ask_price", &OrderBookSnapshot::best_ask_price, "最优卖价")
        .def_readwrite("best_ask_size", &OrderBookSnapshot::best_ask_size, "最优卖量")
        .def_readwrite("mid_price", &OrderBookSnapshot::mid_price, "中间价")
        .def_readwrite("spread", &OrderBookSnapshot::spread, "价差");
    
    // ==================== FundingRateData ====================
    py::class_<FundingRateData>(m, "FundingRateData", "资金费率数据")
        .def(py::init<>())
        .def_readwrite("timestamp", &FundingRateData::timestamp, "时间戳（毫秒）")
        .def_readwrite("funding_rate", &FundingRateData::funding_rate, "当前资金费率")
        .def_readwrite("next_funding_rate", &FundingRateData::next_funding_rate, "下一期预测资金费率")
        .def_readwrite("funding_time", &FundingRateData::funding_time, "资金费时间（毫秒）")
        .def_readwrite("next_funding_time", &FundingRateData::next_funding_time, "下一期资金费时间（毫秒）")
        .def_readwrite("min_funding_rate", &FundingRateData::min_funding_rate, "资金费率下限")
        .def_readwrite("max_funding_rate", &FundingRateData::max_funding_rate, "资金费率上限")
        .def_readwrite("interest_rate", &FundingRateData::interest_rate, "利率")
        .def_readwrite("impact_value", &FundingRateData::impact_value, "深度加权金额")
        .def_readwrite("premium", &FundingRateData::premium, "溢价指数")
        .def_readwrite("sett_funding_rate", &FundingRateData::sett_funding_rate, "结算资金费率")
        .def_readwrite("method", &FundingRateData::method, "资金费收取逻辑")
        .def_readwrite("formula_type", &FundingRateData::formula_type, "公式类型")
        .def_readwrite("sett_state", &FundingRateData::sett_state, "结算状态");
    
    // ==================== BalanceInfo ====================
    py::class_<BalanceInfo>(m, "BalanceInfo", "余额信息")
        .def(py::init<>())
        .def_readwrite("currency", &BalanceInfo::currency, "币种")
        .def_readwrite("available", &BalanceInfo::available, "可用余额")
        .def_readwrite("frozen", &BalanceInfo::frozen, "冻结余额")
        .def_readwrite("total", &BalanceInfo::total, "总余额")
        .def_readwrite("usd_value", &BalanceInfo::usd_value, "USD估值")
        .def_readwrite("update_time", &BalanceInfo::update_time, "更新时间")
        .def("__repr__", [](const BalanceInfo& b) {
            return "BalanceInfo(" + b.currency + 
                   ", avail=" + std::to_string(b.available) + ")";
        });
    
    // ==================== PositionInfo ====================
    py::class_<PositionInfo>(m, "PositionInfo", "持仓信息")
        .def(py::init<>())
        .def_readwrite("symbol", &PositionInfo::symbol, "交易对")
        .def_readwrite("pos_side", &PositionInfo::pos_side, "持仓方向")
        .def_readwrite("quantity", &PositionInfo::quantity, "持仓数量")
        .def_readwrite("avg_price", &PositionInfo::avg_price, "持仓均价")
        .def_readwrite("mark_price", &PositionInfo::mark_price, "标记价格")
        .def_readwrite("unrealized_pnl", &PositionInfo::unrealized_pnl, "未实现盈亏")
        .def_readwrite("realized_pnl", &PositionInfo::realized_pnl, "已实现盈亏")
        .def_readwrite("margin", &PositionInfo::margin, "保证金")
        .def_readwrite("leverage", &PositionInfo::leverage, "杠杆倍数")
        .def_readwrite("liquidation_price", &PositionInfo::liquidation_price, "强平价格")
        .def_readwrite("update_time", &PositionInfo::update_time, "更新时间")
        .def("__repr__", [](const PositionInfo& p) {
            return "PositionInfo(" + p.symbol + " " + p.pos_side + 
                   ", qty=" + std::to_string(p.quantity) + ")";
        });
    
    // ==================== OrderInfo ====================
    py::class_<OrderInfo>(m, "OrderInfo", "订单信息")
        .def(py::init<>())
        .def_readwrite("client_order_id", &OrderInfo::client_order_id, "客户端订单ID")
        .def_readwrite("exchange_order_id", &OrderInfo::exchange_order_id, "交易所订单ID")
        .def_readwrite("symbol", &OrderInfo::symbol, "交易对")
        .def_readwrite("side", &OrderInfo::side, "买卖方向")
        .def_readwrite("order_type", &OrderInfo::order_type, "订单类型")
        .def_readwrite("pos_side", &OrderInfo::pos_side, "持仓方向")
        .def_readwrite("price", &OrderInfo::price, "价格")
        .def_readwrite("quantity", &OrderInfo::quantity, "数量")
        .def_readwrite("filled_quantity", &OrderInfo::filled_quantity, "已成交数量")
        .def_readwrite("filled_price", &OrderInfo::filled_price, "成交均价")
        .def_readwrite("error_msg", &OrderInfo::error_msg, "错误信息")
        .def("__repr__", [](const OrderInfo& o) {
            return "OrderInfo(" + o.symbol + " " + o.side + 
                   ", qty=" + std::to_string(o.quantity) + ")";
        });
    
    // ==================== ScheduledTask ====================
    py::class_<ScheduledTask>(m, "ScheduledTask", "定时任务信息")
        .def(py::init<>())
        .def_readwrite("function_name", &ScheduledTask::function_name, "Python 方法名")
        .def_readwrite("interval_ms", &ScheduledTask::interval_ms, "执行间隔（毫秒）")
        .def_readwrite("next_run_time_ms", &ScheduledTask::next_run_time_ms, "下次执行时间（毫秒时间戳）")
        .def_readwrite("last_run_time_ms", &ScheduledTask::last_run_time_ms, "上次执行时间（毫秒时间戳）")
        .def_readwrite("enabled", &ScheduledTask::enabled, "是否启用")
        .def_readwrite("run_count", &ScheduledTask::run_count, "执行次数")
        .def("__repr__", [](const ScheduledTask& t) {
            return "ScheduledTask(" + t.function_name +
                   ", enabled=" + (t.enabled ? "True" : "False") +
                   ", count=" + std::to_string(t.run_count) + ")";
        });

    // ==================== HistoricalKline (使用 server::KlineBar) ====================
    py::class_<server::KlineBar>(m, "HistoricalKline", R"doc(
历史 K 线数据结构

用于从 Redis 查询的历史 K 线数据，支持最多 60 天的历史数据。
支持从 1 秒 K 线聚合成更大周期（1m/5m/15m/1h/4h/1d）。
    )doc")
        .def(py::init<>())
        .def_readwrite("symbol", &server::KlineBar::symbol, "交易对")
        .def_readwrite("exchange", &server::KlineBar::exchange, "交易所")
        .def_readwrite("interval", &server::KlineBar::interval, "时间周期")
        .def_property("timestamp",
            [](const server::KlineBar& k) -> int64_t { return k.timestamp; },
            [](server::KlineBar& k, int64_t value) { k.timestamp = value; },
            "开盘时间戳（毫秒）")
        .def_readwrite("open", &server::KlineBar::open, "开盘价")
        .def_readwrite("high", &server::KlineBar::high, "最高价")
        .def_readwrite("low", &server::KlineBar::low, "最低价")
        .def_readwrite("close", &server::KlineBar::close, "收盘价")
        .def_readwrite("volume", &server::KlineBar::volume, "成交量")
        .def_readwrite("turnover", &server::KlineBar::turnover, "成交额")
        .def_readwrite("is_closed", &server::KlineBar::is_closed, "是否已完结")
        .def("to_json", &server::KlineBar::to_json, "转换为 JSON")
        .def("__repr__", [](const server::KlineBar& k) {
            return "HistoricalKline(" + k.symbol + "@" + k.exchange +
                   ", ts=" + std::to_string(k.timestamp) +
                   ", c=" + std::to_string(k.close) + ")";
        });
    
    // ==================== StrategyBase ====================
    py::class_<PyStrategyBase, PyStrategyTrampoline>(m, "StrategyBase", R"doc(
        策略基类
        
        模块化设计，组合三个独立模块：
        - 行情数据：subscribe_kline, get_klines, get_closes, ...
        - 交易操作：send_swap_market_order, cancel_order, ...
        - 账户管理：register_account, get_all_positions, get_usdt_available, ...
        
        Python 策略继承此类，重写以下方法：
        - on_init(): 策略初始化
        - on_stop(): 策略停止
        - on_tick(): 每次循环调用
        - on_kline(symbol, interval, bar): K线回调
        - on_trade(symbol, trade): 逐笔成交回调
        - on_order_report(report): 订单回报回调
        - on_register_report(success, error_msg): 账户注册回报
        - on_position_update(position): 持仓更新回调
        - on_balance_update(balance): 余额更新回调
    )doc")
        // 构造函数
        .def(py::init<const std::string&, size_t, size_t, size_t, size_t, const std::string&>(),
             py::arg("strategy_id"),
             py::arg("max_kline_bars") = 7200,
             py::arg("max_trades") = 10000,
             py::arg("max_orderbook_snapshots") = 1000,
             py::arg("max_funding_rate_records") = 100,
             py::arg("log_file_path") = "",
             R"doc(
创建策略实例

Args:
    strategy_id: 策略ID
    max_kline_bars: K线最大存储数量（默认 7200 = 2小时1s K线）
    max_trades: Trades最大存储数量（默认 10000 条）
    max_orderbook_snapshots: OrderBook最大存储数量（默认 1000 个快照）
    max_funding_rate_records: FundingRate最大存储数量（默认 100 条）
    log_file_path: 日志文件路径（空字符串表示不记录到文件）
)doc")
        // 设置 Python 对象引用（内部使用，在 Python __init__ 中调用）
        .def("_set_python_self", &PyStrategyBase::set_python_self,
             py::arg("self"), py::keep_alive<1, 2>(),  // 保持 Python 对象引用
             "设置 Python 对象引用（内部使用）")
        
        // ========== 连接管理 ==========
        .def("connect", &PyStrategyBase::connect, "连接到实盘服务器")
        .def("disconnect", &PyStrategyBase::disconnect, "断开连接")
        
        // ========== 行情数据模块 ==========
        .def("subscribe_kline", &PyStrategyBase::subscribe_kline,
             py::arg("symbol"), py::arg("interval"),
             "订阅K线数据")
        .def("unsubscribe_kline", &PyStrategyBase::unsubscribe_kline,
             py::arg("symbol"), py::arg("interval"),
             "取消订阅K线")
        .def("subscribe_trades", &PyStrategyBase::subscribe_trades,
             py::arg("symbol"),
             "订阅逐笔成交数据")
        .def("unsubscribe_trades", &PyStrategyBase::unsubscribe_trades,
             py::arg("symbol"),
             "取消订阅逐笔成交")
        .def("subscribe_orderbook", &PyStrategyBase::subscribe_orderbook,
             py::arg("symbol"), py::arg("channel") = "books5",
             R"doc(
订阅深度数据（OrderBook）

Args:
    symbol: 交易对（如 BTC-USDT-SWAP）
    channel: 深度频道类型:
        - "books5" (默认): 5档，推送频率100ms
        - "books": 400档，推送频率100ms
        - "bbo-tbt": 1档，推送频率10ms
        - "books-l2-tbt": 400档，推送频率10ms
        - "books50-l2-tbt": 50档，推送频率10ms
        - "books-elp": 增强限价单深度
             )doc")
        .def("unsubscribe_orderbook", &PyStrategyBase::unsubscribe_orderbook,
             py::arg("symbol"), py::arg("channel") = "books5",
             "取消订阅深度数据")
        .def("subscribe_funding_rate", &PyStrategyBase::subscribe_funding_rate,
             py::arg("symbol"),
             "订阅资金费率数据（永续合约）")
        .def("unsubscribe_funding_rate", &PyStrategyBase::unsubscribe_funding_rate,
             py::arg("symbol"),
             "取消订阅资金费率数据")
        
        // K线数据查询
        .def("get_klines", &PyStrategyBase::get_klines,
             py::arg("symbol"), py::arg("interval"),
             "获取所有K线数据")
        .def("get_closes", &PyStrategyBase::get_closes,
             py::arg("symbol"), py::arg("interval"),
             "获取收盘价数组")
        .def("get_opens", &PyStrategyBase::get_opens,
             py::arg("symbol"), py::arg("interval"),
             "获取开盘价数组")
        .def("get_highs", &PyStrategyBase::get_highs,
             py::arg("symbol"), py::arg("interval"),
             "获取最高价数组")
        .def("get_lows", &PyStrategyBase::get_lows,
             py::arg("symbol"), py::arg("interval"),
             "获取最低价数组")
        .def("get_volumes", &PyStrategyBase::get_volumes,
             py::arg("symbol"), py::arg("interval"),
             "获取成交量数组")
        .def("get_recent_klines", &PyStrategyBase::get_recent_klines,
             py::arg("symbol"), py::arg("interval"), py::arg("n"),
             "获取最近n根K线")
        .def("get_last_kline", [](const PyStrategyBase& self, 
                                  const std::string& symbol, 
                                  const std::string& interval) -> py::object {
            KlineBar bar;
            if (self.get_last_kline(symbol, interval, bar)) {
                return py::cast(bar);
            }
            return py::none();
        }, py::arg("symbol"), py::arg("interval"),
           "获取最后一根K线，无数据返回None")
        .def("get_kline_count", &PyStrategyBase::get_kline_count,
             py::arg("symbol"), py::arg("interval"),
             "获取K线数量")
        
        // Trades数据查询
        .def("get_trades", &PyStrategyBase::get_trades,
             py::arg("symbol"),
             "获取所有成交数据")
        .def("get_recent_trades", &PyStrategyBase::get_recent_trades,
             py::arg("symbol"), py::arg("n"),
             "获取最近N条成交")
        .def("get_trades_by_time", &PyStrategyBase::get_trades_by_time,
             py::arg("symbol"), py::arg("time_ms"),
             "获取最近N毫秒内的成交")
        .def("get_last_trade", [](const PyStrategyBase& self,
                                  const std::string& symbol) -> py::object {
            TradeData trade;
            if (self.get_last_trade(symbol, trade)) {
                return py::cast(trade);
            }
            return py::none();
        }, py::arg("symbol"),
           "获取最后一条成交，无数据返回None")
        .def("get_trade_count", &PyStrategyBase::get_trade_count,
             py::arg("symbol"),
             "获取成交数量")
        
        // OrderBook数据查询
        .def("get_orderbooks", &PyStrategyBase::get_orderbooks,
             py::arg("symbol"), py::arg("channel") = "books5",
             "获取所有深度快照")
        .def("get_recent_orderbooks", &PyStrategyBase::get_recent_orderbooks,
             py::arg("symbol"), py::arg("n"), py::arg("channel") = "books5",
             "获取最近N个快照")
        .def("get_orderbooks_by_time", &PyStrategyBase::get_orderbooks_by_time,
             py::arg("symbol"), py::arg("time_ms"), py::arg("channel") = "books5",
             "获取最近N毫秒内的快照")
        .def("get_last_orderbook", [](const PyStrategyBase& self,
                                     const std::string& symbol,
                                     const std::string& channel) -> py::object {
            OrderBookSnapshot snapshot;
            if (self.get_last_orderbook(symbol, snapshot, channel)) {
                return py::cast(snapshot);
            }
            return py::none();
        }, py::arg("symbol"), py::arg("channel") = "books5",
           "获取最后一个快照，无数据返回None")
        .def("get_orderbook_count", &PyStrategyBase::get_orderbook_count,
             py::arg("symbol"), py::arg("channel") = "books5",
             "获取快照数量")
        
        // FundingRate数据查询
        .def("get_funding_rates", &PyStrategyBase::get_funding_rates,
             py::arg("symbol"),
             "获取所有资金费率数据")
        .def("get_recent_funding_rates", &PyStrategyBase::get_recent_funding_rates,
             py::arg("symbol"), py::arg("n"),
             "获取最近N条记录")
        .def("get_funding_rates_by_time", &PyStrategyBase::get_funding_rates_by_time,
             py::arg("symbol"), py::arg("time_ms"),
             "获取最近N毫秒内的记录")
        .def("get_last_funding_rate", [](const PyStrategyBase& self,
                                         const std::string& symbol) -> py::object {
            FundingRateData fr;
            if (self.get_last_funding_rate(symbol, fr)) {
                return py::cast(fr);
            }
            return py::none();
        }, py::arg("symbol"),
           "获取最后一条记录，无数据返回None")
        .def("get_funding_rate_count", &PyStrategyBase::get_funding_rate_count,
             py::arg("symbol"),
             "获取记录数量")
        
        // ========== 交易模块 ==========

        // --- 最小下单单位管理 ---
        .def("load_min_order_config", &PyStrategyBase::load_min_order_config,
             py::arg("exchange"), py::arg("config_dir") = "../strategies/configs",
             R"doc(
加载交易所最小下单单位配置文件

Args:
    exchange: 交易所名称 ("okx" 或 "binance")
    config_dir: 配置文件目录路径（默认为 "../strategies/configs"）

Returns:
    bool: 是否加载成功
             )doc")
        .def("get_min_order_quantity", &PyStrategyBase::get_min_order_quantity,
             py::arg("exchange"), py::arg("symbol"),
             R"doc(
获取指定交易对的最小下单单位

Args:
    exchange: 交易所名称
    symbol: 交易对符号

Returns:
    float: 最小下单单位，如果未找到返回0
             )doc")
        .def("calculate_order_quantity", &PyStrategyBase::calculate_order_quantity,
             py::arg("exchange"), py::arg("symbol"),
             py::arg("usdt_amount"), py::arg("current_price"),
             R"doc(
根据USDT金额计算实际下单数量

Args:
    exchange: 交易所名称 ("okx" 或 "binance")
    symbol: 交易对符号
    usdt_amount: 下单金额（USDT）
    current_price: 当前价格（从K线获取）

Returns:
    float: 实际下单数量（已四舍五入到最小单位的整数倍），失败返回0
             )doc")
        .def("send_market_order_by_usdt", &PyStrategyBase::send_market_order_by_usdt,
             py::arg("exchange"), py::arg("symbol"), py::arg("side"),
             py::arg("usdt_amount"), py::arg("current_price"),
             py::arg("pos_side") = "net",
             R"doc(
根据USDT金额下市价单（自动计算数量）

Args:
    exchange: 交易所名称 ("okx" 或 "binance")
    symbol: 交易对符号
    side: 方向 ("buy" 或 "sell")
    usdt_amount: 下单金额（USDT）
    current_price: 当前价格（从K线获取）
    pos_side: 持仓方向 (OKX: "long"/"short"/"net", Binance: "LONG"/"SHORT"/"BOTH")

Returns:
    str: 订单ID，失败返回空字符串
             )doc")
        .def("send_limit_order_by_usdt", &PyStrategyBase::send_limit_order_by_usdt,
             py::arg("exchange"), py::arg("symbol"), py::arg("side"),
             py::arg("usdt_amount"), py::arg("price"), py::arg("current_price"),
             py::arg("pos_side") = "net",
             R"doc(
根据USDT金额下限价单（自动计算数量）

Args:
    exchange: 交易所名称 ("okx" 或 "binance")
    symbol: 交易对符号
    side: 方向 ("buy" 或 "sell")
    usdt_amount: 下单金额（USDT）
    price: 限价单价格
    current_price: 当前价格（用于计算数量）
    pos_side: 持仓方向

Returns:
    str: 订单ID，失败返回空字符串
             )doc")

        // --- 原有下单接口 ---
        .def("send_swap_market_order", &PyStrategyBase::send_swap_market_order,
             py::arg("symbol"), py::arg("side"), py::arg("quantity"),
             py::arg("pos_side") = "net",
             R"doc(
发送合约市价订单 (OKX)

Args:
    symbol: 交易对（如 BTC-USDT-SWAP）
    side: "buy" 或 "sell"
    quantity: 张数
    pos_side: 持仓方向 "net"(默认), "long", "short"

Returns:
    客户端订单ID
             )doc")
        .def("send_binance_futures_market_order", &PyStrategyBase::send_binance_futures_market_order,
             py::arg("symbol"), py::arg("side"), py::arg("quantity"),
             py::arg("pos_side") = "BOTH",
             R"doc(
发送 Binance 期货市价订单

Args:
    symbol: 交易对（如 BTCUSDT）
    side: "buy" 或 "sell"
    quantity: 数量（币数，非张数）
    pos_side: 持仓方向 "BOTH"(默认/单向持仓), "LONG", "SHORT"(双向持仓)

Returns:
    客户端订单ID
             )doc")
        .def("send_binance_futures_market_order_with_price", &PyStrategyBase::send_binance_futures_market_order_with_price,
             py::arg("symbol"), py::arg("side"), py::arg("quantity"),
             py::arg("estimated_price"), py::arg("order_value"),
             py::arg("pos_side") = "BOTH",
             R"doc(
发送 Binance 期货市价订单（带价格信息，用于风控检查）

Args:
    symbol: 交易对（如 BTCUSDT）
    side: "buy" 或 "sell"
    quantity: 数量（币数）
    estimated_price: 估算价格（当前市价，用于风控计算订单金额）
    order_value: 订单金额（USDT，用于风控检查）
    pos_side: 持仓方向 "BOTH"(默认/单向持仓), "LONG", "SHORT"(双向持仓)

Returns:
    客户端订单ID
             )doc")
        .def("send_binance_futures_limit_order", &PyStrategyBase::send_binance_futures_limit_order,
             py::arg("symbol"), py::arg("side"), py::arg("quantity"),
             py::arg("price"), py::arg("pos_side") = "BOTH",
             R"doc(
发送 Binance 期货限价订单

Args:
    symbol: 交易对（如 BTCUSDT）
    side: "buy" 或 "sell"
    quantity: 数量（币数）
    price: 限价
    pos_side: 持仓方向 "BOTH"(默认/单向持仓), "LONG", "SHORT"(双向持仓)

Returns:
    客户端订单ID
             )doc")
        .def("send_swap_limit_order", &PyStrategyBase::send_swap_limit_order,
             py::arg("symbol"), py::arg("side"), py::arg("quantity"), 
             py::arg("price"), py::arg("pos_side") = "net",
             "发送合约限价订单")
        .def("send_swap_market_order_with_tp_sl", &PyStrategyBase::send_swap_market_order_with_tp_sl,
             py::arg("symbol"), py::arg("side"), py::arg("quantity"),
             py::arg("tp_trigger_px") = "", py::arg("tp_ord_px") = "",
             py::arg("sl_trigger_px") = "", py::arg("sl_ord_px") = "",
             py::arg("pos_side") = "net", py::arg("tag") = "",
             "发送合约市价订单（带止盈止损）")
        .def("send_swap_limit_order_with_tp_sl", &PyStrategyBase::send_swap_limit_order_with_tp_sl,
             py::arg("symbol"), py::arg("side"), py::arg("quantity"), py::arg("price"),
             py::arg("tp_trigger_px") = "", py::arg("tp_ord_px") = "",
             py::arg("sl_trigger_px") = "", py::arg("sl_ord_px") = "",
             py::arg("pos_side") = "net", py::arg("tag") = "",
             "发送合约限价订单（带止盈止损）")
        .def("send_swap_advanced_order", &PyStrategyBase::send_swap_advanced_order,
             py::arg("symbol"), py::arg("side"), py::arg("quantity"), py::arg("price"),
             py::arg("ord_type"), py::arg("pos_side") = "net", py::arg("tag") = "",
             "发送高级订单类型（post_only, fok, ioc等）")
        .def("send_batch_orders", &PyStrategyBase::send_batch_orders,
             py::arg("orders"), py::arg("exchange") = "okx",
             "批量下单（OKX最多20个订单，Binance最多5个订单）")
        .def("change_leverage", &PyStrategyBase::change_leverage,
             py::arg("symbol"), py::arg("leverage"), py::arg("exchange") = "binance",
             "调整杠杆倍数（仅Binance合约）")
        .def("cancel_order", &PyStrategyBase::cancel_order,
             py::arg("symbol"), py::arg("client_order_id"),
             "撤销订单")
        .def("cancel_all_orders", &PyStrategyBase::cancel_all_orders,
             py::arg("symbol") = "",
             "撤销所有订单")
        .def("get_active_orders", &PyStrategyBase::get_active_orders,
             "获取所有活跃订单")
        .def("pending_order_count", &PyStrategyBase::pending_order_count,
             "获取未完成订单数量")
        
        // ========== 账户模块 ==========
        .def("register_account", &PyStrategyBase::register_account,
             py::arg("api_key"), py::arg("secret_key"),
             py::arg("passphrase"), py::arg("is_testnet") = true,
             py::arg("account_id") = "",
             "注册 OKX 账户 (account_id 可选: 显式指定账户主键)")
        .def("register_binance_account", &PyStrategyBase::register_binance_account,
             py::arg("api_key"), py::arg("secret_key"),
             py::arg("is_testnet") = true,
             py::arg("account_id") = "",
             R"doc(
注册 Binance 账户

Args:
    api_key: Binance API Key
    secret_key: Binance Secret Key
    is_testnet: 是否使用测试网（默认True）
    account_id: 可选, 显式指定账户主键。同一 api_key 多策略复用时, 传相同的 account_id 可避免 registry 出现重复条目

Returns:
    是否发送成功
             )doc")
        .def("unregister_account", &PyStrategyBase::unregister_account,
             "注销账户")
        .def("is_account_registered", &PyStrategyBase::is_account_registered,
             "账户是否已注册")
        
        // 余额查询
        .def("get_usdt_available", &PyStrategyBase::get_usdt_available,
             "获取USDT可用余额")
        .def("get_total_equity", &PyStrategyBase::get_total_equity,
             "获取总权益（USD）")
        .def("get_all_balances", &PyStrategyBase::get_all_balances,
             "获取所有余额")
        
        // 持仓查询
        .def("get_all_positions", &PyStrategyBase::get_all_positions,
             "获取所有持仓")
        .def("get_active_positions", &PyStrategyBase::get_active_positions,
             "获取有效持仓（数量不为0）")
        .def("get_position", [](const PyStrategyBase& self,
                               const std::string& symbol,
                               const std::string& pos_side) -> py::object {
            PositionInfo pos;
            if (self.get_position(symbol, pos, pos_side)) {
                return py::cast(pos);
            }
            return py::none();
        }, py::arg("symbol"), py::arg("pos_side") = "net",
           "获取指定持仓，无数据返回None")
        
        // 刷新
        .def("refresh_account", &PyStrategyBase::refresh_account,
             "请求刷新账户信息")
        .def("refresh_positions", &PyStrategyBase::refresh_positions,
             "请求刷新持仓信息")
        .def("clear_positions", &PyStrategyBase::clear_positions,
             "清空内存���的持仓缓存（在 refresh_positions 前调用，确保获取最新全量持仓）")
        .def("is_position_query_done", &PyStrategyBase::is_position_query_done,
             "持仓查询是否已完成（C++服务端已返回响应）")
        .def("is_position_query_error", &PyStrategyBase::is_position_query_error,
             "持仓查询是否出错")
        
        // ========== 定时任务模块 ==========
        .def("schedule_task", &PyStrategyBase::schedule_task,
             py::arg("function_name"), py::arg("interval"), py::arg("start_time") = "",
             R"doc(
注册定时任务

Args:
    function_name: Python 方法名（策略类中定义的方法名，如 "do_buy_order"）
        注意: 该方法必须是策略类中已定义的方法，基类会在定时到达时直接调用
    interval: 执行间隔，格式:
        - "30s", "60s" - 秒
        - "1m", "5m", "30m" - 分钟
        - "1h", "4h" - 小时
        - "1d" - 天
        - "1w" - 周
    start_time: 首次执行时间，格式 "HH:MM"（如 "14:00"）
        - 空字符串或"now"表示立即开始
        - 如果指定时间已过，会计算下一个合适的时间

Returns:
    是否成功

Example:
    # 定义方法
    def do_buy_order(self):
        self.send_swap_market_order("BTC-USDT-SWAP", "buy", 1)
    
    # 注册定时任务（每1秒执行一次）
    self.schedule_task("do_buy_order", "1s")
    
    # 每天14:00执行
    self.schedule_task("daily_rebalance", "1d", "14:00")
             )doc")
        .def("unschedule_task", &PyStrategyBase::unschedule_task,
             py::arg("function_name"),
             "取消定时任务")
        .def("pause_task", &PyStrategyBase::pause_task,
             py::arg("function_name"),
             "暂停定时任务")
        .def("resume_task", &PyStrategyBase::resume_task,
             py::arg("function_name"),
             "恢复定时任务")
        .def("get_scheduled_tasks", &PyStrategyBase::get_scheduled_tasks,
             "获取所有定时任务")
        .def("get_task_info", [](const PyStrategyBase& self,
                                const std::string& function_name) -> py::object {
            ScheduledTask task;
            if (self.get_task_info(function_name, task)) {
                return py::cast(task);
            }
            return py::none();
        }, py::arg("function_name"),
           "获取任务信息，不存在返回None")

        // ========== 历史数据模块 ==========
        .def("connect_historical_data", &PyStrategyBase::connect_historical_data,
             R"doc(
连接到 Redis 历史数据服务

Returns:
    是否成功连接

Note:
    需要设置环境变量 REDIS_HOST, REDIS_PORT, REDIS_PASSWORD, REDIS_DB
             )doc")
        .def("get_historical_klines", &PyStrategyBase::get_historical_klines,
             py::arg("symbol"), py::arg("exchange"), py::arg("interval"),
             py::arg("start_time"), py::arg("end_time"),
             R"doc(
查询指定时间范围的历史 K 线数据

Args:
    symbol: 交易对（如 BTC-USDT-SWAP 或 BTCUSDT）
    exchange: 交易所（okx/binance）
    interval: 时间周期（1s/1m/5m/15m/1h/4h/8h/1d）
    start_time: 开始时间戳（毫秒）
    end_time: 结束时间戳（毫秒）

Returns:
    K 线数据列表（按时间升序）

Note:
    如果请求的周期没有直接存储，会自动从 1s K 线聚合
             )doc")
        .def("get_historical_klines_by_days", &PyStrategyBase::get_historical_klines_by_days,
             py::arg("symbol"), py::arg("exchange"), py::arg("interval"), py::arg("days"),
             R"doc(
查询最近 N 天的历史 K 线数据

Args:
    symbol: 交易对
    exchange: 交易所
    interval: 时间周期
    days: 天数（最大 60 天）

Returns:
    K 线数据列表（按时间升序）
             )doc")
        .def("get_latest_historical_klines", &PyStrategyBase::get_latest_historical_klines,
             py::arg("symbol"), py::arg("exchange"), py::arg("interval"), py::arg("count"),
             R"doc(
查询最近 N 根历史 K 线

Args:
    symbol: 交易对
    exchange: 交易所
    interval: 时间周期
    count: 数量

Returns:
    K 线数据列表（按时间升序）
             )doc")
        .def("get_okx_historical_klines", &PyStrategyBase::get_okx_historical_klines,
             py::arg("symbol"), py::arg("interval"), py::arg("days"),
             R"doc(
获取 OKX 历史 K 线（便捷方法）

Args:
    symbol: 交易对（如 BTC-USDT-SWAP）
    interval: 时间周期
    days: 天数

Returns:
    K 线数据列表
             )doc")
        .def("get_binance_historical_klines", &PyStrategyBase::get_binance_historical_klines,
             py::arg("symbol"), py::arg("interval"), py::arg("days"),
             R"doc(
获取 Binance 历史 K 线（便捷方法）

Args:
    symbol: 交易对（如 BTCUSDT）
    interval: 时间周期
    days: 天数

Returns:
    K 线数据列表
             )doc")
        .def("get_historical_closes", &PyStrategyBase::get_historical_closes,
             py::arg("symbol"), py::arg("exchange"), py::arg("interval"), py::arg("days"),
             R"doc(
获取历史收盘价数组

Args:
    symbol: 交易对
    exchange: 交易所
    interval: 时间周期
    days: 天数

Returns:
    收盘价数组（按时间升序）
             )doc")
        .def("get_available_historical_symbols", &PyStrategyBase::get_available_historical_symbols,
             py::arg("exchange") = "",
             R"doc(
获取可用的历史数据交易对列表

Args:
    exchange: 交易所（可选，空则返回所有）

Returns:
    交易对列表
             )doc")
        .def("get_historical_data_time_range", &PyStrategyBase::get_historical_data_time_range,
             py::arg("symbol"), py::arg("exchange"), py::arg("interval"),
             R"doc(
获取指定交易对的历史数据时间范围

Args:
    symbol: 交易对
    exchange: 交易所
    interval: 时间周期

Returns:
    (earliest_timestamp, latest_timestamp) 元组
             )doc")
        .def("get_historical_kline_count", &PyStrategyBase::get_historical_kline_count,
             py::arg("symbol"), py::arg("exchange"), py::arg("interval"),
             R"doc(
获取指定交易对的历史 K 线数量

Args:
    symbol: 交易对
    exchange: 交易所
    interval: 时间周期

Returns:
    K 线数量
             )doc")

        .def("batch_get_latest_kline_timestamps", &PyStrategyBase::batch_get_latest_kline_timestamps,
             py::arg("symbols"), py::arg("exchange"), py::arg("interval"),
             R"doc(
批量获取多个币种最新K线时间戳（Redis Pipeline，单次往返，<1ms）

Args:
    symbols: 交易对列表
    exchange: 交易所
    interval: 时间周期

Returns:
    dict: {symbol: latest_timestamp_ms}
             )doc")

        .def("batch_get_latest_klines", &PyStrategyBase::batch_get_latest_klines,
             py::arg("symbols"), py::arg("exchange"), py::arg("interval"),
             R"doc(
批量获取多个币种最新1根K线数据（Redis Pipeline，单次往返）

Args:
    symbols: 交易对列表
    exchange: 交易所
    interval: 时间周期

Returns:
    dict: {symbol: KlineBar}
             )doc")

        .def("lua_batch_get_latest_timestamps", &PyStrategyBase::lua_batch_get_latest_timestamps,
             py::arg("symbols"), py::arg("exchange"), py::arg("interval"),
             R"doc(
Lua脚本批量获取最新K线时间戳（Redis服务端执行，单次EVALSHA，<0.5ms）

Args:
    symbols: 交易对列表
    exchange: 交易所
    interval: 时间周期

Returns:
    dict: {symbol: latest_timestamp_ms}
             )doc")

        // ========== 运行控制 ==========
        .def("run", &PyStrategyBase::run, py::call_guard<py::gil_scoped_release>(),
             "运行策略（主循环）")
        .def("stop", &PyStrategyBase::stop, "停止策略")
        .def("poll_messages", &PyStrategyBase::poll_messages,
             py::call_guard<py::gil_scoped_release>(),
             "手动处理一轮ZMQ消息（在等待期间调用，避免sleep阻塞主循环）")
        
        // ========== 虚函数（供 Python 重写）==========
        .def("on_init", &PyStrategyBase::on_init, "策略初始化回调")
        .def("on_stop", &PyStrategyBase::on_stop, "策略停止回调")
        .def("on_tick", &PyStrategyBase::on_tick, "每次循环回调")
        .def("on_kline", &PyStrategyBase::on_kline,
             py::arg("symbol"), py::arg("interval"), py::arg("bar"),
             "K线回调")
        .def("on_trade", &PyStrategyBase::on_trade,
             py::arg("symbol"), py::arg("trade"),
             "逐笔成交回调")
        .def("on_orderbook", &PyStrategyBase::on_orderbook,
             py::arg("symbol"), py::arg("snapshot"),
             "深度数据回调")
        .def("on_funding_rate", &PyStrategyBase::on_funding_rate,
             py::arg("symbol"), py::arg("fr"),
             "资金费率回调")
        .def("on_order_report", &PyStrategyBase::on_order_report,
             py::arg("report"), "订单回报回调")
        .def("on_register_report", &PyStrategyBase::on_register_report,
             py::arg("success"), py::arg("error_msg"),
             "账户注册回报回调")
        .def("on_position_update", &PyStrategyBase::on_position_update,
             py::arg("position"),
             "持仓更新回调")
        .def("on_balance_update", &PyStrategyBase::on_balance_update,
             py::arg("balance"),
             "余额更新回调")
        .def("on_account_update", &PyStrategyBase::on_account_update,
             py::arg("total_equity"), py::arg("margin_ratio"),
             "账户更新回调")

        // ========== 日志 ==========
        .def("log_info", &PyStrategyBase::log_info, 
             py::arg("msg"), "输出信息日志")
        .def("log_error", &PyStrategyBase::log_error, 
             py::arg("msg"), "输出错误日志")
        
        // ========== 属性 ==========
        .def_property_readonly("strategy_id", &PyStrategyBase::strategy_id, "策略ID")
        .def_property_readonly("is_running", &PyStrategyBase::is_running, "是否运行中")
        .def_property_readonly("kline_count", &PyStrategyBase::kline_count, "接收的K线数量")
        .def_property_readonly("order_count", &PyStrategyBase::order_count, "发送的订单数量")
        .def_property_readonly("report_count", &PyStrategyBase::report_count, "收到的回报数量");
}
