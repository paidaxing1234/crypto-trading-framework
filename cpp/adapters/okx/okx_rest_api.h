#pragma once

#include <string>
#include <memory>
#include <vector>
#include <optional>
#include <atomic>
#include <nlohmann/json.hpp>
#include "../../network/proxy_config.h"

namespace trading {
namespace okx {

// ==================== CURL 中断控制 ====================

/**
 * @brief 设置 CURL 中断标志
 * 
 * 在程序退出时调用（如信号处理函数中），可以中断正在进行的 CURL 请求
 * 
 * @param abort true = 中断所有请求, false = 允许请求
 */
void set_curl_abort_flag(bool abort);

/**
 * @brief 获取 CURL 中断标志状态
 */
bool get_curl_abort_flag();

// ==================== 数据结构定义 ====================

/**
 * @brief 止盈止损订单参数
 * 
 * 用于下单时附带止盈止损信息
 */
struct AttachAlgoOrder {
    std::string attach_algo_id;         // 附带止盈止损的订单ID（改单时必填，由系统生成）
    std::string attach_algo_cl_ord_id;  // 客户自定义策略订单ID (可选)
    
    // 止盈参数
    std::string tp_trigger_px;           // 止盈触发价
    std::string tp_trigger_ratio;        // 止盈触发比例 (如 "0.3" 表示 30%)
    std::string tp_ord_px;               // 止盈委托价 ("-1" 表示市价)
    std::string tp_ord_kind;             // 止盈订单类型: "condition"(条件单) 或 "limit"(限价单)
    std::string tp_trigger_px_type;      // 触发价类型: "last"/"index"/"mark"
    
    // 止损参数
    std::string sl_trigger_px;           // 止损触发价
    std::string sl_trigger_ratio;        // 止损触发比例
    std::string sl_ord_px;               // 止损委托价 ("-1" 表示市价)
    std::string sl_trigger_px_type;      // 触发价类型: "last"/"index"/"mark"
    
    // 分批止盈参数
    std::string sz;                      // 数量 (分批止盈必填)
    std::string amend_px_on_trigger_type; // 是否启用开仓价止损: "0"/"1"
    
    // 转换为JSON
    nlohmann::json to_json() const;
};

/**
 * @brief 下单请求参数
 * 
 * 完整支持OKX下单API的所有参数
 * 参考: https://www.okx.com/docs-v5/zh/#order-book-trading-trade-post-place-order
 */
struct PlaceOrderRequest {
    // ========== 必填参数 ==========
    std::string inst_id;      // 产品ID，如 "BTC-USDT"
    std::string td_mode;      // 交易模式: "cash"/"isolated"/"cross"/"spot_isolated"
    std::string side;         // 订单方向: "buy"/"sell"
    std::string ord_type;     // 订单类型: "market"/"limit"/"post_only"/"fok"/"ioc"等
    std::string sz;           // 委托数量
    
    // ========== 可选参数 ==========
    std::string ccy;          // 保证金币种 (逐仓杠杆/合约全仓杠杆)
    std::string cl_ord_id;    // 客户自定义订单ID (1-32位)
    std::string tag;          // 订单标签 (1-16位)
    std::string pos_side;     // 持仓方向: "long"/"short" (开平仓模式必填)
    std::string px;           // 委托价格 (限价单必填)
    std::string px_usd;       // 以USD价格下单 (仅期权)
    std::string px_vol;       // 以隐含波动率下单 (仅期权)
    
    bool reduce_only = false;       // 是否只减仓
    std::string tgt_ccy;            // 市价单数量单位: "base_ccy"/"quote_ccy"
    bool ban_amend = false;         // 是否禁止币币市价改单
    std::string px_amend_type;      // 价格修正类型: "0"/"1"
    std::string trade_quote_ccy;    // 用于交易的计价币种
    std::string stp_mode;           // 自成交保护: "cancel_maker"/"cancel_taker"/"cancel_both"
    
    // 止盈止损
    std::vector<AttachAlgoOrder> attach_algo_ords;  // 附带止盈止损订单
    
    // 转换为JSON
    nlohmann::json to_json() const;
};

/**
 * @brief 下单响应
 */
struct PlaceOrderResponse {
    std::string code;         // 结果代码，"0"表示成功
    std::string msg;          // 错误信息
    std::string ord_id;       // 订单ID
    std::string cl_ord_id;    // 客户自定义订单ID
    std::string tag;          // 订单标签
    int64_t ts;               // 时间戳
    std::string s_code;       // 事件执行结果code
    std::string s_msg;        // 事件执行消息
    int64_t in_time;          // 网关接收时间 (微秒)
    int64_t out_time;         // 网关发送时间 (微秒)
    
    // 从JSON解析
    static PlaceOrderResponse from_json(const nlohmann::json& j);
    
    // 是否成功
    bool is_success() const { return code == "0" && s_code == "0"; }
};

/**
 * @brief 策略委托下单请求参数
 * 
 * 支持多种策略委托类型：
 * - conditional: 单向止盈止损
 * - oco: 双向止盈止损
 * - trigger: 计划委托
 * - move_order_stop: 移动止盈止损
 * - twap: 时间加权委托
 * - chase: 追逐限价委托
 * 
 * 参考: https://www.okx.com/docs-v5/zh/#trading-account-rest-api-post-place-algo-order
 */
struct PlaceAlgoOrderRequest {
    // ========== 必填参数 ==========
    std::string inst_id;      // 产品ID，如 "BTC-USDT-SWAP"
    std::string td_mode;      // 交易模式: "cash"/"isolated"/"cross"
    std::string side;         // 订单方向: "buy"/"sell"
    std::string ord_type;     // 订单类型: "conditional"/"oco"/"trigger"/"move_order_stop"/"twap"/"chase"
    
    // ========== 通用可选参数 ==========
    std::string sz;           // 委托数量
    std::string ccy;          // 保证金币种
    std::string pos_side;     // 持仓方向: "long"/"short"/"net"
    std::string tag;          // 订单标签
    std::string tgt_ccy;      // 委托数量类型: "base_ccy"/"quote_ccy"
    std::string algo_cl_ord_id;  // 客户自定义策略订单ID
    std::string close_fraction;  // 平仓百分比 (如 "1" 代表100%)
    bool reduce_only = false;    // 是否只减仓
    
    // ========== 止盈止损参数 (conditional/oco) ==========
    std::string tp_trigger_px;       // 止盈触发价
    std::string tp_trigger_px_type;  // 止盈触发价类型: "last"/"index"/"mark"
    std::string tp_ord_px;           // 止盈委托价 ("-1"表示市价)
    std::string tp_ord_kind;         // 止盈订单类型: "condition"/"limit"
    
    std::string sl_trigger_px;       // 止损触发价
    std::string sl_trigger_px_type;  // 止损触发价类型: "last"/"index"/"mark"
    std::string sl_ord_px;           // 止损委托价 ("-1"表示市价)
    
    bool cxl_on_close_pos = false;   // 仓位平仓时是否撤单
    
    // ========== 计划委托参数 (trigger) ==========
    std::string trigger_px;          // 触发价
    std::string order_px;            // 委托价 ("-1"表示市价)
    std::string trigger_px_type;     // 触发价类型: "last"/"index"/"mark"
    std::string advance_ord_type;    // 高级订单类型: "fok"/"ioc"
    std::vector<AttachAlgoOrder> attach_algo_ords;  // 附带止盈止损
    
    // ========== 移动止盈止损参数 (move_order_stop) ==========
    std::string callback_ratio;      // 回调幅度比例 (如 "0.05" 代表 5%)
    std::string callback_spread;     // 回调幅度价距
    std::string active_px;           // 激活价格
    
    // ========== 时间加权参数 (twap) ==========
    std::string sz_limit;            // 单笔数量
    std::string px_limit;            // 吃单限制价
    std::string time_interval;       // 下单间隔(秒)
    std::string px_var;              // 价格优于盘口的比例
    std::string px_spread;           // 价格优于盘口的价距
    
    // ========== 追逐限价委托参数 (chase) ==========
    std::string chase_type;          // 追逐类型: "distance"/"ratio"
    std::string chase_val;           // 追逐值
    std::string max_chase_type;      // 最大追逐值类型: "distance"/"ratio"
    std::string max_chase_val;       // 最大追逐值
    
    // 转换为JSON
    nlohmann::json to_json() const;
};

/**
 * @brief 策略委托响应
 */
struct PlaceAlgoOrderResponse {
    std::string code;            // 结果代码，"0"表示成功
    std::string msg;             // 错误信息
    std::string algo_id;         // 策略委托单ID
    std::string cl_ord_id;       // 客户自定义订单ID (已废弃)
    std::string algo_cl_ord_id;  // 客户自定义策略订单ID
    std::string s_code;          // 事件执行结果code
    std::string s_msg;           // 事件执行消息
    std::string tag;             // 订单标签
    
    // 从JSON解析
    static PlaceAlgoOrderResponse from_json(const nlohmann::json& j);
    
    // 是否成功
    bool is_success() const { return code == "0" && s_code == "0"; }
};

/**
 * @brief 修改策略委托订单请求参数
 * 
 * 仅支持止盈止损和计划委托订单的修改
 * （不包括冰山委托、时间加权、移动止盈止损等）
 */
struct AmendAlgoOrderRequest {
    // ========== 必填参数 ==========
    std::string inst_id;         // 产品ID
    
    // ========== ID参数（必须传一个） ==========
    std::string algo_id;         // 策略委托单ID
    std::string algo_cl_ord_id;  // 客户自定义策略订单ID
    
    // ========== 通用可选参数 ==========
    bool cxl_on_fail = false;    // 修改失败时是否自动撤单
    std::string req_id;          // 用户自定义修改事件ID
    std::string new_sz;          // 修改的新数量
    
    // ========== 止盈止损修改参数 ==========
    std::string new_tp_trigger_px;       // 止盈触发价
    std::string new_tp_ord_px;           // 止盈委托价
    std::string new_tp_trigger_px_type;  // 止盈触发价类型
    std::string new_sl_trigger_px;       // 止损触发价
    std::string new_sl_ord_px;           // 止损委托价
    std::string new_sl_trigger_px_type;  // 止损触发价类型
    
    // ========== 计划委托修改参数 ==========
    std::string new_trigger_px;          // 修改后的触发价格
    std::string new_ord_px;              // 修改后的委托价格
    std::string new_trigger_px_type;     // 修改后的触发价格类型
    
    // ========== 附带止盈止损修改 ==========
    struct AttachAlgoAmend {
        std::string new_tp_trigger_px;
        std::string new_tp_trigger_ratio;
        std::string new_tp_trigger_px_type;
        std::string new_tp_ord_px;
        std::string new_sl_trigger_px;
        std::string new_sl_trigger_ratio;
        std::string new_sl_trigger_px_type;
        std::string new_sl_ord_px;
        
        nlohmann::json to_json() const;
    };
    std::vector<AttachAlgoAmend> attach_algo_ords;
    
    // 转换为JSON
    nlohmann::json to_json() const;
};

/**
 * @brief 修改策略委托响应
 */
struct AmendAlgoOrderResponse {
    std::string code;            // 结果代码，"0"表示成功
    std::string msg;             // 错误信息
    std::string algo_id;         // 策略委托单ID
    std::string algo_cl_ord_id;  // 客户自定义策略订单ID
    std::string req_id;          // 用户自定义修改事件ID
    std::string s_code;          // 事件执行结果code
    std::string s_msg;           // 事件执行消息
    
    // 从JSON解析
    static AmendAlgoOrderResponse from_json(const nlohmann::json& j);
    
    // 是否成功
    bool is_success() const { return code == "0" && s_code == "0"; }
};

// ==================== API类定义 ====================

/**
 * @brief OKX REST API封装
 * 
 * 提供以下功能：
 * - 下单（限价、市价、止盈止损）
 * - 撤单
 * - 查询订单
 * - 查询账户余额
 * - 查询持仓
 */
class OKXRestAPI {
public:
    OKXRestAPI(
        const std::string& api_key,
        const std::string& secret_key,
        const std::string& passphrase,
        bool is_testnet = false,
        const core::ProxyConfig& proxy_config = core::ProxyConfig::get_default()
    );
    
    ~OKXRestAPI() = default;
    
    // ==================== 下单接口 ====================
    
    /**
     * @brief 简化版下单 (向后兼容)
     */
    nlohmann::json place_order(
        const std::string& inst_id,
        const std::string& td_mode,
        const std::string& side,
        const std::string& ord_type,
        double sz,
        double px = 0.0,
        const std::string& cl_ord_id = ""
    );
    
    /**
     * @brief 完整版下单 (支持所有参数包括止盈止损)
     */
    PlaceOrderResponse place_order_advanced(const PlaceOrderRequest& request);
    
    /**
     * @brief 带止盈止损的下单 (便捷方法)
     */
    PlaceOrderResponse place_order_with_tp_sl(
        const std::string& inst_id,
        const std::string& td_mode,
        const std::string& side,
        const std::string& ord_type,
        const std::string& sz,
        const std::string& px,
        const std::string& tp_trigger_px = "",
        const std::string& tp_ord_px = "-1",
        const std::string& sl_trigger_px = "",
        const std::string& sl_ord_px = "-1",
        const std::string& cl_ord_id = ""
    );
    
    /**
     * @brief 批量下单
     * 
     * 每次最多可以批量提交20个新订单
     * 
     * @param orders 订单请求数组，每个元素是一个PlaceOrderRequest
     * @return nlohmann::json 包含所有订单的响应结果
     */
    nlohmann::json place_batch_orders(const std::vector<PlaceOrderRequest>& orders);
    
    // ==================== 策略委托接口 ====================
    
    /**
     * @brief 策略委托下单
     * 
     * 支持以下策略类型：
     * - conditional: 单向止盈止损
     * - oco: 双向止盈止损
     * - trigger: 计划委托
     * - move_order_stop: 移动止盈止损
     * - twap: 时间加权委托
     * - chase: 追逐限价委托
     * 
     * @param request 策略委托请求参数
     * @return PlaceAlgoOrderResponse 策略委托响应
     */
    PlaceAlgoOrderResponse place_algo_order(const PlaceAlgoOrderRequest& request);
    
    /**
     * @brief 便捷方法：单向止盈止损委托
     */
    PlaceAlgoOrderResponse place_conditional_order(
        const std::string& inst_id,
        const std::string& td_mode,
        const std::string& side,
        const std::string& sz,
        const std::string& tp_trigger_px = "",
        const std::string& tp_ord_px = "-1",
        const std::string& sl_trigger_px = "",
        const std::string& sl_ord_px = "-1",
        const std::string& pos_side = ""
    );
    
    /**
     * @brief 便捷方法：计划委托
     */
    PlaceAlgoOrderResponse place_trigger_order(
        const std::string& inst_id,
        const std::string& td_mode,
        const std::string& side,
        const std::string& sz,
        const std::string& trigger_px,
        const std::string& order_px = "-1",
        const std::string& pos_side = ""
    );
    
    /**
     * @brief 便捷方法：移动止盈止损委托
     */
    PlaceAlgoOrderResponse place_move_stop_order(
        const std::string& inst_id,
        const std::string& td_mode,
        const std::string& side,
        const std::string& sz,
        const std::string& callback_ratio,
        const std::string& active_px = "",
        const std::string& pos_side = ""
    );
    
    /**
     * @brief 撤销策略委托订单
     * 
     * 每次最多可以撤销10个策略委托单
     * 
     * @param inst_id 产品ID
     * @param algo_id 策略委托单ID
     * @param algo_cl_ord_id 客户自定义策略订单ID
     * @return nlohmann::json 撤销结果
     * 
     * 注意：algo_id和algo_cl_ord_id必须传一个，若传两个，以algo_id为主
     */
    nlohmann::json cancel_algo_order(
        const std::string& inst_id,
        const std::string& algo_id = "",
        const std::string& algo_cl_ord_id = ""
    );
    
    /**
     * @brief 批量撤销策略委托订单
     * 
     * 每次最多可以撤销10个策略委托单
     * 
     * @param orders 撤销请求数组，每个元素包含instId和algoId/algoClOrdId
     * @return nlohmann::json 批量撤销结果
     */
    nlohmann::json cancel_algo_orders(const std::vector<nlohmann::json>& orders);
    
    /**
     * @brief 修改策略委托订单
     * 
     * 仅支持止盈止损和计划委托订单的修改
     * （不包括冰山委托、时间加权、移动止盈止损等）
     * 
     * @param request 修改请求参数
     * @return AmendAlgoOrderResponse 修改结果
     */
    AmendAlgoOrderResponse amend_algo_order(const AmendAlgoOrderRequest& request);
    
    /**
     * @brief 便捷方法：修改计划委托的触发价和委托价
     */
    AmendAlgoOrderResponse amend_trigger_order(
        const std::string& inst_id,
        const std::string& algo_id,
        const std::string& new_trigger_px,
        const std::string& new_ord_px
    );
    
    /**
     * @brief 获取策略委托单信息
     * 
     * @param algo_id 策略委托单ID
     * @param algo_cl_ord_id 客户自定义策略订单ID
     * @return nlohmann::json 策略委托单详细信息
     * 
     * 注意：algo_id和algo_cl_ord_id必须传一个，若传两个，以algo_id为主
     */
    nlohmann::json get_algo_order(
        const std::string& algo_id = "",
        const std::string& algo_cl_ord_id = ""
    );
    
    /**
     * @brief 获取未完成策略委托单列表
     * 
     * @param ord_type 订单类型（必填），支持：
     *                 - conditional: 单向止盈止损
     *                 - oco: 双向止盈止损
     *                 - trigger: 计划委托
     *                 - move_order_stop: 移动止盈止损
     *                 - twap: 时间加权委托
     *                 - chase: 追逐限价委托
     *                 支持conditional和oco同时查询，用逗号分隔
     * @param inst_type 产品类型（可选）：SPOT/SWAP/FUTURES/MARGIN
     * @param inst_id 产品ID（可选），如 BTC-USDT
     * @param after 请求此ID之前的分页内容
     * @param before 请求此ID之后的分页内容
     * @param limit 返回结果数量，最大100，默认100
     * @return nlohmann::json 未完成策略委托单列表
     */
    nlohmann::json get_algo_orders_pending(
        const std::string& ord_type,
        const std::string& inst_type = "",
        const std::string& inst_id = "",
        const std::string& after = "",
        const std::string& before = "",
        int limit = 100
    );
    
    /**
     * @brief 获取历史策略委托单列表
     * 
     * 获取最近3个月当前账户下所有策略委托单列表
     * 
     * @param ord_type 订单类型（必填），同get_algo_orders_pending
     * @param state 订单状态（可选）：effective/canceled/order_failed
     *              注意：state和algo_id必填且只能填其一
     * @param algo_id 策略委托单ID（可选）
     *                注意：state和algo_id必填且只能填其一
     * @param inst_type 产品类型（可选）：SPOT/SWAP/FUTURES/MARGIN
     * @param inst_id 产品ID（可选），如 BTC-USDT
     * @param after 请求此ID之前的分页内容
     * @param before 请求此ID之后的分页内容
     * @param limit 返回结果数量，最大100，默认100
     * @return nlohmann::json 历史策略委托单列表
     */
    nlohmann::json get_algo_orders_history(
        const std::string& ord_type,
        const std::string& state = "",
        const std::string& algo_id = "",
        const std::string& inst_type = "",
        const std::string& inst_id = "",
        const std::string& after = "",
        const std::string& before = "",
        int limit = 100
    );
    
    // ==================== 撤单接口 ====================
    
    nlohmann::json cancel_order(
        const std::string& inst_id,
        const std::string& ord_id = "",
        const std::string& cl_ord_id = ""
    );
    
    nlohmann::json cancel_batch_orders(
        const std::vector<std::string>& ord_ids,
        const std::string& inst_id
    );
    
    // ==================== 修改订单接口 ====================
    
    /**
     * @brief 修改订单
     * 
     * 修改当前未成交的挂单
     * 
     * @param inst_id 产品ID
     * @param ord_id 订单ID（与cl_ord_id二选一，优先使用ord_id）
     * @param cl_ord_id 客户自定义订单ID
     * @param new_sz 修改的新数量（可选）
     * @param new_px 修改后的新价格（可选）
     * @param new_px_usd 以USD价格进行期权改单（可选，仅期权）
     * @param new_px_vol 以隐含波动率进行期权改单（可选，仅期权）
     * @param cxl_on_fail 当订单修改失败时，该订单是否需要自动撤销（默认false）
     * @param req_id 用户自定义修改事件ID（可选）
     * @param px_amend_type 订单价格修正类型："0"不允许系统修改，"1"允许（默认"0"）
     * @param attach_algo_ords 修改附带止盈止损信息（可选）
     * @return nlohmann::json 修改结果
     */
    nlohmann::json amend_order(
        const std::string& inst_id,
        const std::string& ord_id = "",
        const std::string& cl_ord_id = "",
        const std::string& new_sz = "",
        const std::string& new_px = "",
        const std::string& new_px_usd = "",
        const std::string& new_px_vol = "",
        bool cxl_on_fail = false,
        const std::string& req_id = "",
        const std::string& px_amend_type = "0",
        const std::vector<AttachAlgoOrder>& attach_algo_ords = {}
    );
    
    /**
     * @brief 批量修改订单
     * 
     * 修改未完成的订单，一次最多可批量修改20个订单
     * 
     * @param orders 修改订单请求数组，每个元素包含instId、ordId/clOrdId、newSz/newPx等
     * @return nlohmann::json 包含所有修改结果的响应
     */
    nlohmann::json amend_batch_orders(const std::vector<nlohmann::json>& orders);
    
    // ==================== 查询接口 ====================
    
    nlohmann::json get_order(
        const std::string& inst_id,
        const std::string& ord_id = "",
        const std::string& cl_ord_id = ""
    );
    
    nlohmann::json get_pending_orders(
        const std::string& inst_type = "",
        const std::string& inst_id = ""
    );
    
    nlohmann::json get_account_balance(const std::string& ccy = "");
    
    nlohmann::json get_positions(
        const std::string& inst_type = "",
        const std::string& inst_id = ""
    );
    
    nlohmann::json get_account_instruments(
        const std::string& inst_type,
        const std::string& inst_family = "",
        const std::string& inst_id = ""
    );
    
    nlohmann::json get_candles(
        const std::string& inst_id,
        const std::string& bar,
        int64_t after = 0,
        int64_t before = 0,
        int limit = 100
    );

    /**
     * @brief 获取历史K线数据（用于拉取历史数据）
     *
     * 获取历史K线数据（最多可获取最近3个月的数据）
     * 限速：20次/2s
     *
     * @param inst_id 产品ID，如 "BTC-USDT-SWAP"
     * @param bar K线周期，如 "1m", "5m", "15m", "30m", "1H", "4H", "1D"
     * @param after 请求此时间戳之前的数据（更早的数据，时间戳 < after）
     * @param before 请求此时间戳之后的数据（更新的数据，时间戳 > before）
     * @param limit 返回结果的数量，最大值为100，默认100
     * @return nlohmann::json K线数据数组（降序排列）
     */
    nlohmann::json get_history_candles(
        const std::string& inst_id,
        const std::string& bar,
        int64_t after = 0,
        int64_t before = 0,
        int limit = 100
    );

    /**
     * @brief 获取永续合约资金费率
     * 
     * 获取当前资金费率
     * 限速：10次/2s
     * 限速规则：IP + Instrument ID
     * 
     * @param inst_id 产品ID，如 "BTC-USD-SWAP" 或 "ANY" 以返回所有永续合约的资金费率信息
     *                仅适用于永续合约
     * @return nlohmann::json 资金费率数据，包含：
     *         - instType: 产品类型 SWAP
     *         - instId: 产品ID
     *         - fundingRate: 资金费率
     *         - nextFundingRate: 下一期预测资金费率
     *         - fundingTime: 资金费时间（毫秒时间戳）
     *         - nextFundingTime: 下一期资金费时间（毫秒时间戳）
     *         - minFundingRate: 资金费率下限
     *         - maxFundingRate: 资金费率上限
     *         - settState: 资金费率结算状态（processing/settled）
     *         - settFundingRate: 结算资金费率
     *         - premium: 溢价指数
     *         - ts: 数据更新时间（毫秒时间戳）
     * 
     * 注意：用户应关注 fundingTime 及 nextFundingTime 字段以确定合约的资金费收取频率
     *      （可能是8小时/6小时/4小时/2小时/1小时收付）
     */
    nlohmann::json get_funding_rate(const std::string& inst_id = "BTC-USDT-SWAP");

    /**
     * @brief 获取所有交易产品信息（公共接口）
     *
     * @param inst_type 产品类型：SPOT, MARGIN, SWAP, FUTURES, OPTION
     * @return nlohmann::json 产品列表
     */
    nlohmann::json get_instruments(const std::string& inst_type);

    // ==================== 代理设置 ====================
    
    /**
     * @brief 设置HTTP代理
     * 
     * @param proxy_host 代理主机地址（如 "127.0.0.1"）
     * @param proxy_port 代理端口（如 7890）
     */
    void set_proxy(const std::string& proxy_host, uint16_t proxy_port);

private:
    std::string create_signature(
        const std::string& timestamp,
        const std::string& method,
        const std::string& request_path,
        const std::string& body
    );
    
    nlohmann::json send_request(
        const std::string& method,
        const std::string& endpoint,
        const nlohmann::json& params = nlohmann::json::object()
    );
    
    std::string get_iso8601_timestamp();
    
    std::string api_key_;
    std::string secret_key_;
    std::string passphrase_;
    std::string base_url_;
    bool is_testnet_;
    core::ProxyConfig proxy_config_;
};

} // namespace okx
} // namespace trading
