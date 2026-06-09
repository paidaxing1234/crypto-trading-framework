#pragma once

/**
 * @file binance_rest_api.h
 * @brief Binance REST API 封装
 * 
 * 支持币安三种合约类型：
 * - SPOT: 现货交易
 * - FUTURES: U本位合约 (USDT保证金)
 * - COIN_FUTURES: 币本位合约 (币种保证金)
 * 
 * API文档：
 * - 现货: https://binance-docs.github.io/apidocs/spot/cn/
 * - U本位合约: https://binance-docs.github.io/apidocs/futures/cn/
 * - 币本位合约: https://binance-docs.github.io/apidocs/delivery/cn/
 * @author Sequence Team
 * @date 2025-12
 */

#include <string>
#include <memory>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>
#include "../../network/proxy_config.h"

namespace trading {
namespace binance {

// ==================== 枚举类型 ====================

/**
 * @brief 市场类型
 */
enum class MarketType {
    SPOT,           // 现货
    FUTURES,        // U本位合约
    COIN_FUTURES    // 币本位合约
};

/**
 * @brief 订单方向
 */
enum class OrderSide {
    BUY,
    SELL
};

/**
 * @brief 订单类型
 */
enum class OrderType {
    LIMIT,              // 限价单
    MARKET,             // 市价单
    STOP_LOSS,          // 止损单
    STOP_LOSS_LIMIT,    // 限价止损单
    TAKE_PROFIT,        // 止盈单
    TAKE_PROFIT_LIMIT,  // 限价止盈单
    LIMIT_MAKER         // 限价只做Maker单
};

/**
 * @brief 持仓方向（合约）
 */
enum class PositionSide {
    BOTH,   // 单向持仓
    LONG,   // 多头
    SHORT   // 空头
};

/**
 * @brief 时间有效性
 */
enum class TimeInForce {
    GTC,    // Good Till Cancel 成交为止
    IOC,    // Immediate or Cancel 无法立即成交的部分就撤销
    FOK,    // Fill or Kill 无法全部立即成交就撤销
    GTX     // Good Till Crossing 无法成为挂单方就撤销
};

// ==================== 数据结构 ====================

// ==================== Binance REST API ====================

/**
 * @brief Binance REST API 客户端
 * 
 * 使用示例：
 * @code
 * // 现货交易
 * BinanceRestAPI spot_api(api_key, secret_key, MarketType::SPOT);
 * auto result = spot_api.place_order("BTCUSDT", OrderSide::BUY, 
 *                                     OrderType::LIMIT, "0.001", "50000");
 * 
 * // U本位合约
 * BinanceRestAPI futures_api(api_key, secret_key, MarketType::FUTURES);
 * auto balance = futures_api.get_account_balance();
 * @endcode
 */
class BinanceRestAPI {
public:
    /**
     * @brief 构造函数
     *
     * @param api_key API密钥
     * @param secret_key Secret密钥
     * @param market_type 市场类型（现货/U本位合约/币本位合约）
     * @param is_testnet 是否使用测试网
     * @param proxy_config 代理配置（可选，默认从环境变量加载）
     */
    BinanceRestAPI(
        const std::string& api_key,
        const std::string& secret_key,
        MarketType market_type = MarketType::SPOT,
        bool is_testnet = false,
        const core::ProxyConfig& proxy_config = core::ProxyConfig::get_default()
    );
    
    ~BinanceRestAPI() = default;
    
    // ==================== 市场数据接口（无需签名） ====================
    
    /**
     * @brief 测试连接
     * 
     * 测试REST API的连通性
     * GET /api/v3/ping
     */
    bool test_connectivity();
    
    /**
     * @brief 获取服务器时间
     * 
     * 获取服务器时间（毫秒时间戳）
     * GET /api/v3/time
     */
    int64_t get_server_time();

    /**
     * @brief 同步本地时间与Binance服务器时间
     */
    void sync_server_time();

    /**
     * @brief 获取交易规则和交易对信息
     *
     * GET /api/v3/exchangeInfo (现货)
     * GET /fapi/v1/exchangeInfo (U本位合约)
     */
    nlohmann::json get_exchange_info(const std::string& symbol = "");

    /**
     * @brief 获取深度信息
     * 
     * @param symbol 交易对
     * @param limit 档位数量，默认100，最大5000
     */
    nlohmann::json get_depth(const std::string& symbol, int limit = 100);
    
    /**
     * @brief 获取最新成交
     * 
     * @param symbol 交易对
     * @param limit 数量，默认500，最大1000
     */
    nlohmann::json get_recent_trades(const std::string& symbol, int limit = 500);
    
    /**
     * @brief 获取K线数据
     * 
     * @param symbol 交易对
     * @param interval K线间隔（1m, 3m, 5m, 15m, 30m, 1h, 2h, 4h, 6h, 8h, 12h, 1d, 3d, 1w, 1M）
     * @param start_time 起始时间（毫秒时间戳）
     * @param end_time 结束时间（毫秒时间戳）
     * @param limit 数量，默认500，最大1500
     */
    nlohmann::json get_klines(
        const std::string& symbol,
        const std::string& interval,
        int64_t start_time = 0,
        int64_t end_time = 0,
        int limit = 500
    );
    
    /**
     * @brief 获取24小时价格变动情况
     * 
     * @param symbol 交易对（为空则返回所有交易对）
     */
    nlohmann::json get_ticker_24hr(const std::string& symbol = "");
    
    /**
     * @brief 获取最新价格
     * 
     * @param symbol 交易对（为空则返回所有交易对）
     */
    nlohmann::json get_ticker_price(const std::string& symbol = "");
    
    /**
     * @brief 获取资金费率（仅合约）
     *
     * @param symbol 交易对
     * @param limit 数量，默认100，最大1000
     */
    nlohmann::json get_funding_rate(const std::string& symbol, int limit = 100);

    /**
     * @brief 获取溢价指数K线数据（仅U本位合约）
     *
     * GET /fapi/v1/premiumIndexKlines
     *
     * @param symbol 交易对
     * @param interval K线间隔（1m, 3m, 5m, 15m, 30m, 1h, 2h, 4h, 6h, 8h, 12h, 1d, 3d, 1w, 1M）
     * @param start_time 起始时间（毫秒时间戳）
     * @param end_time 结束时间（毫秒时间戳）
     * @param limit 数量，默认500，最大1500
     */
    nlohmann::json get_premium_index_klines(
        const std::string& symbol,
        const std::string& interval,
        int64_t start_time = 0,
        int64_t end_time = 0,
        int limit = 500
    );

    // ==================== 用户数据流（USER_STREAM） ====================
    
    /**
     * @brief 创建或获取 listenKey
     *
     * - SPOT:        POST /api/v3/userDataStream
     * - FUTURES:     POST /fapi/v1/listenKey
     * - COIN_FUTURES:POST /dapi/v1/listenKey
     */
    nlohmann::json create_listen_key();
    
    /**
     * @brief 延长 listenKey 有效期（60分钟）
     *
     * - SPOT:        PUT /api/v3/userDataStream
     * - FUTURES:     PUT /fapi/v1/listenKey
     * - COIN_FUTURES:PUT /dapi/v1/listenKey
     */
    nlohmann::json keepalive_listen_key(const std::string& listen_key);
    
    // ==================== 交易接口（需要签名） ====================
    
    /**
     * @brief 下单（简化版）
     * 
     * POST /api/v3/order (现货)
     * POST /fapi/v1/order (U本位合约)
     * 
     * @param symbol 交易对
     * @param side 买卖方向
     * @param type 订单类型
     * @param quantity 数量
     * @param price 价格（限价单必填）
     * @param time_in_force 时间有效性
     * @param position_side 持仓方向（仅合约）
     * @param client_order_id 客户自定义订单ID
     */
    nlohmann::json place_order(
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
     * @brief 撤销订单
     * 
     * @param symbol 交易对
     * @param order_id 订单ID
     * @param client_order_id 客户自定义订单ID
     */
    nlohmann::json cancel_order(
        const std::string& symbol,
        int64_t order_id = 0,
        const std::string& client_order_id = ""
    );
    
    /**
     * @brief 查询订单
     *
     * @param symbol 交易对
     * @param order_id 订单ID
     * @param client_order_id 客户自定义订单ID
     */
    nlohmann::json get_order(
        const std::string& symbol,
        int64_t order_id = 0,
        const std::string& client_order_id = ""
    );

    /**
     * @brief 获取当前挂单
     *
     * @param symbol 交易对（为空则返回所有挂单）
     */
    nlohmann::json get_open_orders(const std::string& symbol = "");

    /**
     * @brief 获取所有订单（包括历史）
     *
     * @param symbol 交易对
     * @param start_time 起始时间（毫秒）
     * @param end_time 结束时间（毫秒）
     * @param limit 数量限制
     */
    nlohmann::json get_all_orders(
        const std::string& symbol,
        int64_t start_time = 0,
        int64_t end_time = 0,
        int limit = 500
    );

    /**
     * @brief 撤销所有挂单
     *
     * @param symbol 交易对
     */
    nlohmann::json cancel_all_orders(const std::string& symbol);

    /**
     * @brief 批量下单（仅合约）
     *
     * @param orders 订单列表（JSON数组）
     */
    nlohmann::json place_batch_orders(const nlohmann::json& orders);

    // ==================== 账户接口（需要签名） ====================

    /**
     * @brief 获取账户余额
     *
     * 现货: GET /api/v3/account
     * U本位合约: GET /fapi/v2/balance
     * 币本位合约: GET /dapi/v1/balance
     */
    nlohmann::json get_account_balance();

    /**
     * @brief 获取账户信息（包含余额和持仓）
     *
     * 现货: GET /api/v3/account
     * U本位合约: GET /fapi/v2/account
     */
    nlohmann::json get_account_info();

    /**
     * @brief 获取持仓信息（仅合约）
     *
     * @param symbol 交易对（为空则返回所有持仓）
     */
    nlohmann::json get_positions(const std::string& symbol = "");

    /**
     * @brief 修改杠杆倍数（仅合约）
     *
     * @param symbol 交易对
     * @param leverage 杠杆倍数（1-125）
     */
    nlohmann::json change_leverage(const std::string& symbol, int leverage);

    /**
     * @brief 批量下单（仅合约）
     *
     * @param orders 订单列表，最多5个订单
     *        每个订单包含: symbol, side, type, quantity, price(可选), positionSide(可选)
     * @return 批量下单结果
     *
     * 示例:
     * @code
     * std::vector<nlohmann::json> orders = {
     *     {{"symbol", "BTCUSDT"}, {"side", "BUY"}, {"type", "MARKET"},
     *      {"quantity", "0.001"}, {"positionSide", "LONG"}},
     *     {{"symbol", "ETHUSDT"}, {"side", "SELL"}, {"type", "MARKET"},
     *      {"quantity", "0.01"}, {"positionSide", "SHORT"}}
     * };
     * auto result = api.place_batch_orders(orders);
     * @endcode
     */
    nlohmann::json place_batch_orders(const std::vector<nlohmann::json>& orders);

    /**
     * @brief 修改保证金模式（仅合约）
     *
     * @param symbol 交易对
     * @param margin_type 保证金模式: "ISOLATED"(逐仓) 或 "CROSSED"(全仓)
     */
    nlohmann::json change_margin_type(const std::string& symbol, const std::string& margin_type);

    /**
     * @brief 修改持仓模式（仅合约）
     *
     * @param dual_side_position true=双向持仓, false=单向持仓
     */
    nlohmann::json change_position_mode(bool dual_side_position);

    /**
     * @brief 获取持仓模式（仅合约）
     */
    nlohmann::json get_position_mode();

    // ==================== 工具方法 ====================
    
    /**
     * @brief 设置HTTP代理
     */
    void set_proxy(const std::string& proxy_host, uint16_t proxy_port);
    
    /**
     * @brief 获取市场类型
     */
    MarketType get_market_type() const { return market_type_; }
    
    /**
     * @brief 获取基础URL
     */
    const std::string& get_base_url() const { return base_url_; }

private:
    // 签名相关
    std::string create_signature(const std::string& query_string);
    std::string create_query_string(const nlohmann::json& params);
    
    // HTTP请求
    nlohmann::json send_request(
        const std::string& method,
        const std::string& endpoint,
        const nlohmann::json& params = nlohmann::json::object(),
        bool need_signature = false
    );
    
    // 辅助方法
    std::string order_side_to_string(OrderSide side);
    std::string order_type_to_string(OrderType type);
    std::string time_in_force_to_string(TimeInForce tif);
    std::string position_side_to_string(PositionSide ps);
    int64_t get_timestamp();

    // 成员变量
    std::string api_key_;
    std::string secret_key_;
    std::string base_url_;
    MarketType market_type_;
    bool is_testnet_;
    core::ProxyConfig proxy_config_;
    int64_t time_offset_ms_ = 0;  // 本地时间与Binance服务器时间的偏移量
};

} // namespace binance
} // namespace trading
