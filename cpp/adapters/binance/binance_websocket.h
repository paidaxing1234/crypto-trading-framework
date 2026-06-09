#pragma once

/**
 * @file binance_websocket.h
 * @brief Binance WebSocket 客户端
 * 
 * 功能：
 * - WebSocket API: 通过WebSocket进行交易（更低延迟）
 * - 行情推送: 实时行情、深度、成交、K线数据
 * - 账户推送: 实时账户更新、订单更新
 * 
 * URL 端点:
 * - WebSocket API: wss://ws-api.binance.com/ws-api/v3
 * - 行情推送: wss://stream.binance.com:9443/ws/<streamName>
 * - 账户推送: wss://stream.binance.com:9443/ws/<listenKey>
 * 
 * 测试网URL:
 * - WebSocket API: wss://ws-api.testnet.binance.vision/ws-api/v3
 * - 行情推送: wss://stream.testnet.binance.vision/ws/<streamName>
 * 
 * 参考文档:
 * https://developers.binance.com/docs/zh-CN/binance-spot-api-docs/websocket-api
 * 
 * @author Sequence Team
 * @date 2024-12
 */

#include "binance_rest_api.h"  // 必须先 include，提供 OrderSide/OrderType/TimeInForce/PositionSide
#include "../../core/data.h"
#include "../../trading/order.h"
#include "../../network/ws_client.h"

// 前向声明
namespace trading {
namespace binance {
class BinanceRestAPI;
}
}
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <nlohmann/json.hpp>

namespace trading {
namespace binance {

// ==================== WebSocket类型 ====================

/**
 * @brief WebSocket连接类型
 */
enum class WsConnectionType {
    TRADING,    // 交易API（低延迟下单、撤单）
    MARKET,     // 行情推送（Ticker、深度、K线等）
    USER        // 用户数据流（账户/订单推送）
};

/**
 * @brief 市场数据流类型
 */
enum class StreamType {
    TRADE,          // 逐笔成交
    KLINE,          // K线
    MINI_TICKER,    // 精简Ticker
    TICKER,         // 完整Ticker
    DEPTH,          // 深度
    BOOK_TICKER,    // 最优挂单
    AGG_TRADE       // 归集交易
};

// ==================== 标记价格数据结构 ====================

/**
 * @brief 标记价格数据（仅合约）
 */
struct MarkPriceData {
    using Ptr = std::shared_ptr<MarkPriceData>;
    
    std::string symbol;           // 交易对
    double mark_price;            // 标记价格
    double index_price;           // 现货指数价格
    double funding_rate;          // 资金费率
    int64_t next_funding_time;    // 下次资金时间（毫秒）
    int64_t timestamp;            // 事件时间
    
    MarkPriceData() 
        : mark_price(0.0)
        , index_price(0.0)
        , funding_rate(0.0)
        , next_funding_time(0)
        , timestamp(0)
    {}
};

// ==================== 回调类型定义 ====================

/**
 * @brief 各类数据的回调函数类型
 *
 * 使用原始JSON格式，不做归一化处理
 * OKX 和 Binance 各自保持原始数据结构
 */
using TradeCallback = std::function<void(const nlohmann::json&)>;        // Trade原始JSON
using KlineCallback = std::function<void(const nlohmann::json&)>;        // Kline原始JSON
using TickerCallback = std::function<void(const nlohmann::json&)>;       // Ticker原始JSON
using OrderBookCallback = std::function<void(const nlohmann::json&)>;    // OrderBook原始JSON
using MarkPriceCallback = std::function<void(const nlohmann::json&)>;    // MarkPrice原始JSON
using RawMessageCallback = std::function<void(const nlohmann::json&)>;
using AccountUpdateCallback = std::function<void(const nlohmann::json&)>;
using OrderTradeUpdateCallback = std::function<void(const nlohmann::json&)>;  // 订单成交更新回调

// WebSocket Trading API 响应回调
using OrderResponseCallback = std::function<void(const nlohmann::json&)>;

// ==================== BinanceWebSocket类 ====================

/**
 * @brief Binance WebSocket客户端
 * 
 * 支持三种连接类型：
 * 1. 交易API - 通过WebSocket下单、撤单（低延迟）
 * 2. 行情推送 - 实时行情数据订阅
 * 3. 用户数据 - 实时账户和订单更新
 * 
 * 使用示例：
 * @code
 * // 创建交易API连接
 * BinanceWebSocket ws_trading(api_key, secret_key, WsConnectionType::TRADING);
 * ws_trading.connect();
 * 
 * // WebSocket下单（比REST API更快）
 * ws_trading.place_order_ws("BTCUSDT", OrderSide::BUY, OrderType::LIMIT, 
 *                           "0.001", "50000");
 * 
 * // 创建行情推送连接
 * BinanceWebSocket ws_market("", "", WsConnectionType::MARKET);
 * ws_market.connect();
 * ws_market.subscribe_trade("btcusdt");
 * ws_market.subscribe_kline("btcusdt", "1m");
 * @endcode
 */
class BinanceWebSocket {
public:
    /**
     * @brief 构造函数
     *
     * @param api_key API密钥（交易API和用户数据流需要）
     * @param secret_key Secret密钥（交易API需要）
     * @param conn_type 连接类型
     * @param market_type 市场类型（现货/合约）
     * @param is_testnet 是否使用测试网
     * @param ws_config WebSocket配置（SSL验证、代理等）
     */
    BinanceWebSocket(
        const std::string& api_key = "",
        const std::string& secret_key = "",
        WsConnectionType conn_type = WsConnectionType::MARKET,
        MarketType market_type = MarketType::SPOT,
        bool is_testnet = false,
        const core::WebSocketConfig& ws_config = {}
    );
    
    ~BinanceWebSocket();
    
    // ==================== 连接管理 ====================
    
    /**
     * @brief 连接WebSocket
     */
    bool connect();

    /**
     * @brief 连接WebSocket并直接在URL中指定streams（组合流方式）
     *
     * 使用URL格式: wss://fstream.binance.com/stream?streams=stream1/stream2/...
     * 这种方式比SUBSCRIBE消息更可靠，特别是订阅大量streams时
     *
     * @param streams stream列表，如 ["btcusdt_perpetual@continuousKline_1m", ...]
     * @return 是否连接成功
     */
    bool connect_with_streams(const std::vector<std::string>& streams);

    /**
     * @brief 断开连接
     */
    void disconnect();
    
    /**
     * @brief 是否已连接
     */
    bool is_connected() const { return is_connected_.load(); }

    /**
     * @brief 启用/禁用自动重连
     */
    void set_auto_reconnect(bool enabled);
    
    // ==================== WebSocket交易API（低延迟下单） ====================
    
    /**
     * @brief WebSocket下单
     * 
     * 延迟比 REST 低 5-10 倍，适合高频交易
     * 
     * @param symbol 交易对，如 "BTCUSDT"
     * @param side 买卖方向
     * @param type 订单类型
     * @param quantity 数量
     * @param price 价格（限价单必填）
     * @param time_in_force 时间有效性
     * @param position_side 持仓方向（合约）
     * @param client_order_id 客户自定义订单ID
     * @return 请求ID（用于匹配响应）
     */
    std::string place_order_ws(
        const std::string& symbol,
        OrderSide side,
        OrderType type,
        const std::string& quantity,
        const std::string& price = "",
        TimeInForce time_in_force = TimeInForce::GTC,
        PositionSide position_side = PositionSide::BOTH,
        const std::string& client_order_id = ""
    );
    
    /**
     * @brief WebSocket撤单
     * 
     * @param symbol 交易对
     * @param order_id 订单ID
     * @param client_order_id 客户自定义订单ID
     * @return 请求ID
     */
    std::string cancel_order_ws(
        const std::string& symbol,
        int64_t order_id = 0,
        const std::string& client_order_id = ""
    );
    
    /**
     * @brief WebSocket查询订单
     * 
     * @param symbol 交易对
     * @param order_id 订单ID
     * @param client_order_id 客户自定义订单ID
     * @return 请求ID
     */
    std::string query_order_ws(
        const std::string& symbol,
        int64_t order_id = 0,
        const std::string& client_order_id = ""
    );
    
    /**
     * @brief WebSocket修改订单
     * 
     * 仅支持限价单（LIMIT）修改
     * 
     * @param symbol 交易对
     * @param side 买卖方向
     * @param quantity 新数量
     * @param price 新价格
     * @param order_id 订单ID
     * @param client_order_id 客户自定义订单ID
     * @param position_side 持仓方向（合约）
     * @return 请求ID
     */
    std::string modify_order_ws(
        const std::string& symbol,
        OrderSide side,
        const std::string& quantity,
        const std::string& price,
        int64_t order_id = 0,
        const std::string& client_order_id = "",
        PositionSide position_side = PositionSide::BOTH
    );

    std::string start_user_data_stream_ws();
    std::string ping_user_data_stream_ws();
    
    /**
     * @brief 设置订单响应回调（仿照 OKX）
     */
    void set_order_response_callback(OrderResponseCallback callback) {
        order_response_callback_ = std::move(callback);
    }
    
    // ==================== 行情推送订阅（已测试） ====================
    
    /**
     * @brief 订阅逐笔成交流
     * 
     * @param symbol 交易对（小写），如 "btcusdt"
     * 
     * 推送示例：
     * {
     *   "e": "trade",
     *   "s": "BTCUSDT",
     *   "t": 12345,
     *   "p": "0.001",
     *   "q": "100",
     *   "T": 123456785
     * }
     */
    void subscribe_trade(const std::string& symbol);

    /**
     * @brief 批量订阅多个stream（一次请求）
     *
     * @param streams stream列表，如 ["btcusdt@trade", "ethusdt@kline_1m"]
     */
    void subscribe_streams_batch(const std::vector<std::string>& streams);

    /**
     * @brief 批量订阅逐笔成交（一次请求订阅多个币种）
     *
     * @param symbols 交易对列表（小写）
     */
    void subscribe_trades_batch(const std::vector<std::string>& symbols);

    /**
     * @brief 批量订阅K线（一次请求订阅多个币种）
     *
     * @param symbols 交易对列表（小写）
     * @param interval K线间隔
     */
    void subscribe_klines_batch(const std::vector<std::string>& symbols, const std::string& interval);

    /**
     * @brief 批量订阅深度（一次请求订阅多个币种）
     *
     * @param symbols 交易对列表（小写）
     * @param levels 档位数
     * @param update_speed 更新速度(ms)
     */
    void subscribe_depths_batch(const std::vector<std::string>& symbols, int levels = 20, int update_speed = 100);

    /**
     * @brief 订阅K线流
     * 
     * @param symbol 交易对（小写）
     * @param interval K线间隔（1m, 3m, 5m, 15m, 30m, 1h, 2h, 4h, 6h, 8h, 12h, 1d, 3d, 1w, 1M）
     */
    void subscribe_kline(const std::string& symbol, const std::string& interval);
    
    /**
     * @brief 订阅精简Ticker
     * 
     * @param symbol 交易对（小写），空则订阅所有交易对
     */
    void subscribe_mini_ticker(const std::string& symbol = "");
    
    /**
     * @brief 订阅完整Ticker
     * 
     * @param symbol 交易对（小写），空则订阅所有交易对
     */
    void subscribe_ticker(const std::string& symbol = "");
    
    /**
     * @brief 订阅深度信息
     * 
     * @param symbol 交易对（小写）
     * @param levels 档位数量（5, 10, 20）
     * @param update_speed 更新速度（1000ms 或 100ms）
     */
    void subscribe_depth(
        const std::string& symbol,
        int levels = 20,
        int update_speed = 1000
    );
    
    /**
     * @brief 订阅最优挂单
     * 
     * @param symbol 交易对（小写）
     */
    void subscribe_book_ticker(const std::string& symbol);
    
    /**
     * @brief 订阅标记价格（仅合约）
     * 
     * @param symbol 交易对（小写），如 "btcusdt"
     * @param update_speed 更新速度：3000(默认) 或 1000(@1s)
     */
    void subscribe_mark_price(const std::string& symbol, int update_speed = 3000);
    
    /**
     * @brief 订阅全市场标记价格（仅合约）
     * 
     * 一次性订阅所有交易对的标记价格+资金费率
     * 推送数组格式：[{s:"BTCUSDT", p:"...", r:"...", T:...}, ...]
     * 
     * @param update_speed 更新速度：3000(默认) 或 1000(@1s)
     */
    void subscribe_all_mark_prices(int update_speed = 3000);
    
    /**
     * @brief 取消订阅
     */
    void unsubscribe(const std::string& stream_name);
    
    // ==================== 回调设置（已测试的行情推送） ====================
    
    void set_trade_callback(TradeCallback callback) { 
        trade_callback_ = std::move(callback); 
    }
    
    void set_kline_callback(KlineCallback callback) { 
        kline_callback_ = std::move(callback); 
    }
    
    void set_ticker_callback(TickerCallback callback) { 
        ticker_callback_ = std::move(callback); 
    }
    
    void set_orderbook_callback(OrderBookCallback callback) { 
        orderbook_callback_ = std::move(callback); 
    }
    
    void set_mark_price_callback(MarkPriceCallback callback) { 
        mark_price_callback_ = std::move(callback); 
    }
    
    void set_raw_message_callback(RawMessageCallback callback) { 
        raw_callback_ = std::move(callback); 
    }

    void set_account_update_callback(AccountUpdateCallback callback) {
        account_update_callback_ = std::move(callback);
    }
    
    /**
     * @brief 设置订单成交更新回调（ORDER_TRADE_UPDATE）
     */
    void set_order_trade_update_callback(OrderTradeUpdateCallback callback) {
        order_trade_update_callback_ = std::move(callback);
    }

    bool connect_user_stream(const std::string& listen_key);
    
    /**
     * @brief 启动自动刷新 listenKey（每50分钟刷新一次）
     * 
     * @param rest_api REST API 客户端（用于刷新 listenKey）
     * @param interval_seconds 刷新间隔（秒），默认3000秒（50分钟）
     */
    void start_auto_refresh_listen_key(
        BinanceRestAPI* rest_api,
        int interval_seconds = 3000
    );
    
    /**
     * @brief 停止自动刷新 listenKey
     */
    void stop_auto_refresh_listen_key();
    
    // ==================== 工具方法 ====================

    /**
     * @brief 获取连接类型
     */
    WsConnectionType get_connection_type() const { return conn_type_; }

    /**
     * @brief 获取WebSocket URL
     */
    const std::string& get_url() const { return ws_url_; }

private:
    /**
     * @brief 重新订阅所有 streams（重连后调用）
     */
    void resubscribe_all();
    // 内部方法
    std::string build_ws_url() const;
    void run();
    void on_message(const std::string& message);
    bool send_message(const nlohmann::json& msg);
    
    // 消息解析
    void parse_trade(const nlohmann::json& data);
    void parse_kline(const nlohmann::json& data);
    void parse_ticker(const nlohmann::json& data);
    void parse_depth(const nlohmann::json& data);
    void parse_book_ticker(const nlohmann::json& data);
    void parse_mark_price(const nlohmann::json& data);
    void parse_account_update(const nlohmann::json& data);
    void parse_order_trade_update(const nlohmann::json& data);
    
    // WebSocket Trading API 辅助方法
    std::string generate_request_id();
    std::string create_signature(const std::string& query_string);
    std::string order_side_to_string(OrderSide side);
    std::string order_type_to_string(OrderType type);
    std::string time_in_force_to_string(TimeInForce tif);
    std::string position_side_to_string(PositionSide ps);
    int64_t get_timestamp();
    
    // 成员变量
    std::string api_key_;
    std::string secret_key_;
    std::string ws_url_;
    WsConnectionType conn_type_;
    MarketType market_type_;
    bool is_testnet_;
    std::string listen_key_;
    core::WebSocketConfig ws_config_;  // WebSocket配置

    // 状态
    std::atomic<bool> is_running_{false};
    std::atomic<bool> is_connected_{false};
    std::atomic<bool> is_disconnected_{false};  // 防止重复断开

    // 线程
    std::unique_ptr<std::thread> recv_thread_;
    std::unique_ptr<std::thread> ping_thread_;
    std::unique_ptr<std::thread> reconnect_monitor_thread_;  // 重连监控线程
    std::atomic<bool> reconnect_enabled_{false};  // 是否启用自动重连
    std::atomic<bool> need_reconnect_{false};     // 是否需要重连
    std::atomic<bool> use_combined_stream_url_{false};  // 是否使用组合流URL模式（重连时不需要发送SUBSCRIBE）
    std::mutex reconnect_mutex_;                  // 重连条件变量互斥锁
    std::condition_variable reconnect_cv_;        // 重连条件变量（用于快速唤醒）
    
    // listenKey 自动刷新
    std::atomic<bool> refresh_running_{false};
    std::unique_ptr<std::thread> refresh_thread_;
    BinanceRestAPI* rest_api_for_refresh_{nullptr};
    int refresh_interval_seconds_{3300};
    
    // 订阅列表
    std::unordered_map<std::string, std::string> subscriptions_;
    mutable std::mutex subscriptions_mutex_;

    // 用于无 event 字段的部分订阅消息（如 depth<levels> 快照）做兜底标记
    std::string last_depth_symbol_;
    
    // 回调函数
    TradeCallback trade_callback_;
    KlineCallback kline_callback_;
    TickerCallback ticker_callback_;
    OrderBookCallback orderbook_callback_;
    MarkPriceCallback mark_price_callback_;
    OrderResponseCallback order_response_callback_;  // WebSocket Trading 响应
    RawMessageCallback raw_callback_;
    AccountUpdateCallback account_update_callback_;
    OrderTradeUpdateCallback order_trade_update_callback_;  // 订单成交更新
    
    // 请求ID计数器（用于订阅消息）
    std::atomic<uint64_t> request_id_counter_{0};

    // WebSocket实现（使用公共 WebSocketClient）
    std::shared_ptr<core::WebSocketClient> impl_;
};

// ==================== 便捷工厂函数 ====================

/**
 * @brief 创建交易API WebSocket（低延迟下单）
 */
inline std::unique_ptr<BinanceWebSocket> create_trading_ws(
    const std::string& api_key,
    const std::string& secret_key,
    MarketType market_type = MarketType::SPOT,
    bool is_testnet = false
) {
    return std::make_unique<BinanceWebSocket>(
        api_key, secret_key, WsConnectionType::TRADING, market_type, is_testnet
    );
}

/**
 * @brief 创建行情推送WebSocket
 */
inline std::unique_ptr<BinanceWebSocket> create_market_ws(
    MarketType market_type = MarketType::SPOT,
    bool is_testnet = false
) {
    return std::make_unique<BinanceWebSocket>(
        "", "", WsConnectionType::MARKET, market_type, is_testnet
    );
}

inline std::unique_ptr<BinanceWebSocket> create_user_ws(
    const std::string& api_key,
    MarketType market_type = MarketType::SPOT,
    bool is_testnet = false
) {
    return std::make_unique<BinanceWebSocket>(
        api_key, "", WsConnectionType::USER, market_type, is_testnet
    );
}

} // namespace binance
} // namespace trading
