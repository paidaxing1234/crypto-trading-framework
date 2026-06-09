#pragma once

#include "event.h"
#include <string>
#include <memory>
#include <atomic>
#include <cstdio>

namespace trading {

/**
 * @brief 订单类型
 */
enum class OrderType {
    LIMIT,      // 限价单
    MARKET,     // 市价单
    POST_ONLY,  // 只做Maker（Post-only）
    FOK,        // 全部成交或立即取消（Fill-or-Kill）
    IOC         // 立即成交并取消剩余（Immediate-or-Cancel）
};

/**
 * @brief 订单方向
 */
enum class OrderSide {
    BUY,   // 买入
    SELL   // 卖出
};

/**
 * @brief 订单生命周期状态
 * 
 * 状态转换：
 * CREATED → SUBMITTED → ACCEPTED → PARTIALLY_FILLED → FILLED
 *                                → CANCELLED
 */
enum class OrderState {
    CREATED,          // 本地创建
    SUBMITTED,        // 已提交到交易所
    ACCEPTED,         // 交易所已接受
    PARTIALLY_FILLED, // 部分成交
    FILLED,           // 完全成交
    CANCELLED,        // 已取消
    REJECTED,         // 被拒绝
    FAILED            // 失败
};

/**
 * @brief 订单事件
 * 
 * 既是数据模型，也是事件
 * 订单的任何状态变化都会作为事件在引擎中流转
 */
class Order : public Event {
public:
    using Ptr = std::shared_ptr<Order>;
    
    // 订单ID生成器（全局唯一）
    static int64_t next_order_id() {
        static std::atomic<int64_t> id_gen{1};
        return id_gen.fetch_add(1);
    }
    
    Order(
        const std::string& symbol,
        OrderType order_type,
        OrderSide side,
        double quantity,
        double price = 0.0,
        const std::string& exchange = "okx"
    ) 
        : order_id_(next_order_id())
        , client_order_id_("order_" + std::to_string(order_id_))
        , exchange_(exchange)
        , symbol_(symbol)
        , order_type_(order_type)
        , side_(side)
        , price_(price)
        , quantity_(quantity)
        , filled_quantity_(0.0)
        , filled_price_(0.0)
        , state_(OrderState::CREATED)
        , fee_(0.0)
        , create_time_(0)
        , update_time_(0) {
    }
    
    virtual ~Order() noexcept override = default;
    
    virtual std::string type_name() const override {
        return "Order";
    }
    
    // Getters
    int64_t order_id() const { return order_id_; }
    const std::string& client_order_id() const { return client_order_id_; }
    const std::string& exchange_order_id() const { return exchange_order_id_; }
    const std::string& exchange() const { return exchange_; }
    const std::string& symbol() const { return symbol_; }
    OrderType order_type() const { return order_type_; }
    OrderSide side() const { return side_; }
    double price() const { return price_; }
    double quantity() const { return quantity_; }
    double filled_quantity() const { return filled_quantity_; }
    double filled_price() const { return filled_price_; }
    OrderState state() const { return state_; }
    double fee() const { return fee_; }
    const std::string& fee_currency() const { return fee_currency_; }
    int64_t create_time() const { return create_time_; }
    int64_t update_time() const { return update_time_; }
    const std::string& error_msg() const { return error_msg_; }
    
    // Setters
    void set_client_order_id(const std::string& id) { client_order_id_ = id; }
    void set_exchange_order_id(const std::string& id) { exchange_order_id_ = id; }
    void set_price(double price) { price_ = price; }
    void set_filled_quantity(double qty) { filled_quantity_ = qty; }
    void set_filled_price(double price) { filled_price_ = price; }
    void set_state(OrderState state) { state_ = state; }
    void set_fee(double fee) { fee_ = fee; }
    void set_fee_currency(const std::string& currency) { fee_currency_ = currency; }
    void set_create_time(int64_t time) { create_time_ = time; }
    void set_update_time(int64_t time) { update_time_ = time; }
    void set_error_msg(const std::string& msg) { error_msg_ = msg; }
    
    // 便捷属性
    bool is_buy() const { return side_ == OrderSide::BUY; }
    bool is_sell() const { return side_ == OrderSide::SELL; }
    bool is_filled() const { return state_ == OrderState::FILLED; }
    
    bool is_active() const {
        return state_ == OrderState::SUBMITTED ||
               state_ == OrderState::ACCEPTED ||
               state_ == OrderState::PARTIALLY_FILLED;
    }
    
    bool is_final() const {
        return state_ == OrderState::FILLED ||
               state_ == OrderState::CANCELLED ||
               state_ == OrderState::REJECTED ||
               state_ == OrderState::FAILED;
    }
    
    double remaining_quantity() const {
        return quantity_ - filled_quantity_;
    }
    
    // 工厂方法
    static Ptr create_limit_order(
        const std::string& symbol,
        OrderSide side,
        double quantity,
        double price,
        const std::string& exchange = "okx"
    ) {
        return std::make_shared<Order>(symbol, OrderType::LIMIT, side, quantity, price, exchange);
    }
    
    static Ptr create_market_order(
        const std::string& symbol,
        OrderSide side,
        double quantity,
        const std::string& exchange = "okx"
    ) {
        return std::make_shared<Order>(symbol, OrderType::MARKET, side, quantity, 0.0, exchange);
    }
    
    static Ptr buy_limit(const std::string& symbol, double quantity, double price) {
        return create_limit_order(symbol, OrderSide::BUY, quantity, price);
    }
    
    static Ptr sell_limit(const std::string& symbol, double quantity, double price) {
        return create_limit_order(symbol, OrderSide::SELL, quantity, price);
    }
    
    static Ptr buy_market(const std::string& symbol, double quantity) {
        return create_market_order(symbol, OrderSide::BUY, quantity);
    }
    
    static Ptr sell_market(const std::string& symbol, double quantity) {
        return create_market_order(symbol, OrderSide::SELL, quantity);
    }
    
    // 字符串表示
    std::string to_string() const;

private:
    // 基本信息
    int64_t order_id_;                  // 本地订单ID
    std::string client_order_id_;       // 客户端订单ID
    std::string exchange_order_id_;     // 交易所订单ID
    std::string exchange_;              // 交易所名称
    
    // 订单参数
    std::string symbol_;                // 交易对
    OrderType order_type_;              // 订单类型
    OrderSide side_;                    // 买卖方向
    double price_;                      // 价格
    double quantity_;                   // 数量
    
    // 成交信息
    double filled_quantity_;            // 已成交数量
    double filled_price_;               // 成交均价
    
    // 状态
    OrderState state_;                  // 订单状态
    
    // 费用
    double fee_;                        // 手续费
    std::string fee_currency_;          // 手续费币种
    
    // 时间
    int64_t create_time_;               // 创建时间
    int64_t update_time_;               // 更新时间
    
    // 错误信息
    std::string error_msg_;             // 错误信息
};

// 辅助函数：枚举转字符串
inline std::string order_type_to_string(OrderType type) {
    switch (type) {
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::MARKET: return "MARKET";
        case OrderType::POST_ONLY: return "POST_ONLY";
        case OrderType::FOK: return "FOK";
        case OrderType::IOC: return "IOC";
        default: return "UNKNOWN";
    }
}

inline std::string order_side_to_string(OrderSide side) {
    switch (side) {
        case OrderSide::BUY: return "BUY";
        case OrderSide::SELL: return "SELL";
        default: return "UNKNOWN";
    }
}

inline std::string order_state_to_string(OrderState state) {
    switch (state) {
        case OrderState::CREATED: return "CREATED";
        case OrderState::SUBMITTED: return "SUBMITTED";
        case OrderState::ACCEPTED: return "ACCEPTED";
        case OrderState::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderState::FILLED: return "FILLED";
        case OrderState::CANCELLED: return "CANCELLED";
        case OrderState::REJECTED: return "REJECTED";
        case OrderState::FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

inline std::string Order::to_string() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "Order(id=%lld, exchange=%s, symbol=%s, side=%s, type=%s, "
        "price=%.2f, qty=%.4f, filled=%.4f, state=%s)",
        (long long)order_id_,
        exchange_.c_str(),
        symbol_.c_str(),
        order_side_to_string(side_).c_str(),
        order_type_to_string(order_type_).c_str(),
        price_,
        quantity_,
        filled_quantity_,
        order_state_to_string(state_).c_str()
    );
    return std::string(buf);
}

} // namespace trading

