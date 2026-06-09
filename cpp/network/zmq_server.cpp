/**
 * @file zmq_server.cpp
 * @brief ZeroMQ 服务端实现
 * 
 * 实现说明：
 * 
 * 1. IPC 通道原理：
 *    - 使用 Unix Domain Socket，数据不经过 TCP/IP 协议栈
 *    - 内核直接在两个进程间拷贝数据
 *    - 延迟约 30-100μs（比 TCP localhost 快 3-5 倍）
 * 
 * 2. ZeroMQ 消息模式：
 *    - PUB-SUB：发布-订阅，一对多广播
 *    - PUSH-PULL：推送-拉取，多对一汇聚
 * 
 * 3. 非阻塞接收：
 *    - 使用 ZMQ_DONTWAIT 标志
 *    - 没有消息时立即返回，不阻塞主线程
 * 
 * @author Sequence Team
 * @date 2024-12
 */

#include "zmq_server.h"
#include <iostream>
#include <cstdio>  // for remove()

namespace trading {
namespace server {

// ============================================================
// 构造函数和析构函数
// ============================================================

ZmqServer::ZmqServer(int mode)
    : context_(1)  // 1 个 I/O 线程，对于 IPC 足够了
{
    // 统一使用实盘地址（mode 参数保留用于将来扩展）
    (void)mode;  // 避免未使用参数警告

    market_data_addr_ = IpcAddresses::MARKET_DATA;
    market_data_okx_addr_ = IpcAddresses::MARKET_DATA_OKX;
    market_data_binance_addr_ = IpcAddresses::MARKET_DATA_BINANCE;
    order_addr_ = IpcAddresses::ORDER;
    report_addr_ = IpcAddresses::REPORT;
    query_addr_ = IpcAddresses::QUERY;
    subscribe_addr_ = IpcAddresses::SUBSCRIBE;

    std::cout << "[ZmqServer] 初始化完成\n";
}

ZmqServer::~ZmqServer() {
    // 确保停止
    stop();
    std::cout << "[ZmqServer] 销毁完成\n";
}

// ============================================================
// 生命周期管理
// ============================================================

bool ZmqServer::start() {
    if (running_.load()) {
        std::cout << "[ZmqServer] 已经在运行中\n";
        return true;
    }
    
    try {
        // ========================================
        // 创建行情发布 socket (PUB)
        // ========================================
        // PUB socket 用于一对多广播
        // 所有连接到这个地址的 SUB socket 都会收到消息
        market_pub_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::pub);
        
        // 设置 socket 选项
        // LINGER = 0: 关闭时不等待未发送的消息
        int linger = 0;
        market_pub_->set(zmq::sockopt::linger, linger);
        
        // 绑定到 IPC 地址
        // 如果文件已存在，先删除
        std::string md_path = std::string(market_data_addr_).substr(6);  // 去掉 "ipc://"
        std::remove(md_path.c_str());
        market_pub_->bind(market_data_addr_);
        std::cout << "[ZmqServer] 行情通道已绑定: " << market_data_addr_ << "\n";

        // ========================================
        // 创建 OKX 专用行情发布 socket (PUB)
        // ========================================
        market_pub_okx_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::pub);
        market_pub_okx_->set(zmq::sockopt::linger, linger);

        std::string md_okx_path = std::string(market_data_okx_addr_).substr(6);
        std::remove(md_okx_path.c_str());
        market_pub_okx_->bind(market_data_okx_addr_);
        std::cout << "[ZmqServer] OKX行情通道已绑定: " << market_data_okx_addr_ << "\n";

        // ========================================
        // 创建 Binance 专用行情发布 socket (PUB)
        // ========================================
        market_pub_binance_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::pub);
        market_pub_binance_->set(zmq::sockopt::linger, linger);

        std::string md_binance_path = std::string(market_data_binance_addr_).substr(6);
        std::remove(md_binance_path.c_str());
        market_pub_binance_->bind(market_data_binance_addr_);
        std::cout << "[ZmqServer] Binance行情通道已绑定: " << market_data_binance_addr_ << "\n";
        
        // ========================================
        // 创建订单接收 socket (PULL)
        // ========================================
        // PULL socket 用于接收多个客户端的消息
        // 消息会自动负载均衡（每条消息只有一个 PULL 收到）
        order_pull_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::pull);
        order_pull_->set(zmq::sockopt::linger, linger);

        // 绑定到 IPC 地址
        std::string order_path = std::string(order_addr_).substr(6);
        std::remove(order_path.c_str());
        order_pull_->bind(order_addr_);
        std::cout << "[ZmqServer] 订单通道已绑定: " << order_addr_ << "\n";
        
        // ========================================
        // 创建回报发布 socket (PUB)
        // ========================================
        report_pub_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::pub);
        report_pub_->set(zmq::sockopt::linger, linger);

        std::string report_path = std::string(report_addr_).substr(6);
        std::remove(report_path.c_str());
        report_pub_->bind(report_addr_);
        std::cout << "[ZmqServer] 回报通道已绑定: " << report_addr_ << "\n";
        
        // ========================================
        // 创建查询响应 socket (REP)
        // ========================================
        query_rep_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::rep);
        query_rep_->set(zmq::sockopt::linger, linger);
        query_rep_->set(zmq::sockopt::rcvtimeo, 0);  // 非阻塞

        std::string query_path = std::string(query_addr_).substr(6);
        std::remove(query_path.c_str());
        query_rep_->bind(query_addr_);
        std::cout << "[ZmqServer] 查询通道已绑定: " << query_addr_ << "\n";
        
        // ========================================
        // 创建订阅管理 socket (PULL)
        // ========================================
        subscribe_pull_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::pull);
        subscribe_pull_->set(zmq::sockopt::linger, linger);

        std::string subscribe_path = std::string(subscribe_addr_).substr(6);
        std::remove(subscribe_path.c_str());
        subscribe_pull_->bind(subscribe_addr_);
        std::cout << "[ZmqServer] 订阅通道已绑定: " << subscribe_addr_ << "\n";
        
        running_.store(true);
        std::cout << "[ZmqServer] 服务已启动\n";
        return true;
        
    } catch (const zmq::error_t& e) {
        std::cerr << "[ZmqServer] 启动失败: " << e.what() << "\n";
        stop();
        return false;
    }
}

void ZmqServer::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    // 关闭所有 socket
    // 注意：必须先关闭 socket，再销毁 context
    if (market_pub_) {
        market_pub_->close();
        market_pub_.reset();
    }

    if (market_pub_okx_) {
        market_pub_okx_->close();
        market_pub_okx_.reset();
    }

    if (market_pub_binance_) {
        market_pub_binance_->close();
        market_pub_binance_.reset();
    }
    
    if (order_pull_) {
        order_pull_->close();
        order_pull_.reset();
    }
    
    if (report_pub_) {
        report_pub_->close();
        report_pub_.reset();
    }
    
    if (query_rep_) {
        query_rep_->close();
        query_rep_.reset();
    }
    
    if (subscribe_pull_) {
        subscribe_pull_->close();
        subscribe_pull_.reset();
    }
    
    // 清理 IPC 文件
    std::string md_path = std::string(market_data_addr_).substr(6);
    std::string md_okx_path = std::string(market_data_okx_addr_).substr(6);
    std::string md_binance_path = std::string(market_data_binance_addr_).substr(6);
    std::string order_path = std::string(order_addr_).substr(6);
    std::string report_path = std::string(report_addr_).substr(6);
    std::string query_path = std::string(query_addr_).substr(6);
    std::string subscribe_path = std::string(subscribe_addr_).substr(6);

    std::remove(md_path.c_str());
    std::remove(md_okx_path.c_str());
    std::remove(md_binance_path.c_str());
    std::remove(order_path.c_str());
    std::remove(report_path.c_str());
    std::remove(query_path.c_str());
    std::remove(subscribe_path.c_str());
    
    std::cout << "[ZmqServer] 服务已停止\n";
    std::cout << "[ZmqServer] 统计 - 行情: " << market_msg_count_ 
              << ", 订单: " << order_recv_count_ 
              << ", 回报: " << report_msg_count_
              << ", 查询: " << query_count_
              << ", 订阅: " << subscribe_count_ << "\n";
}

// ============================================================
// 行情发布
// ============================================================

bool ZmqServer::publish_ticker(const nlohmann::json& ticker_data) {
    return publish_market(ticker_data, MessageType::TICKER);
}

bool ZmqServer::publish_depth(const nlohmann::json& depth_data) {
    return publish_market(depth_data, MessageType::DEPTH);
}

bool ZmqServer::publish_market(const nlohmann::json& data, MessageType msg_type) {
    if (!running_.load() || !market_pub_) {
        return false;
    }

    // 线程安全保护
    std::lock_guard<std::mutex> lock(market_mutex_);

    // 构建主题: {exchange}.{type}.{symbol}[.{interval}]
    std::string exchange = data.value("exchange", "unknown");
    std::string symbol = data.value("symbol", "");
    std::string type_str;

    switch (msg_type) {
        case MessageType::TICKER: type_str = "ticker"; break;
        case MessageType::DEPTH:  type_str = "depth"; break;
        case MessageType::TRADE:  type_str = "trade"; break;
        case MessageType::KLINE:  type_str = "kline"; break;
        default: type_str = "unknown"; break;
    }

    // 从 JSON 中获取 type 字段（更准确）
    std::string json_type = data.value("type", "");
    if (!json_type.empty()) {
        type_str = json_type;
    }

    std::string topic = exchange + "." + type_str + "." + symbol;

    // K线数据添加周期
    if (msg_type == MessageType::KLINE || json_type == "kline") {
        std::string interval = data.value("interval", "");
        if (!interval.empty()) {
            topic += "." + interval;
        }
    }

    // 消息格式: topic|json_data
    std::string msg = topic + "|" + data.dump();

    if (send_message(*market_pub_, msg)) {
        market_msg_count_++;
        return true;
    }
    return false;
}

// ============================================================
// 订单接收
// ============================================================

bool ZmqServer::recv_order(std::string& order_msg) {
    if (!running_.load() || !order_pull_) {
        return false;
    }

    // 线程安全保护
    std::lock_guard<std::mutex> lock(order_mutex_);
    return recv_message(*order_pull_, order_msg);
}

bool ZmqServer::recv_order_json(nlohmann::json& order) {
    std::string msg;
    if (!recv_order(msg)) {
        return false;
    }
    
    try {
        order = nlohmann::json::parse(msg);
        order_recv_count_++;
        return true;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[ZmqServer] JSON 解析失败: " << e.what() << "\n";
        return false;
    }
}

int ZmqServer::poll_orders() {
    int count = 0;
    nlohmann::json order;
    
    // 循环接收所有待处理的订单
    while (recv_order_json(order)) {
        if (order_callback_) {
            order_callback_(order);
        }
        count++;
    }
    
    return count;
}

// ============================================================
// 回报发布
// ============================================================

bool ZmqServer::publish_report(const nlohmann::json& report_data) {
    if (!running_.load() || !report_pub_) {
        std::cout << "[ZmqServer] DEBUG: publish_report 失败 - running=" << running_.load()
                  << ", report_pub_=" << (report_pub_ ? "valid" : "null") << "\n";
        return false;
    }

    // 线程安全保护
    std::lock_guard<std::mutex> lock(report_mutex_);

    // 构建主题: report.{strategy_id}
    // 策略端可以订阅 "report.my_strategy_id" 只收自己的回报
    std::string strategy_id = report_data.value("strategy_id", "");
    std::string topic = "report." + strategy_id;

    // 消息格式: topic|json_data
    std::string msg = topic + "|" + report_data.dump();

    if (send_message(*report_pub_, msg)) {
        report_msg_count_++;
        return true;
    }
    return false;
}

// ============================================================
// 查询处理
// ============================================================

bool ZmqServer::handle_query() {
    if (!running_.load() || !query_rep_ || !query_callback_) {
        return false;
    }
    
    try {
        zmq::message_t request;
        auto result = query_rep_->recv(request, zmq::recv_flags::dontwait);
        
        if (!result.has_value()) {
            return false;
        }
        
        // 解析请求
        std::string req_str(static_cast<char*>(request.data()), request.size());
        nlohmann::json req_json = nlohmann::json::parse(req_str);
        
        // 调用回调获取响应
        nlohmann::json response = query_callback_(req_json);
        
        // 发送响应
        std::string resp_str = response.dump();
        zmq::message_t reply(resp_str.data(), resp_str.size());
        query_rep_->send(reply, zmq::send_flags::none);
        
        query_count_++;
        return true;
        
    } catch (const zmq::error_t& e) {
        if (e.num() != EAGAIN) {
            std::cerr << "[ZmqServer] 查询处理失败: " << e.what() << "\n";
        }
        return false;
    } catch (const nlohmann::json::exception& e) {
        // JSON 解析错误，发送错误响应
        try {
            nlohmann::json error_resp = {{"error", e.what()}, {"code", -1}};
            std::string resp_str = error_resp.dump();
            zmq::message_t reply(resp_str.data(), resp_str.size());
            query_rep_->send(reply, zmq::send_flags::none);
        } catch (...) {}
        return false;
    }
}

int ZmqServer::poll_queries() {
    int count = 0;
    while (handle_query()) {
        count++;
    }
    return count;
}

// ============================================================
// 订阅管理
// ============================================================

int ZmqServer::poll_subscriptions() {
    if (!running_.load() || !subscribe_pull_ || !subscribe_callback_) {
        return 0;
    }
    
    int count = 0;
    std::string msg;
    
    while (recv_message(*subscribe_pull_, msg)) {
        try {
            nlohmann::json req = nlohmann::json::parse(msg);
            subscribe_callback_(req);
            subscribe_count_++;
            count++;
        } catch (const nlohmann::json::exception& e) {
            std::cerr << "[ZmqServer] 订阅请求解析失败: " << e.what() << "\n";
        }
    }
    
    return count;
}

// ============================================================
// K线发布
// ============================================================

bool ZmqServer::publish_kline(const nlohmann::json& kline_data) {
    return publish_market(kline_data, MessageType::KLINE);
}

bool ZmqServer::publish_with_topic(const std::string& topic, const nlohmann::json& data) {
    if (!running_.load() || !market_pub_) {
        return false;
    }

    // 消息格式: topic|json_data
    std::string msg = topic + "|" + data.dump();

    if (send_message(*market_pub_, msg)) {
        market_msg_count_++;
        return true;
    }
    return false;
}

// ============================================================
// 交易所专用行情发布
// ============================================================

bool ZmqServer::publish_okx_market(const nlohmann::json& data, MessageType msg_type) {
    if (!running_.load() || !market_pub_okx_) {
        return false;
    }

    // 线程安全保护
    std::lock_guard<std::mutex> lock(market_mutex_);

    // 构建主题: okx.{type}.{symbol}[.{interval}]
    std::string symbol = data.value("symbol", "");
    std::string type_str;

    switch (msg_type) {
        case MessageType::TICKER: type_str = "ticker"; break;
        case MessageType::DEPTH:  type_str = "depth"; break;
        case MessageType::TRADE:  type_str = "trade"; break;
        case MessageType::KLINE:  type_str = "kline"; break;
        default: type_str = "unknown"; break;
    }

    // 从 JSON 中获取 type 字段（更准确）
    std::string json_type = data.value("type", "");
    if (!json_type.empty()) {
        type_str = json_type;
    }

    std::string topic = "okx." + type_str + "." + symbol;

    // K线数据添加周期
    if (msg_type == MessageType::KLINE || json_type == "kline") {
        std::string interval = data.value("interval", "");
        if (!interval.empty()) {
            topic += "." + interval;
        }
    }

    // 消息格式: topic|json_data
    std::string msg = topic + "|" + data.dump();

    if (send_message(*market_pub_okx_, msg)) {
        market_msg_count_++;
        return true;
    }
    return false;
}

bool ZmqServer::publish_binance_market(const nlohmann::json& data, MessageType msg_type) {
    if (!running_.load() || !market_pub_binance_) {
        return false;
    }

    // 线程安全保护
    std::lock_guard<std::mutex> lock(market_mutex_);

    // 构建主题: binance.{type}.{symbol}[.{interval}]
    std::string symbol = data.value("symbol", "");
    std::string type_str;

    switch (msg_type) {
        case MessageType::TICKER: type_str = "ticker"; break;
        case MessageType::DEPTH:  type_str = "depth"; break;
        case MessageType::TRADE:  type_str = "trade"; break;
        case MessageType::KLINE:  type_str = "kline"; break;
        default: type_str = "unknown"; break;
    }

    // 从 JSON 中获取 type 字段（更准确）
    std::string json_type = data.value("type", "");
    if (!json_type.empty()) {
        type_str = json_type;
    }

    std::string topic = "binance." + type_str + "." + symbol;

    // K线数据添加周期
    if (msg_type == MessageType::KLINE || json_type == "kline") {
        std::string interval = data.value("interval", "");
        if (!interval.empty()) {
            topic += "." + interval;
        }
    }

    // 消息格式: topic|json_data
    std::string msg = topic + "|" + data.dump();

    if (send_message(*market_pub_binance_, msg)) {
        market_msg_count_++;
        return true;
    }
    return false;
}

// ============================================================
// 私有辅助函数
// ============================================================

bool ZmqServer::send_message(zmq::socket_t& socket, const std::string& data) {
    try {
        // 创建 ZeroMQ 消息
        zmq::message_t message(data.data(), data.size());
        
        // 发送消息
        // send() 对于 PUB socket 是非阻塞的
        auto result = socket.send(message, zmq::send_flags::none);
        
        return result.has_value();
        
    } catch (const zmq::error_t& e) {
        std::cerr << "[ZmqServer] 发送失败: " << e.what() << "\n";
        return false;
    }
}

bool ZmqServer::recv_message(zmq::socket_t& socket, std::string& data) {
    try {
        zmq::message_t message;
        
        // 非阻塞接收
        // ZMQ_DONTWAIT: 没有消息时立即返回，不等待
        auto result = socket.recv(message, zmq::recv_flags::dontwait);
        
        if (!result.has_value()) {
            // 没有消息
            return false;
        }
        
        // 提取数据
        data = std::string(static_cast<char*>(message.data()), message.size());
        return true;
        
    } catch (const zmq::error_t& e) {
        // EAGAIN 表示没有消息，不是错误
        if (e.num() != EAGAIN) {
            std::cerr << "[ZmqServer] 接收失败: " << e.what() << "\n";
        }
        return false;
    }
}

} // namespace server
} // namespace trading

