/**
 * @file zmq_server.h
 * @brief ZeroMQ 服务端核心封装
 * 
 * 功能：
 * 1. 行情发布（PUB-SUB模式）- 一对多广播
 * 2. 订单接收（PULL模式）- 多对一汇聚
 * 3. 回报发布（PUB-SUB模式）- 一对多广播
 * 
 * 使用 IPC 通道（Unix Domain Socket），延迟 30-100μs
 * 
 * 原理说明：
 * - PUB-SUB：发布者发送消息，所有订阅者都能收到（广播）
 * - PUSH-PULL：多个发送者推送，一个接收者拉取（负载均衡/汇聚）
 * 
 * @author Sequence Team
 * @date 2025-12
 */

#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <zmq.hpp>
#include <nlohmann/json.hpp>

namespace trading {
namespace server {

// ============================================================
// IPC 地址配置
// ============================================================

/**
 * @brief IPC 通道地址
 *
 * 使用 ipc:// 协议，底层是 Unix Domain Socket
 * 比 tcp://localhost 延迟低 3-5 倍
 */
struct IpcAddresses {
    // 行情通道：服务端 PUB，策略端 SUB（统一通道）
    static constexpr const char* MARKET_DATA = "ipc:///tmp/seq_md.ipc";

    // 按交易所分离的行情通道
    static constexpr const char* MARKET_DATA_OKX = "ipc:///tmp/seq_md_okx.ipc";
    static constexpr const char* MARKET_DATA_BINANCE = "ipc:///tmp/seq_md_binance.ipc";

    // 订单通道：策略端 PUSH，服务端 PULL
    static constexpr const char* ORDER = "ipc:///tmp/seq_order.ipc";

    // 回报通道：服务端 PUB，策略端 SUB
    static constexpr const char* REPORT = "ipc:///tmp/seq_report.ipc";

    // 查询通道：策略端 REQ，服务端 REP
    static constexpr const char* QUERY = "ipc:///tmp/seq_query.ipc";

    // 订阅管理：策略端 PUSH，服务端 PULL
    static constexpr const char* SUBSCRIBE = "ipc:///tmp/seq_subscribe.ipc";
};

// ============================================================
// 消息类型枚举
// ============================================================

/**
 * @brief 消息类型
 * 
 * 用于区分不同类型的消息，便于策略端过滤处理
 */
enum class MessageType {
    // 行情类
    TICKER = 1,          // 行情快照
    DEPTH = 2,           // 深度数据
    TRADE = 3,           // 逐笔成交
    KLINE = 4,           // K线数据
    
    // 订单类
    ORDER_REQUEST = 10,  // 订单请求（策略 -> 服务端）
    ORDER_REPORT = 11,   // 订单回报（服务端 -> 策略）
    ORDER_CANCEL = 12,   // 撤单请求
    
    // 系统类
    HEARTBEAT = 99,      // 心跳
    ERROR = 100,         // 错误消息
};

// ============================================================
// ZeroMQ 服务端类
// ============================================================

/**
 * @brief ZeroMQ 服务端
 * 
 * 管理三个 ZeroMQ 通道：
 * 1. market_pub_ : 发布行情数据
 * 2. order_pull_ : 接收订单请求
 * 3. report_pub_ : 发布订单回报
 * 
 * 使用方法：
 * @code
 * ZmqServer server;
 * server.start();
 * 
 * // 发布行情
 * server.publish_ticker(ticker_json);
 * 
 * // 接收订单（非阻塞）
 * std::string order_msg;
 * if (server.recv_order(order_msg)) {
 *     // 处理订单...
 * }
 * 
 * // 发布回报
 * server.publish_report(report_json);
 * 
 * server.stop();
 * @endcode
 */
class ZmqServer {
public:
    // 回调类型定义
    using OrderCallback = std::function<void(const nlohmann::json& order)>;
    using QueryCallback = std::function<nlohmann::json(const nlohmann::json& request)>;
    using SubscribeCallback = std::function<void(const nlohmann::json& request)>;
    
    /**
     * @brief 构造函数
     *
     * 初始化 ZeroMQ context，但不创建 socket
     * socket 在 start() 中创建
     *
     * @param mode 服务器模式: 0=实盘, 1=模拟盘, 2=WebSocket服务器
     */
    ZmqServer(int mode = 0);
    
    /**
     * @brief 析构函数
     * 
     * 自动停止服务并释放资源
     */
    ~ZmqServer();
    
    // 禁止拷贝
    ZmqServer(const ZmqServer&) = delete;
    ZmqServer& operator=(const ZmqServer&) = delete;
    
    // ========================================
    // 生命周期管理
    // ========================================
    
    /**
     * @brief 启动服务
     * 
     * 创建并绑定三个 ZeroMQ socket：
     * 1. market_pub_ : bind(MARKET_DATA)
     * 2. order_pull_ : bind(ORDER)
     * 3. report_pub_ : bind(REPORT)
     * 
     * @return true 启动成功
     * @return false 启动失败（可能端口被占用）
     */
    bool start();

    /**
     * @brief 停止服务
     * 
     * 关闭所有 socket，清理 IPC 文件
     */
    void stop();
    
    /**
     * @brief 检查是否运行中
     */
    bool is_running() const { return running_.load(); }
    
    // ========================================
    // 行情发布（服务端 -> 策略端）
    // ========================================
    
    /**
     * @brief 发布行情数据
     * 
     * 将 JSON 格式的行情数据通过 PUB socket 广播给所有订阅者
     * 
     * @param ticker_data JSON 格式的行情数据，包含：
     *   - type: "ticker"
     *   - symbol: 交易对
     *   - last_price, bid_price, ask_price, volume 等
     * 
     * @return true 发送成功
     * @return false 发送失败
     * 
     * 示例：
     * @code
     * nlohmann::json ticker = {
     *     {"type", "ticker"},
     *     {"symbol", "BTC-USDT"},
     *     {"last_price", 43250.5},
     *     {"bid_price", 43250.0},
     *     {"ask_price", 43251.0},
     *     {"timestamp", 1702000000000}
     * };
     * server.publish_ticker(ticker);
     * @endcode
     */
    bool publish_ticker(const nlohmann::json& ticker_data);
    
    /**
     * @brief 发布深度数据
     * 
     * @param depth_data JSON 格式的深度数据
     */
    bool publish_depth(const nlohmann::json& depth_data);
    
    /**
     * @brief 发布任意行情消息
     * 
     * @param data 消息内容（JSON）
     * @param msg_type 消息类型
     */
    bool publish_market(const nlohmann::json& data, MessageType msg_type);
    
    // ========================================
    // 订单接收（策略端 -> 服务端）
    // ========================================
    
    /**
     * @brief 非阻塞接收订单
     * 
     * 尝试从 PULL socket 接收订单消息
     * 如果没有消息，立即返回 false
     * 
     * @param order_msg 输出参数，接收到的订单 JSON 字符串
     * @return true 收到订单
     * @return false 没有订单（队列为空）
     * 
     * 示例：
     * @code
     * std::string msg;
     * if (server.recv_order(msg)) {
     *     auto order = nlohmann::json::parse(msg);
     *     // 处理订单...
     * }
     * @endcode
     */
    bool recv_order(std::string& order_msg);
    
    /**
     * @brief 非阻塞接收订单（JSON版本）
     * 
     * @param order 输出参数，解析后的订单 JSON
     * @return true 收到订单
     * @return false 没有订单
     */
    bool recv_order_json(nlohmann::json& order);
    
    /**
     * @brief 设置订单接收回调
     * 
     * 设置后，可以使用 poll_orders() 自动调用回调
     * 
     * @param callback 订单处理回调函数
     */
    void set_order_callback(OrderCallback callback) {
        order_callback_ = std::move(callback);
    }
    
    /**
     * @brief 轮询订单
     * 
     * 非阻塞地接收所有待处理订单，并调用回调
     * 
     * @return 处理的订单数量
     */
    int poll_orders();
    
    // ========================================
    // 查询处理（REQ-REP模式）
    // ========================================
    
    /**
     * @brief 设置查询回调
     * 
     * 回调函数接收请求JSON，返回响应JSON
     */
    void set_query_callback(QueryCallback callback) {
        query_callback_ = std::move(callback);
    }
    
    /**
     * @brief 处理一个查询请求
     * 
     * 非阻塞，如果没有请求返回 false
     * 
     * @return true 处理了一个请求
     */
    bool handle_query();
    
    /**
     * @brief 轮询所有查询请求
     * 
     * @return 处理的请求数量
     */
    int poll_queries();
    
    // ========================================
    // 订阅管理
    // ========================================
    
    /**
     * @brief 设置订阅回调
     */
    void set_subscribe_callback(SubscribeCallback callback) {
        subscribe_callback_ = std::move(callback);
    }
    
    /**
     * @brief 轮询订阅请求
     * 
     * @return 处理的请求数量
     */
    int poll_subscriptions();
    
    /**
     * @brief 发布K线数据
     */
    bool publish_kline(const nlohmann::json& kline_data);

    /**
     * @brief 发布带主题的行情数据
     *
     * 使用 ZMQ 主题过滤，策略端可以只订阅感兴趣的数据
     * 主题格式: {exchange}.{type}.{symbol}.{interval}
     * 例如: okx.kline.BTC-USDT.1m
     *
     * @param topic 主题前缀
     * @param data 消息内容（JSON）
     * @return true 发送成功
     */
    bool publish_with_topic(const std::string& topic, const nlohmann::json& data);

    /**
     * @brief 发布 OKX 行情数据到专用通道
     */
    bool publish_okx_market(const nlohmann::json& data, MessageType msg_type);

    /**
     * @brief 发布 Binance 行情数据到专用通道
     */
    bool publish_binance_market(const nlohmann::json& data, MessageType msg_type);
    
    // ========================================
    // 回报发布（服务端 -> 策略端）
    // ========================================
    
    /**
     * @brief 发布订单回报
     * 
     * 将订单执行结果广播给所有策略
     * 策略根据 strategy_id 过滤自己的回报
     * 
     * @param report_data JSON 格式的回报数据，包含：
     *   - type: "order_report"
     *   - strategy_id: 策略ID
     *   - client_order_id: 客户端订单ID
     *   - exchange_order_id: 交易所订单ID
     *   - status: "accepted"/"filled"/"cancelled"/"rejected"
     *   - filled_price, filled_quantity, fee 等
     * 
     * @return true 发送成功
     * @return false 发送失败
     */
    bool publish_report(const nlohmann::json& report_data);
    
    // ========================================
    // 统计信息
    // ========================================
    
    /**
     * @brief 获取已发布的行情消息数量
     */
    uint64_t get_market_msg_count() const { return market_msg_count_.load(); }
    
    /**
     * @brief 获取已接收的订单数量
     */
    uint64_t get_order_recv_count() const { return order_recv_count_.load(); }
    
    /**
     * @brief 获取已发布的回报数量
     */
    uint64_t get_report_msg_count() const { return report_msg_count_.load(); }

private:
    /**
     * @brief 发送消息到指定 socket
     * 
     * @param socket ZeroMQ socket
     * @param data 消息内容
     * @return true 发送成功
     */
    bool send_message(zmq::socket_t& socket, const std::string& data);
    
    /**
     * @brief 非阻塞接收消息
     * 
     * @param socket ZeroMQ socket
     * @param data 输出参数
     * @return true 收到消息
     */
    bool recv_message(zmq::socket_t& socket, std::string& data);

private:
    // ZeroMQ 上下文（线程安全，可共享）
    zmq::context_t context_;

    // IPC 地址
    const char* market_data_addr_;
    const char* market_data_okx_addr_;
    const char* market_data_binance_addr_;
    const char* order_addr_;
    const char* report_addr_;
    const char* query_addr_;
    const char* subscribe_addr_;

    // 通道 sockets
    std::unique_ptr<zmq::socket_t> market_pub_;          // 行情发布 (PUB) - 统一通道
    std::unique_ptr<zmq::socket_t> market_pub_okx_;      // OKX 行情发布 (PUB)
    std::unique_ptr<zmq::socket_t> market_pub_binance_;  // Binance 行情发布 (PUB)
    std::unique_ptr<zmq::socket_t> order_pull_;          // 订单接收 (PULL)
    std::unique_ptr<zmq::socket_t> report_pub_;          // 回报发布 (PUB)
    std::unique_ptr<zmq::socket_t> query_rep_;           // 查询响应 (REP)
    std::unique_ptr<zmq::socket_t> subscribe_pull_;      // 订阅管理 (PULL)
    
    // 运行状态
    std::atomic<bool> running_{false};
    
    // 回调函数
    OrderCallback order_callback_;
    QueryCallback query_callback_;
    SubscribeCallback subscribe_callback_;

    // 统计计数器
    std::atomic<uint64_t> market_msg_count_{0};
    std::atomic<uint64_t> order_recv_count_{0};
    std::atomic<uint64_t> report_msg_count_{0};
    std::atomic<uint64_t> query_count_{0};
    std::atomic<uint64_t> subscribe_count_{0};

    // 线程安全保护（ZMQ socket 非线程安全）
    mutable std::mutex market_mutex_;      // 保护行情发布
    mutable std::mutex report_mutex_;      // 保护回报发布
    mutable std::mutex order_mutex_;       // 保护订单接收
    mutable std::mutex query_mutex_;       // 保护查询响应
};

// ============================================================
// 辅助函数
// ============================================================

/**
 * @brief 获取当前毫秒时间戳（用于兼容性）
 */
inline int64_t current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

/**
 * @brief 获取当前纳秒时间戳（用于跨进程延迟测量）
 * 
 * ⚠️ 重要：使用 system_clock（Unix 时间戳），因为：
 * 1. Python 的 time.time_ns() 也是 Unix 时间戳
 * 2. 跨进程测量延迟必须用同一时钟源
 * 
 * 注意：system_clock 可能受 NTP 调整影响，但对于 IPC 延迟测量足够了
 */
inline int64_t current_timestamp_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

/**
 * @brief 构建 Ticker 消息（标准版）
 */
inline nlohmann::json make_ticker_msg(
    const std::string& symbol,
    double last_price,
    double bid_price,
    double ask_price,
    double bid_size = 0.0,
    double ask_size = 0.0,
    double volume_24h = 0.0
) {
    return {
        {"type", "ticker"},
        {"symbol", symbol},
        {"last_price", last_price},
        {"bid_price", bid_price},
        {"ask_price", ask_price},
        {"bid_size", bid_size},
        {"ask_size", ask_size},
        {"volume_24h", volume_24h},
        {"timestamp", current_timestamp_ms()},
        {"timestamp_ns", current_timestamp_ns()}
    };
}

/**
 * @brief 构建大型 Ticker 消息（用于延迟测试）
 * 
 * 生成约 8KB 的行情消息，包含：
 * - 基本行情数据
 * - 序列号（用于检测丢包）
 * - 发送时间戳（纳秒精度）
 * - 深度数据（模拟 20 档买卖盘）
 * - 最近成交记录（模拟 50 笔）
 * - 填充数据（确保总大小约 8KB）
 * 
 * @param symbol 交易对
 * @param seq_num 序列号
 * @param last_price 最新价
 * @return 约 8KB 的 JSON 消息
 */
inline nlohmann::json make_large_ticker_msg(
    const std::string& symbol,
    uint64_t seq_num,
    double last_price
) {
    nlohmann::json msg;
    
    // 基本信息
    msg["type"] = "ticker";
    msg["symbol"] = symbol;
    msg["seq_num"] = seq_num;
    
    // 时间戳（关键！用于延迟计算）
    msg["timestamp_ms"] = current_timestamp_ms();
    msg["timestamp_ns"] = current_timestamp_ns();
    msg["send_time_ns"] = current_timestamp_ns();  // 发送时的精确时间
    
    // 基本行情
    msg["last_price"] = last_price;
    msg["bid_price"] = last_price - 0.5;
    msg["ask_price"] = last_price + 0.5;
    msg["bid_size"] = 10.0;
    msg["ask_size"] = 12.0;
    msg["volume_24h"] = 1000000.0;
    msg["high_24h"] = last_price * 1.05;
    msg["low_24h"] = last_price * 0.95;
    msg["open_24h"] = last_price * 0.98;
    
    // 20 档深度数据（模拟）
    nlohmann::json bids = nlohmann::json::array();
    nlohmann::json asks = nlohmann::json::array();
    for (int i = 0; i < 20; i++) {
        bids.push_back({
            {"price", last_price - (i + 1) * 0.1},
            {"size", 1.0 + i * 0.5},
            {"orders", 5 + i}
        });
        asks.push_back({
            {"price", last_price + (i + 1) * 0.1},
            {"size", 1.5 + i * 0.3},
            {"orders", 3 + i}
        });
    }
    msg["depth"] = {{"bids", bids}, {"asks", asks}};
    
    // 50 笔最近成交（模拟）
    nlohmann::json trades = nlohmann::json::array();
    for (int i = 0; i < 50; i++) {
        trades.push_back({
            {"price", last_price + (i % 10 - 5) * 0.01},
            {"size", 0.1 + (i % 10) * 0.05},
            {"side", (i % 2 == 0) ? "buy" : "sell"},
            {"time", current_timestamp_ms() - i * 100}
        });
    }
    msg["recent_trades"] = trades;
    
    // 填充数据，确保总大小约 8KB
    // 当前约 4KB，需要再加约 4KB 填充
    std::string padding(4000, 'X');  // 4KB 填充
    msg["padding"] = padding;
    msg["msg_size_hint"] = "8KB";
    
    return msg;
}

/**
 * @brief 构建订单回报消息
 *
 * @param strategy_id 策略ID
 * @param client_order_id 客户端订单ID
 * @param exchange_order_id 交易所订单ID
 * @param symbol 交易对
 * @param status 状态: "accepted"/"filled"/"cancelled"/"rejected"
 * @param filled_price 成交价
 * @param filled_qty 成交量
 * @param fee 手续费
 * @param error_msg 错误信息（如果有）
 * @param exchange 交易所类型 ("okx"/"binance")
 * @return JSON 格式的回报消息
 */
inline nlohmann::json make_order_report(
    const std::string& strategy_id,
    const std::string& client_order_id,
    const std::string& exchange_order_id,
    const std::string& symbol,
    const std::string& status,
    double filled_price = 0.0,
    double filled_qty = 0.0,
    double fee = 0.0,
    const std::string& error_msg = "",
    const std::string& exchange = "okx"
) {
    return {
        {"type", "order_report"},
        {"strategy_id", strategy_id},
        {"client_order_id", client_order_id},
        {"exchange_order_id", exchange_order_id},
        {"symbol", symbol},
        {"status", status},
        {"filled_price", filled_price},
        {"filled_quantity", filled_qty},
        {"fee", fee},
        {"error_msg", error_msg},
        {"exchange", exchange},
        {"timestamp", current_timestamp_ms()},
        {"timestamp_ns", current_timestamp_ns()}  // 纳秒时间戳用于延迟测量
    };
}

} // namespace server
} // namespace trading

