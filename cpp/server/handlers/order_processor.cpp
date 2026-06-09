/**
 * @file order_processor.cpp
 * @brief 订单处理模块实现
 */

#include "order_processor.h"
#include "../config/server_config.h"
#include "../managers/account_manager.h"
#include "../managers/account_monitor.h"  // 账户监控模块
#include "../../trading/account_registry.h"
#include "../../trading/risk_manager.h"  // 风控管理器
#include "../../adapters/okx/okx_rest_api.h"
#include "../../adapters/binance/binance_rest_api.h"
#include "../../core/logger.h"
#include "../../network/websocket_server.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <functional>
#include <future>

using namespace trading::core;

namespace trading {
namespace server {

// 全局风控管理器实例（非 static，可被其他模块访问）
// 从配置文件加载风控参数，并配置邮件告警
static std::string get_project_root() {
    // 可执行文件在 cpp/build/ 下，项目根目录是 ../../
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe");
    return exe_path.parent_path().parent_path().string();  // cpp/build -> cpp/
}

static RiskManager create_risk_manager() {
    std::string cpp_dir = get_project_root();

    // 创建告警配置
    AlertConfig alert_config;
    alert_config.email_enabled = true;
    alert_config.email_config_file = cpp_dir + "/trading/alerts/email_config.json";
    alert_config.lark_enabled = true;
    alert_config.lark_config_file = cpp_dir + "/trading/alerts/lark_config.json";

    // 加载风控限制并创建风控管理器
    return RiskManager(
        RiskLimits::from_file(cpp_dir + "/risk_config.json"),
        alert_config
    );
}

RiskManager g_risk_manager = create_risk_manager();

// 全局账户监控器指针（在 trading_server_main.cpp 中初始化）
AccountMonitor* g_account_monitor = nullptr;

// strategy_id → account_id 映射，用于日志 source
static std::mutex g_sa_map_mutex;
static std::map<std::string, std::string> g_strategy_account_map;

// 获取 account_id（用于告警消息等）
static std::string get_account_id(const std::string& strategy_id) {
    std::lock_guard<std::mutex> lock(g_sa_map_mutex);
    auto it = g_strategy_account_map.find(strategy_id);
    if (it != g_strategy_account_map.end() && !it->second.empty()) {
        return it->second;
    }
    return "";
}

// 获取日志 source: "account_id_strategy_id" 或 fallback 到 "strategy_id"
static std::string get_log_source(const std::string& strategy_id) {
    std::lock_guard<std::mutex> lock(g_sa_map_mutex);
    auto it = g_strategy_account_map.find(strategy_id);
    if (it != g_strategy_account_map.end() && !it->second.empty()) {
        return it->second + "_" + strategy_id;
    }
    return strategy_id;
}

void process_place_order(ZmqServer& server, const nlohmann::json& order) {
    g_order_count++;

    std::string strategy_id = order.value("strategy_id", "unknown");
    std::string client_order_id = order.value("client_order_id", "");
    std::string symbol = order.value("symbol", "BTC-USDT");
    std::string side = order.value("side", "buy");
    std::string order_type = order.value("order_type", "limit");
    double price = order.value("price", 0.0);
    double quantity = order.value("quantity", 0.0);
    std::string td_mode = order.value("td_mode", "cash");
    std::string pos_side = order.value("pos_side", "");
    std::string tgt_ccy = order.value("tgt_ccy", "");

    LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "RECEIVED", "symbol=" + symbol + " side=" + side + " qty=" + std::to_string(quantity));
    LOG_AUDIT_SRC(get_log_source(strategy_id), "ORDER_SUBMIT", "order_id=" + client_order_id + " symbol=" + symbol);

    Logger::instance().info(get_log_source(strategy_id), "[下单] " + symbol + " | " + side + " " + order_type + " | 数量: " + std::to_string(quantity));

    // 🆕 验证策略是否已注册
    // TODO: 实现 is_strategy_registered 函数
    /*
    if (!is_strategy_registered(strategy_id)) {
        std::string error_msg = "策略 " + strategy_id + " 未注册账户";
        std::cout << "[下单] ✗ " << error_msg << "\n";
        LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "REJECTED", "reason=" + error_msg);
        g_order_failed++;

        nlohmann::json report = make_order_report(
            strategy_id, client_order_id, "", symbol,
            "rejected", price, quantity, 0.0, error_msg
        );
        server.publish_report(report);
        return;
    }
    */

    // ========== 风控检查 ==========
    // 在实盘交易前进行风控检查
    OrderSide order_side = (side == "buy" || side == "BUY") ? OrderSide::BUY : OrderSide::SELL;

    // 获取订单金额用于风控检查
    double order_value = 0.0;
    double check_price = price;

    // 优先使用策略端传来的 order_value（避免OKX张数/Binance币数计算问题）
    if (order.contains("order_value")) {
        order_value = order.value("order_value", 0.0);
        Logger::instance().info(get_log_source(strategy_id), "[风控] 使用策略端订单金额: " + std::to_string(order_value) + " USDT");
    } else {
        // 如果没有 order_value，则使用 estimated_price * quantity 计算
        if ((price == 0.0 || order_type == "market") && order.contains("estimated_price")) {
            check_price = order.value("estimated_price", 0.0);
            Logger::instance().info(get_log_source(strategy_id), "[风控] 市价单使用估算价格: " + std::to_string(check_price) + " USDT");
        }
        order_value = check_price * quantity;
        Logger::instance().info(get_log_source(strategy_id), "[风控] 计算订单金额: " + std::to_string(check_price) + " × " + std::to_string(quantity) + " = " + std::to_string(order_value) + " USDT");
    }

    // 调试：打印订单信息和当前风控限制
    auto current_limits = g_risk_manager.get_limits();
    Logger::instance().info(get_log_source(strategy_id), "[风控] 检查订单: " + symbol + " " + side + " 订单金额=" + std::to_string(order_value) + " USDT");
    Logger::instance().info(get_log_source(strategy_id), "[风控] 当前限制: max_order_value=" + std::to_string(current_limits.max_order_value) + " USDT");

    // 使用 check_order_with_value 传入准确的订单金额
    RiskCheckResult risk_result = g_risk_manager.check_order_with_value(symbol, order_side, check_price, quantity, order_value, strategy_id);

    if (!risk_result.passed) {
        // 风控检查失败，拒绝订单
        std::string error_msg = "[风控拒绝] " + risk_result.reason;
        Logger::instance().error(get_log_source(strategy_id), "[下单] ✗ " + error_msg);
        LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "RISK_REJECTED", "reason=" + risk_result.reason);
        g_order_failed++;

        nlohmann::json report = make_order_report(
            strategy_id, client_order_id, "", symbol,
            "rejected", price, quantity, 0.0, error_msg
        );
        report["risk_check"] = false;
        report["risk_reason"] = risk_result.reason;
        server.publish_report(report);

        // 发送到前端 WebSocket
        if (g_frontend_server) {
            g_frontend_server->send_event("order_report", report);
        }

        // 发送风控告警
        std::string acct = get_account_id(strategy_id);
        std::string acct_info = acct.empty() ? "" : (" 账户: " + acct);
        g_risk_manager.send_alert(
            "订单被风控拒绝: " + strategy_id + acct_info + " " + symbol + " " + side + " " + std::to_string(quantity) +
            "\n原因: " + risk_result.reason,
            AlertLevel::WARNING,
            "风控拒绝订单"
        );
        return;
    }

    // 风控检查通过，记录订单执行
    g_risk_manager.record_order_execution();
    Logger::instance().info(get_log_source(strategy_id), "[风控] ✓ 订单通过风控检查");
    // ========== 风控检查结束 ==========

    // 获取交易所类型
    std::string exchange = order.value("exchange", "okx");
    std::transform(exchange.begin(), exchange.end(), exchange.begin(), ::tolower);

    // 根据交易所类型处理订单
    if (exchange == "binance") {
        // Binance 下单处理
        binance::BinanceRestAPI* binance_api = get_binance_api_for_strategy(strategy_id);
        if (!binance_api) {
            std::string error_msg = "策略 " + strategy_id + " 未注册Binance账户，且无默认账户";
            Logger::instance().error(get_log_source(strategy_id), "[下单] ✗ " + error_msg);
            LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "REJECTED", "reason=" + error_msg);
            g_order_failed++;

            nlohmann::json report = make_order_report(
                strategy_id, client_order_id, "", symbol,
                "rejected", price, quantity, 0.0, error_msg, "binance"
            );
            server.publish_report(report);
            return;
        }

        bool success = false;
        std::string exchange_order_id;
        std::string error_msg;
        bool is_filled = false;          // newOrderRespType=RESULT 响应里已确认成交
        double fill_qty = 0.0;           // 真实成交数量(executedQty)
        double fill_px = 0.0;            // 真实成交均价(avgPrice)

        try {
            // 转换订单参数
            binance::OrderSide binance_side = (side == "buy") ? binance::OrderSide::BUY : binance::OrderSide::SELL;
            binance::OrderType binance_type = (order_type == "market") ? binance::OrderType::MARKET : binance::OrderType::LIMIT;
            binance::PositionSide binance_pos_side = binance::PositionSide::BOTH;

            if (!pos_side.empty()) {
                if (pos_side == "LONG") binance_pos_side = binance::PositionSide::LONG;
                else if (pos_side == "SHORT") binance_pos_side = binance::PositionSide::SHORT;
            }

            auto send_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            auto response = binance_api->place_order(
                symbol,
                binance_side,
                binance_type,
                std::to_string(quantity),
                (price > 0 && order_type != "market") ? std::to_string(price) : "",
                binance::TimeInForce::GTC,
                binance_pos_side,
                client_order_id
            );
            auto resp_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();

            if (response.contains("orderId")) {
                success = true;
                exchange_order_id = std::to_string(response["orderId"].get<int64_t>());
                g_order_success++;
                // newOrderRespType=RESULT: 响应自带 status/avgPrice/executedQty(真实成交)
                std::string ord_status = response.value("status", "");
                try { fill_qty = std::stod(response.value("executedQty", "0")); } catch (...) {}
                try { fill_px = std::stod(response.value("avgPrice", "0")); } catch (...) {}
                is_filled = (ord_status == "FILLED" || ord_status == "PARTIALLY_FILLED") && fill_qty > 0;
                LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "ACCEPTED", "exchange_id=" + exchange_order_id
                    + (is_filled ? (" filled=" + response.value("executedQty", "0") + "@" + response.value("avgPrice", "0")) : ""));
                Logger::instance().info(get_log_source(strategy_id), "[Binance响应] 订单ID: " + client_order_id + " | 往返: " + std::to_string((resp_ns - send_ns) / 1000000) + " ms | ✓");
            } else {
                error_msg = response.value("msg", "未知错误");
                g_order_failed++;
                LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "REJECTED", "reason=" + error_msg);
                Logger::instance().error(get_log_source(strategy_id), "[Binance响应] ✗ " + error_msg);
            }
        } catch (const std::exception& e) {
            error_msg = std::string("Binance API异常: ") + e.what();
            g_order_failed++;
            LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "REJECTED", "reason=" + error_msg);
            Logger::instance().error(get_log_source(strategy_id), "[Binance异常] " + error_msg);
        }

        // 下单失败时发送邮件通知
        if (!success && !error_msg.empty()) {
            std::string acct = get_account_id(strategy_id);
            std::string acct_line = acct.empty() ? "" : ("账户: " + acct + "\n");
            std::string email_body = acct_line + "策略: " + strategy_id + "\n交易对: " + symbol + "\n方向: " + side +
                "\n数量: " + std::to_string(quantity) + "\n失败原因: " + error_msg;
            g_risk_manager.send_risk_alert_to_strategy(strategy_id, email_body, "Binance下单失败");
        }

        // 发送订单回报: RESULT 响应已确认成交则带真实成交均价/数量, 否则维持原行为
        nlohmann::json report = make_order_report(
            strategy_id, client_order_id, exchange_order_id, symbol,
            success ? (is_filled ? "filled" : "submitted") : "rejected",
            is_filled ? fill_px : price,
            is_filled ? fill_qty : quantity,
            0.0, error_msg, "binance"
        );
        report["side"] = side;
        server.publish_report(report);

        if (g_frontend_server) {
            g_frontend_server->send_event("order_report", report);
        }
        return;
    }

    // OKX 下单处理（原有逻辑）
    okx::OKXRestAPI* api = get_api_for_strategy(strategy_id);
    if (!api) {
        std::string error_msg = "策略 " + strategy_id + " 未注册账户，且无默认账户";
        Logger::instance().error(get_log_source(strategy_id), "[下单] ✗ " + error_msg);
        LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "REJECTED", "reason=" + error_msg);
        g_order_failed++;

        nlohmann::json report = make_order_report(
            strategy_id, client_order_id, "", symbol,
            "rejected", price, quantity, 0.0, error_msg
        );
        server.publish_report(report);
        return;
    }

    bool success = false;
    std::string exchange_order_id;
    std::string error_msg;

    try {
        okx::PlaceOrderRequest req;
        req.inst_id = symbol;
        req.td_mode = td_mode;
        req.side = side;
        req.ord_type = order_type;
        req.sz = std::to_string(quantity);
        if (price > 0) req.px = std::to_string(price);
        if (!pos_side.empty()) req.pos_side = pos_side;
        if (!tgt_ccy.empty()) req.tgt_ccy = tgt_ccy;
        if (!client_order_id.empty()) req.cl_ord_id = client_order_id;

        if (order.contains("tag") && !order["tag"].is_null()) {
            req.tag = order["tag"].get<std::string>();
        }

        if (order.contains("attach_algo_ords") && order["attach_algo_ords"].is_array()) {
            for (const auto& algo_json : order["attach_algo_ords"]) {
                okx::AttachAlgoOrder algo;
                if (algo_json.contains("tp_trigger_px") && !algo_json["tp_trigger_px"].is_null()) {
                    algo.tp_trigger_px = algo_json["tp_trigger_px"].get<std::string>();
                    algo.tp_ord_px = algo_json.value("tp_ord_px", "-1");
                    algo.tp_trigger_px_type = algo_json.value("tp_trigger_px_type", "last");
                }
                if (algo_json.contains("sl_trigger_px") && !algo_json["sl_trigger_px"].is_null()) {
                    algo.sl_trigger_px = algo_json["sl_trigger_px"].get<std::string>();
                    algo.sl_ord_px = algo_json.value("sl_ord_px", "-1");
                    algo.sl_trigger_px_type = algo_json.value("sl_trigger_px_type", "last");
                }
                if (!algo.tp_trigger_px.empty() || !algo.sl_trigger_px.empty()) {
                    req.attach_algo_ords.push_back(algo);
                }
            }
        }

        auto send_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        auto response = api->place_order_advanced(req);
        auto resp_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();

        if (response.is_success()) {
            success = true;
            exchange_order_id = response.ord_id;
            g_order_success++;
            LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "ACCEPTED", "exchange_id=" + exchange_order_id);
            Logger::instance().info(get_log_source(strategy_id), "[OKX响应] 订单ID: " + client_order_id + " | 往返: " + std::to_string((resp_ns - send_ns) / 1000000) + " ms | ✓");
        } else {
            error_msg = response.s_msg.empty() ? response.msg : response.s_msg;
            g_order_failed++;
            LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "REJECTED", "error=" + error_msg);
            Logger::instance().error(get_log_source(strategy_id), "[OKX响应] 订单ID: " + client_order_id + " | 往返: " + std::to_string((resp_ns - send_ns) / 1000000) + " ms | ✗ " + error_msg);
        }
    } catch (const std::exception& e) {
        error_msg = std::string("异常: ") + e.what();
        g_order_failed++;
        LOG_ORDER_SRC(get_log_source(strategy_id), client_order_id, "ERROR", error_msg);
    }

    // 下单失败时发送邮件通知
    if (!success && !error_msg.empty()) {
        std::string acct = get_account_id(strategy_id);
        std::string acct_line = acct.empty() ? "" : ("账户: " + acct + "\n");
        std::string email_body = acct_line + "策略: " + strategy_id + "\n交易对: " + symbol + "\n方向: " + side +
            "\n数量: " + std::to_string(quantity) + "\n失败原因: " + error_msg;
        g_risk_manager.send_risk_alert_to_strategy(strategy_id, email_body, "OKX下单失败");
    }

    nlohmann::json report = make_order_report(
        strategy_id, client_order_id, exchange_order_id, symbol,
        success ? "accepted" : "rejected",
        price, quantity, 0.0, error_msg
    );
    server.publish_report(report);
}

void process_batch_orders(ZmqServer& server, const nlohmann::json& request) {
    std::string strategy_id = request.value("strategy_id", "unknown");
    std::string batch_id = request.value("batch_id", "");
    std::string exchange = request.value("exchange", "okx");
    std::transform(exchange.begin(), exchange.end(), exchange.begin(), ::tolower);

    Logger::instance().info(get_log_source(strategy_id), "[批量下单] " + batch_id + " | " + exchange);
    LOG_AUDIT_SRC(get_log_source(strategy_id), "BATCH_ORDER_SUBMIT", "batch_id=" + batch_id + " exchange=" + exchange + " count=" + std::to_string(request.contains("orders") ? request["orders"].size() : 0));

    if (!request.contains("orders") || !request["orders"].is_array()) {
        nlohmann::json report = {
            {"type", "batch_report"}, {"strategy_id", strategy_id},
            {"batch_id", batch_id}, {"status", "rejected"},
            {"error_msg", "无效的订单数组"}, {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);
        return;
    }

    // 根据交易所类型处理批量下单
    if (exchange == "binance") {
        // Binance 批量下单
        binance::BinanceRestAPI* binance_api = get_binance_api_for_strategy(strategy_id);
        if (!binance_api) {
            Logger::instance().info(get_log_source(strategy_id), "[批量下单] ✗ 策略未注册Binance账户");
            nlohmann::json report = {
                {"type", "batch_report"}, {"strategy_id", strategy_id},
                {"batch_id", batch_id}, {"status", "rejected"},
                {"error_msg", "策略未注册Binance账户"}, {"timestamp", current_timestamp_ms()}
            };
            server.publish_report(report);
            return;
        }

        // 构建 Binance 批量订单格式
        // Binance 批量下单最多支持 5 个订单，多批并发发送
        const auto& orders_json = request["orders"];
        size_t total_orders = orders_json.size();
        size_t batch_size = 5;  // Binance 每批最多 5 个订单

        int total_success = 0, total_fail = 0;
        nlohmann::json all_results = nlohmann::json::array();

        // 预构建所有批次的订单数据
        struct BatchTask {
            size_t start_idx;
            nlohmann::json batch_orders;
        };
        std::vector<BatchTask> tasks;

        for (size_t i = 0; i < total_orders; i += batch_size) {
            BatchTask task;
            task.start_idx = i;
            task.batch_orders = nlohmann::json::array();

            for (size_t j = i; j < std::min(i + batch_size, total_orders); ++j) {
                const auto& ord = orders_json[j];

                nlohmann::json binance_order;
                binance_order["symbol"] = ord.value("symbol", "BTCUSDT");
                binance_order["side"] = ord.value("side", "BUY");

                // 转换 side 为大写
                std::string side_str = binance_order["side"].get<std::string>();
                std::transform(side_str.begin(), side_str.end(), side_str.begin(), ::toupper);
                binance_order["side"] = side_str;

                // 订单类型
                std::string order_type = ord.value("order_type", "market");
                std::transform(order_type.begin(), order_type.end(), order_type.begin(), ::toupper);
                binance_order["type"] = order_type;

                // 数量 - 兼容整数和浮点数类型
                double qty = 0.0;
                if (ord.contains("quantity")) {
                    if (ord["quantity"].is_number_float()) {
                        qty = ord["quantity"].get<double>();
                    } else if (ord["quantity"].is_number_integer()) {
                        qty = static_cast<double>(ord["quantity"].get<int64_t>());
                    } else if (ord["quantity"].is_string()) {
                        try {
                            qty = std::stod(ord["quantity"].get<std::string>());
                        } catch (...) {
                            qty = 0.0;
                        }
                    }
                }
                std::string symbol = ord.value("symbol", "");
                std::string client_oid = ord.value("client_order_id", "");
                Logger::instance().info(get_log_source(strategy_id), "[批量下单] " + symbol + " quantity=" + std::to_string(qty));
                LOG_ORDER_SRC(get_log_source(strategy_id), client_oid, "BATCH_RECEIVED", "symbol=" + symbol + " side=" + side_str + " qty=" + std::to_string(qty));
                binance_order["quantity"] = std::to_string(qty);

                // 价格（限价单）
                if (order_type == "LIMIT") {
                    double px = ord.value("price", 0.0);
                    if (px > 0) {
                        binance_order["price"] = std::to_string(px);
                        binance_order["timeInForce"] = "GTC";
                    }
                }

                // 持仓方向（双向持仓模式）
                std::string pos_side = ord.value("pos_side", "BOTH");
                std::transform(pos_side.begin(), pos_side.end(), pos_side.begin(), ::toupper);
                binance_order["positionSide"] = pos_side;

                // 客户端订单ID
                if (ord.contains("client_order_id") && !ord["client_order_id"].is_null()) {
                    binance_order["newClientOrderId"] = ord["client_order_id"];
                }

                task.batch_orders.push_back(binance_order);
            }
            tasks.push_back(std::move(task));
        }

        // 并发发送所有批次（CURL 每次调用创建独立句柄，线程安全）
        struct BatchResult {
            size_t start_idx;
            size_t batch_count;
            int success;
            int fail;
            nlohmann::json results;
        };

        std::vector<std::future<BatchResult>> futures;
        for (auto& task : tasks) {
            size_t start_idx = task.start_idx;
            size_t batch_count = task.batch_orders.size();
            futures.push_back(std::async(std::launch::async,
                [binance_api, batch_orders = std::move(task.batch_orders),
                 start_idx, batch_count, &orders_json, total_orders, &strategy_id]() -> BatchResult {
                    BatchResult br;
                    br.start_idx = start_idx;
                    br.batch_count = batch_count;
                    br.success = 0;
                    br.fail = 0;
                    br.results = nlohmann::json::array();

                    try {
                        auto response = binance_api->place_batch_orders(batch_orders);

                        if (response.is_array()) {
                            for (size_t k = 0; k < response.size(); ++k) {
                                const auto& res = response[k];
                                size_t orig_idx = start_idx + k;
                                std::string orig_symbol = orig_idx < total_orders ? orders_json[orig_idx].value("symbol", "") : "";
                                std::string orig_side = orig_idx < total_orders ? orders_json[orig_idx].value("side", "") : "";

                                if (res.contains("orderId")) {
                                    br.success++;
                                    std::string cli_oid = res.value("clientOrderId", "");
                                    std::string exch_oid = std::to_string(res.value("orderId", 0LL));
                                    LOG_ORDER_SRC(get_log_source(strategy_id), cli_oid, "ACCEPTED", "exchange_id=" + exch_oid + " symbol=" + orig_symbol
                                        + " filled=" + res.value("executedQty", "0") + "@" + res.value("avgPrice", "0"));
                                    // 透传币安原生字段名 —— 客户端 handle_batch_report 读的是
                                    // clientOrderId/orderId/origQty/executedQty/avgPrice/status
                                    // (旧版用 client_order_id/filled_quantity/avg_price/"accepted",
                                    //  与客户端读取的键名不一致, 导致策略侧成交价/量恒为 0)
                                    br.results.push_back({
                                        {"symbol", res.value("symbol", orig_symbol)},
                                        {"side", res.value("side", orig_side)},
                                        {"clientOrderId", cli_oid},
                                        {"orderId", res.value("orderId", 0LL)},
                                        {"origQty", res.value("origQty", "0")},
                                        {"executedQty", res.value("executedQty", "0")},
                                        {"avgPrice", res.value("avgPrice", "0")},
                                        {"status", res.value("status", "NEW")},
                                        {"error_msg", ""}
                                    });
                                } else if (res.contains("code")) {
                                    br.fail++;
                                    std::string err_msg = res.value("msg", "Unknown error");
                                    LOG_ORDER_SRC(get_log_source(strategy_id), "", "REJECTED", "symbol=" + orig_symbol + " side=" + orig_side + " reason=" + err_msg);
                                    br.results.push_back({
                                        {"symbol", orig_symbol},
                                        {"side", orig_side},
                                        {"code", res.value("code", 0)},
                                        {"msg", err_msg},
                                        {"clientOrderId", ""},
                                        {"status", "rejected"},
                                        {"error_msg", err_msg}
                                    });
                                }
                            }
                        }
                    } catch (const std::exception& e) {
                        Logger::instance().info(get_log_source(strategy_id), "[批量下单] ✗ Binance API异常: " + std::string(e.what()));
                        for (size_t k = 0; k < batch_count; ++k) {
                            size_t orig_idx = start_idx + k;
                            br.fail++;
                            br.results.push_back({
                                {"symbol", orig_idx < total_orders ? orders_json[orig_idx].value("symbol", "") : ""},
                                {"side", orig_idx < total_orders ? orders_json[orig_idx].value("side", "") : ""},
                                {"clientOrderId", orig_idx < total_orders ? orders_json[orig_idx].value("client_order_id", "") : ""},
                                {"status", "rejected"},
                                {"error_msg", std::string("异常: ") + e.what()}
                            });
                        }
                    }
                    return br;
                }
            ));
        }

        // 按原始顺序收集结果
        for (auto& fut : futures) {
            BatchResult br = fut.get();
            total_success += br.success;
            total_fail += br.fail;
            for (auto& r : br.results) {
                all_results.push_back(std::move(r));
            }
        }

        g_order_count += total_orders;
        g_order_success += total_success;
        g_order_failed += total_fail;

        Logger::instance().info(get_log_source(strategy_id), "[Binance批量下单] 成功: " + std::to_string(total_success) + " 失败: " + std::to_string(total_fail));

        // 批量下单有失败时发送邮件通知（汇总一封）
        if (total_fail > 0) {
            std::string acct = get_account_id(strategy_id);
            std::string acct_line = acct.empty() ? "" : ("账户: " + acct + "\n");
            std::string email_body = acct_line + "策略: " + strategy_id + "\n批次ID: " + batch_id +
                "\n成功: " + std::to_string(total_success) + " 笔, 失败: " + std::to_string(total_fail) + " 笔\n\n失败订单明细:\n";
            for (const auto& r : all_results) {
                if (r.value("status", "") == "rejected") {
                    email_body += "  " + r.value("symbol", "") + " " + r.value("side", "") +
                        " | " + r.value("error_msg", "") + "\n";
                }
            }
            g_risk_manager.send_risk_alert_to_strategy(strategy_id, email_body, "Binance批量下单失败");
        }

        nlohmann::json report = {
            {"type", "batch_report"}, {"strategy_id", strategy_id},
            {"batch_id", batch_id}, {"exchange", "binance"},
            {"status", total_fail == 0 ? "accepted" : (total_success > 0 ? "partial" : "rejected")},
            {"results", all_results}, {"success_count", total_success}, {"fail_count", total_fail},
            {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);
        return;
    }

    // OKX 批量下单（原有逻辑）
    okx::OKXRestAPI* api = get_api_for_strategy(strategy_id);
    if (!api) {
        Logger::instance().info(get_log_source(strategy_id), "[批量下单] ✗ 策略未注册OKX账户");
        nlohmann::json report = {
            {"type", "batch_report"}, {"strategy_id", strategy_id},
            {"batch_id", batch_id}, {"status", "rejected"},
            {"error_msg", "策略未注册OKX账户"}, {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);
        return;
    }

    std::vector<okx::PlaceOrderRequest> orders;
    for (const auto& ord : request["orders"]) {
        okx::PlaceOrderRequest req;
        req.inst_id = ord.value("symbol", "BTC-USDT-SWAP");
        req.td_mode = ord.value("td_mode", "cross");
        req.side = ord.value("side", "buy");
        req.ord_type = ord.value("order_type", "limit");
        req.sz = std::to_string(ord.value("quantity", 0.0));

        double px = ord.value("price", 0.0);
        if (px > 0) req.px = std::to_string(px);

        req.pos_side = ord.value("pos_side", "");
        req.cl_ord_id = ord.value("client_order_id", "");

        if (ord.contains("tag") && !ord["tag"].is_null()) {
            req.tag = ord["tag"].get<std::string>();
        }

        if (ord.contains("attach_algo_ords") && ord["attach_algo_ords"].is_array()) {
            for (const auto& algo_json : ord["attach_algo_ords"]) {
                okx::AttachAlgoOrder algo;
                if (algo_json.contains("tp_trigger_px") && !algo_json["tp_trigger_px"].is_null()) {
                    algo.tp_trigger_px = algo_json["tp_trigger_px"].get<std::string>();
                    algo.tp_ord_px = algo_json.value("tp_ord_px", "-1");
                    algo.tp_trigger_px_type = algo_json.value("tp_trigger_px_type", "last");
                }
                if (algo_json.contains("sl_trigger_px") && !algo_json["sl_trigger_px"].is_null()) {
                    algo.sl_trigger_px = algo_json["sl_trigger_px"].get<std::string>();
                    algo.sl_ord_px = algo_json.value("sl_ord_px", "-1");
                    algo.sl_trigger_px_type = algo_json.value("sl_trigger_px_type", "last");
                }
                if (!algo.tp_trigger_px.empty() || !algo.sl_trigger_px.empty()) {
                    req.attach_algo_ords.push_back(algo);
                }
            }
        }

        orders.push_back(req);
    }

    try {
        auto response = api->place_batch_orders(orders);

        int success_count = 0, fail_count = 0;
        nlohmann::json results = nlohmann::json::array();

        if (response.contains("data") && response["data"].is_array()) {
            for (const auto& data : response["data"]) {
                bool ok = data["sCode"] == "0";
                if (ok) success_count++; else fail_count++;

                std::string cli_oid = data.value("clOrdId", "");
                if (ok) {
                    LOG_ORDER_SRC(get_log_source(strategy_id), cli_oid, "ACCEPTED", "exchange_id=" + data.value("ordId", ""));
                } else {
                    LOG_ORDER_SRC(get_log_source(strategy_id), cli_oid, "REJECTED", "reason=" + data.value("sMsg", ""));
                }

                results.push_back({
                    {"client_order_id", data.value("clOrdId", "")},
                    {"exchange_order_id", data.value("ordId", "")},
                    {"status", ok ? "accepted" : "rejected"},
                    {"error_msg", data.value("sMsg", "")}
                });
            }
        }

        g_order_count += orders.size();
        g_order_success += success_count;
        g_order_failed += fail_count;

        Logger::instance().info(get_log_source(strategy_id), "[OKX批量下单] 成功: " + std::to_string(success_count) + " 失败: " + std::to_string(fail_count));

        // 批量下单有失败时发送邮件通知（汇总一封）
        if (fail_count > 0) {
            std::string acct = get_account_id(strategy_id);
            std::string acct_line = acct.empty() ? "" : ("账户: " + acct + "\n");
            std::string email_body = acct_line + "策略: " + strategy_id + "\n批次ID: " + batch_id +
                "\n成功: " + std::to_string(success_count) + " 笔, 失败: " + std::to_string(fail_count) + " 笔\n\n失败订单明细:\n";
            for (const auto& r : results) {
                if (r.value("status", "") == "rejected") {
                    email_body += "  " + r.value("client_order_id", "") +
                        " | " + r.value("error_msg", "") + "\n";
                }
            }
            g_risk_manager.send_risk_alert_to_strategy(strategy_id, email_body, "OKX批量下单失败");
        }

        nlohmann::json report = {
            {"type", "batch_report"}, {"strategy_id", strategy_id},
            {"batch_id", batch_id}, {"exchange", "okx"},
            {"status", fail_count == 0 ? "accepted" : (success_count > 0 ? "partial" : "rejected")},
            {"results", results}, {"success_count", success_count}, {"fail_count", fail_count},
            {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);

    } catch (const std::exception& e) {
        Logger::instance().info(get_log_source(strategy_id), "[批量下单] ✗ OKX API异常: " + std::string(e.what()));
        nlohmann::json report = {
            {"type", "batch_report"}, {"strategy_id", strategy_id},
            {"batch_id", batch_id}, {"status", "rejected"},
            {"error_msg", std::string("异常: ") + e.what()},
            {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);
    }
}

void process_cancel_order(ZmqServer& server, const nlohmann::json& request) {
    std::string strategy_id = request.value("strategy_id", "unknown");
    std::string symbol = request.value("symbol", "");
    std::string order_id = request.value("order_id", "");
    std::string client_order_id = request.value("client_order_id", "");

    std::string cancel_id = order_id.empty() ? client_order_id : order_id;
    LOG_ORDER_SRC(get_log_source(strategy_id), cancel_id, "CANCEL_REQUEST", "symbol=" + symbol);
    LOG_AUDIT_SRC(get_log_source(strategy_id), "ORDER_CANCEL", "order_id=" + cancel_id);

    Logger::instance().info(get_log_source(strategy_id), "[撤单] " + symbol + " | " + cancel_id);

    okx::OKXRestAPI* api = get_api_for_strategy(strategy_id);
    if (!api) {
        nlohmann::json report = {
            {"type", "cancel_report"}, {"strategy_id", strategy_id},
            {"order_id", order_id}, {"client_order_id", client_order_id},
            {"status", "rejected"}, {"error_msg", "策略未注册账户"},
            {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);
        return;
    }

    bool success = false;
    std::string error_msg;

    try {
        auto response = api->cancel_order(symbol, order_id, client_order_id);

        if (response["code"] == "0" && response.contains("data") && !response["data"].empty()) {
            auto& data = response["data"][0];
            if (data["sCode"] == "0") {
                success = true;
                LOG_ORDER_SRC(get_log_source(strategy_id), cancel_id, "CANCELLED", "success");
                Logger::instance().info(get_log_source(strategy_id), "[撤单] ✓ 成功");
            } else {
                error_msg = data.value("sMsg", "Unknown error");
                LOG_ORDER_SRC(get_log_source(strategy_id), cancel_id, "CANCEL_FAILED", "error=" + error_msg);
            }
        } else {
            error_msg = response.value("msg", "API error");
            LOG_ORDER_SRC(get_log_source(strategy_id), cancel_id, "CANCEL_FAILED", "error=" + error_msg);
        }
    } catch (const std::exception& e) {
        error_msg = std::string("异常: ") + e.what();
        LOG_ORDER_SRC(get_log_source(strategy_id), cancel_id, "CANCEL_ERROR", error_msg);
    }

    if (!success) Logger::instance().error(get_log_source(strategy_id), "[撤单] ✗ " + error_msg);

    nlohmann::json report = {
        {"type", "cancel_report"}, {"strategy_id", strategy_id},
        {"order_id", order_id}, {"client_order_id", client_order_id},
        {"status", success ? "cancelled" : "rejected"},
        {"error_msg", error_msg}, {"timestamp", current_timestamp_ms()}
    };
    server.publish_report(report);
}

void process_batch_cancel(ZmqServer& server, const nlohmann::json& request) {
    std::string strategy_id = request.value("strategy_id", "unknown");
    std::string symbol = request.value("symbol", "");

    okx::OKXRestAPI* api = get_api_for_strategy(strategy_id);
    if (!api) {
        nlohmann::json report = {
            {"type", "batch_cancel_report"}, {"strategy_id", strategy_id},
            {"status", "rejected"}, {"error_msg", "策略未注册账户"},
            {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);
        return;
    }

    std::vector<std::string> order_ids;
    if (request.contains("order_ids") && request["order_ids"].is_array()) {
        for (const auto& id : request["order_ids"]) {
            order_ids.push_back(id.get<std::string>());
        }
    }

    Logger::instance().info(get_log_source(strategy_id), "[批量撤单] " + symbol + " | " + std::to_string(order_ids.size()) + "个订单");

    try {
        auto response = api->cancel_batch_orders(order_ids, symbol);

        int success_count = 0, fail_count = 0;
        nlohmann::json results = nlohmann::json::array();

        if (response.contains("data") && response["data"].is_array()) {
            for (const auto& data : response["data"]) {
                bool ok = data["sCode"] == "0";
                if (ok) success_count++; else fail_count++;

                results.push_back({
                    {"order_id", data.value("ordId", "")},
                    {"status", ok ? "cancelled" : "rejected"},
                    {"error_msg", data.value("sMsg", "")}
                });
            }
        }

        Logger::instance().info(get_log_source(strategy_id), "[批量撤单] 成功: " + std::to_string(success_count) + " 失败: " + std::to_string(fail_count));

        nlohmann::json report = {
            {"type", "batch_cancel_report"}, {"strategy_id", strategy_id},
            {"symbol", symbol}, {"results", results},
            {"success_count", success_count}, {"fail_count", fail_count},
            {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);

    } catch (const std::exception& e) {
        nlohmann::json report = {
            {"type", "batch_cancel_report"}, {"strategy_id", strategy_id},
            {"status", "rejected"}, {"error_msg", std::string("异常: ") + e.what()},
            {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);
    }
}

void process_amend_order(ZmqServer& server, const nlohmann::json& request) {
    std::string strategy_id = request.value("strategy_id", "unknown");
    std::string symbol = request.value("symbol", "");
    std::string order_id = request.value("order_id", "");
    std::string client_order_id = request.value("client_order_id", "");
    std::string new_px = request.value("new_price", "");
    std::string new_sz = request.value("new_quantity", "");

    Logger::instance().info(get_log_source(strategy_id), "[修改订单] " + symbol);

    okx::OKXRestAPI* api = get_api_for_strategy(strategy_id);
    if (!api) {
        nlohmann::json report = {
            {"type", "amend_report"}, {"strategy_id", strategy_id},
            {"order_id", order_id}, {"client_order_id", client_order_id},
            {"status", "rejected"}, {"error_msg", "策略未注册账户"},
            {"timestamp", current_timestamp_ms()}
        };
        server.publish_report(report);
        return;
    }

    bool success = false;
    std::string error_msg;

    try {
        auto response = api->amend_order(symbol, order_id, client_order_id, new_sz, new_px);

        if (response["code"] == "0" && response.contains("data") && !response["data"].empty()) {
            auto& data = response["data"][0];
            if (data["sCode"] == "0") {
                success = true;
                Logger::instance().info(get_log_source(strategy_id), "[修改订单] ✓ 成功");
            } else {
                error_msg = data.value("sMsg", "Unknown error");
            }
        } else {
            error_msg = response.value("msg", "API error");
        }
    } catch (const std::exception& e) {
        error_msg = std::string("异常: ") + e.what();
    }

    if (!success) Logger::instance().error(get_log_source(strategy_id), "[修改订单] ✗ " + error_msg);

    nlohmann::json report = {
        {"type", "amend_report"}, {"strategy_id", strategy_id},
        {"order_id", order_id}, {"client_order_id", client_order_id},
        {"status", success ? "amended" : "rejected"},
        {"error_msg", error_msg}, {"timestamp", current_timestamp_ms()}
    };
    server.publish_report(report);
}

void process_register_account(ZmqServer& server, const nlohmann::json& request) {
    std::string strategy_id = request.value("strategy_id", "");
    std::string exchange = request.value("exchange", "okx");
    std::string api_key = request.value("api_key", "");
    std::string secret_key = request.value("secret_key", "");
    std::string passphrase = request.value("passphrase", "");
    bool is_testnet = request.value("is_testnet", true);

    LOG_AUDIT_SRC(get_log_source(strategy_id), "ACCOUNT_REGISTER", "exchange=" + exchange + " testnet=" + (is_testnet ? "true" : "false"));
    Logger::instance().info(get_log_source(strategy_id), "[账户注册] 策略: " + strategy_id + " | 交易所: " + exchange);
    Logger::instance().info(get_log_source(strategy_id), "[账户注册] DEBUG: API Key前8位: " + api_key.substr(0, 8) + "...");
    Logger::instance().info(get_log_source(strategy_id), "[账户注册] DEBUG: is_testnet: " + std::string(is_testnet ? "true" : "false"));

    nlohmann::json report;
    report["type"] = "register_report";
    report["strategy_id"] = strategy_id;
    report["exchange"] = exchange;
    report["timestamp"] = current_timestamp_ms();

    if (api_key.empty() || secret_key.empty()) {
        report["status"] = "rejected";
        report["error_msg"] = "缺少必要参数 (api_key, secret_key)";
        Logger::instance().info(get_log_source(strategy_id), "[账户注册] ✗ 参数不完整");
    } else {
        ExchangeType ex_type = string_to_exchange_type(exchange);
        Logger::instance().info(get_log_source(strategy_id), "[账户注册] DEBUG: ExchangeType = " + std::to_string((int)ex_type));

        bool success = false;

        // 优先使用 ZMQ 消息里显式传入的 account_id (新协议, Python 端可传)
        std::string account_id = request.value("account_id", "");
        std::string config_file;

        // 没传 account_id 时, 回退到从 config 文件反查 (旧协议, 向后兼容)
        if (account_id.empty() && !strategy_id.empty()) {
            std::filesystem::path exe_dir = std::filesystem::canonical("/proc/self/exe").parent_path();
            std::string config_dir = (exe_dir / trading::config::ConfigCenter::instance().server().strategy_config_dir).string();
            std::string strategy_cfg_dir = (exe_dir / ".." / "strategies" / "strategy_configs").string();

            // 遍历 strategy_configs/ 和 configs/ 两个目录
            std::vector<std::string> search_dirs = {strategy_cfg_dir, config_dir};
            for (const auto& dir : search_dirs) {
                if (!config_file.empty()) break;
                try {
                    if (std::filesystem::exists(dir)) {
                        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                            try {
                                std::ifstream tf(entry.path().string());
                                nlohmann::json tcfg;
                                tf >> tcfg;
                                tf.close();
                                if (tcfg.value("strategy_id", "") == strategy_id) {
                                    config_file = entry.path().string();
                                    break;
                                }
                            } catch (...) {}
                        }
                    }
                } catch (...) {}
            }

            if (config_file.empty()) {
                config_file = (exe_dir / trading::config::ConfigCenter::instance().server().strategy_config_dir / (strategy_id + ".json")).string();
            }

            // 读取 account_id
            try {
                std::ifstream ifs(config_file);
                if (ifs.is_open()) {
                    nlohmann::json cfg;
                    ifs >> cfg;
                    account_id = cfg.value("account_id", "");
                }
            } catch (...) {}
        } else if (!account_id.empty()) {
            Logger::instance().info(get_log_source(strategy_id),
                "[账户注册] DEBUG: 使用 ZMQ 显式传入的 account_id = " + account_id);
        }

        if (strategy_id.empty()) {
            Logger::instance().info(get_log_source(strategy_id), "[账户注册] DEBUG: strategy_id为空，注册为默认账户");
            if (ex_type == ExchangeType::OKX) {
                g_account_registry.set_default_okx_account(api_key, secret_key, passphrase, is_testnet);
                success = true;
            } else if (ex_type == ExchangeType::BINANCE) {
                g_account_registry.set_default_binance_account(api_key, secret_key, is_testnet);
                success = true;
            }
            Logger::instance().info(get_log_source(strategy_id), "[账户注册] ✓ 默认账户注册成功");
        } else if (!account_id.empty() && account_id != strategy_id &&
                   g_account_registry.is_registered(account_id, ex_type)) {
            // account_id 已在 registry 中（如启动时从磁盘加载），使用别名共享 API 实例
            success = g_account_registry.add_account_alias(strategy_id, account_id, ex_type);
            if (success) {
                Logger::instance().info(get_log_source(strategy_id),
                    "[账户注册] ✓ 策略 " + strategy_id + " 使用已有账户 " + account_id + " 的 API（别名模式）");
            } else {
                // 别名失败，回退到完整注册
                Logger::instance().info(get_log_source(strategy_id), "[账户注册] 别名失败，回退完整注册");
                success = g_account_registry.register_account(
                    strategy_id, ex_type, api_key, secret_key, passphrase, is_testnet
                );
            }
        } else if (!account_id.empty() && account_id != strategy_id) {
            // account_id 不空且 registry 里没有 → 先以 account_id 为主键注册账户, 再 alias strategy_id 到它
            // 这样多个 strategy_id 共享同一个 account 时, registry 里只有 1 个真实条目
            Logger::instance().info(get_log_source(strategy_id),
                "[账户注册] DEBUG: 以 account_id=" + account_id + " 创建新账户主条目, 再 alias strategy_id=" + strategy_id);
            success = g_account_registry.register_account(
                account_id, ex_type, api_key, secret_key, passphrase, is_testnet, account_id
            );
            if (success) {
                bool aliased = g_account_registry.add_account_alias(strategy_id, account_id, ex_type);
                if (aliased) {
                    Logger::instance().info(get_log_source(strategy_id),
                        "[账户注册] ✓ 新账户 " + account_id + " 注册成功, 策略 " + strategy_id + " 已 alias");
                } else {
                    Logger::instance().info(get_log_source(strategy_id),
                        "[账户注册] ⚠ 新账户 " + account_id + " 注册成功但 alias 失败");
                }
            } else {
                Logger::instance().info(get_log_source(strategy_id),
                    "[账户注册] ✗ 以 account_id 注册失败, 回退到按 strategy_id 注册");
                success = g_account_registry.register_account(
                    strategy_id, ex_type, api_key, secret_key, passphrase, is_testnet
                );
            }
        } else {
            Logger::instance().info(get_log_source(strategy_id), "[账户注册] DEBUG: 调用 g_account_registry.register_account()");
            success = g_account_registry.register_account(
                strategy_id, ex_type, api_key, secret_key, passphrase, is_testnet
            );
            if (success) {
                Logger::instance().info(get_log_source(strategy_id), "[账户注册] ✓ 策略 " + strategy_id + " 注册成功");
            } else {
                Logger::instance().info(get_log_source(strategy_id), "[账户注册] ✗ 策略 " + strategy_id + " 注册失败");
            }
        }

        if (success) {
            report["status"] = "registered";
            report["error_msg"] = "";

            // 加载邮箱配置
            if (!config_file.empty()) {
                Logger::instance().info(get_log_source(strategy_id), "[邮箱加载] 配置路径: " + config_file);
                g_risk_manager.load_strategy_email_from_config(config_file);
            }

            // 存入全局映射
            if (!account_id.empty()) {
                Logger::instance().info(get_log_source(strategy_id), "[账户注册] account_id: " + account_id);
                {
                    std::lock_guard<std::mutex> lock(g_sa_map_mutex);
                    g_strategy_account_map[strategy_id] = account_id;
                }
            }

            // 动态添加到账户监控器（如果该 account_id 已在监控中，跳过避免重复）
            bool already_monitored = false;
            if (g_account_monitor && !account_id.empty()) {
                // 检查 account_id 是否已作为独立账户被监控
                if (ex_type == ExchangeType::OKX) {
                    already_monitored = (g_account_registry.get_okx_api(account_id) != nullptr);
                } else if (ex_type == ExchangeType::BINANCE) {
                    already_monitored = (g_account_registry.get_binance_api(account_id) != nullptr);
                }
            }
            if (g_account_monitor && !strategy_id.empty() && !already_monitored) {
                if (ex_type == ExchangeType::OKX) {
                    auto* api = g_account_registry.get_okx_api(strategy_id);
                    if (api) {
                        trading::server::AccountCredentials credentials(api_key, secret_key, passphrase, is_testnet);
                        g_account_monitor->register_okx_account(strategy_id, api, &credentials, account_id);
                        Logger::instance().info(get_log_source(strategy_id), "[账户监控] ✓ 已添加到监控: " + strategy_id + " (account_id: " + account_id + ")");
                    }
                } else if (ex_type == ExchangeType::BINANCE) {
                    auto* api = g_account_registry.get_binance_api(strategy_id);
                    if (api) {
                        trading::server::AccountCredentials credentials(api_key, secret_key, "", is_testnet);
                        g_account_monitor->register_binance_account(strategy_id, api, &credentials, account_id);
                        Logger::instance().info(get_log_source(strategy_id), "[账户监控] ✓ 已添加到监控: " + strategy_id + " (account_id: " + account_id + ")");
                    }
                }
            } else if (already_monitored) {
                Logger::instance().info(get_log_source(strategy_id), "[账户��控] 跳过: 账户 " + account_id + " 已在监控中，策略 " + strategy_id + " 共享该账户");
            }
        } else {
            report["status"] = "rejected";
            report["error_msg"] = "注册失败";
        }
    }

    // 注册策略到进程管理器（用于前端策略列表和心跳检测）
    if (report.value("status", "") == "registered" && !strategy_id.empty()) {
        pid_t pid = static_cast<pid_t>(request.value("pid", 0));
        std::string acct_id = get_account_id(strategy_id);
        std::string start_cmd = request.value("start_command", "");
        std::string work_dir = request.value("work_dir", "");
        g_strategy_manager.register_strategy(strategy_id, pid, acct_id, exchange, start_cmd, work_dir);
        Logger::instance().info(get_log_source(strategy_id), "[策略进程] ✓ 已注册 PID=" + std::to_string(pid) + " account=" + acct_id + " cmd=" + start_cmd.substr(0, 80));
        if (pid <= 0) {
            Logger::instance().warn(get_log_source(strategy_id), "[策略进程] ⚠ PID无效，前端无法中止该策略进程（请确保strategy_base已重新编译）");
        }
    }

    Logger::instance().info(get_log_source(strategy_id), "[账户注册] DEBUG: 发送注册回报...");
    server.publish_report(report);
    Logger::instance().info(get_log_source(strategy_id), "[账户注册] DEBUG: 回报已发送");
}

void process_unregister_account(ZmqServer& server, const nlohmann::json& request) {
    std::string strategy_id = request.value("strategy_id", "");
    std::string exchange = request.value("exchange", "okx");

    Logger::instance().info(get_log_source(strategy_id), "[账户注销] 策略: " + strategy_id + " | 交易所: " + exchange);

    nlohmann::json report;
    report["type"] = "unregister_report";
    report["strategy_id"] = strategy_id;
    report["exchange"] = exchange;
    report["timestamp"] = current_timestamp_ms();

    if (strategy_id.empty()) {
        report["status"] = "rejected";
        report["error_msg"] = "缺少 strategy_id";
    } else {
        ExchangeType ex_type = string_to_exchange_type(exchange);
        bool success = g_account_registry.unregister_account(strategy_id, ex_type);

        // 同步从账户监控中移除
        if (success && g_account_monitor) {
            if (ex_type == ExchangeType::OKX) {
                g_account_monitor->unregister_okx_account(strategy_id);
                Logger::instance().info(get_log_source(strategy_id), "[账户监控] ✓ 已从监控中移除 OKX 账户: " + strategy_id);
            } else if (ex_type == ExchangeType::BINANCE) {
                g_account_monitor->unregister_binance_account(strategy_id);
                Logger::instance().info(get_log_source(strategy_id), "[账户监控] ✓ 已从监控中移除 Binance 账户: " + strategy_id);
            }
        }

        // 停止并删除该账户下的所有策略进程
        if (success) {
            std::string account_id = g_strategy_manager.get_account_id(strategy_id);
            if (!account_id.empty()) {
                auto removed = g_strategy_manager.stop_and_remove_by_account(account_id);
                for (const auto& sid : removed) {
                    Logger::instance().info(get_log_source(strategy_id),
                        "[账户注销] 联动停止策略: " + sid + " (account: " + account_id + ")");
                }
            }
        }

        report["status"] = success ? "unregistered" : "rejected";
        report["error_msg"] = success ? "" : "策略未找到";
    }

    server.publish_report(report);
}

void process_query_account(ZmqServer& server, const nlohmann::json& request) {
    std::string strategy_id = request.value("strategy_id", "");
    std::string exchange = request.value("exchange", "binance");  // 默认 Binance

    std::string exchange_lower = exchange;
    std::transform(exchange_lower.begin(), exchange_lower.end(),
                   exchange_lower.begin(), ::tolower);

    Logger::instance().info(get_log_source(strategy_id), "[账户查询] 策略: " + strategy_id + " | 交易所: " + exchange);
    Logger::instance().info(get_log_source(strategy_id), "[账户查询] DEBUG: exchange_lower = " + exchange_lower);

    nlohmann::json report;
    report["type"] = "account_update";
    report["strategy_id"] = strategy_id;
    report["exchange"] = exchange;
    report["timestamp"] = current_timestamp_ms();

    if (exchange_lower == "binance") {
        Logger::instance().info(get_log_source(strategy_id), "[账户查询] DEBUG: 进入Binance分支");
        Logger::instance().info(get_log_source(strategy_id), "[账户查询] DEBUG: 调用 get_binance_api_for_strategy(\"" + strategy_id + "\")");

        binance::BinanceRestAPI* api = get_binance_api_for_strategy(strategy_id);

        if (!api) {
            Logger::instance().info(get_log_source(strategy_id), "[账户查询] ✗ 策略未注册 Binance 账户");
            return;
        }

        try {
            // 调用 Binance REST API 获取账户信息
            auto account_info = api->get_account_info();

            // Binance 合约账户响应格式: { assets: [...], positions: [...], ... }
            if (account_info.contains("assets") && account_info["assets"].is_array()) {
                nlohmann::json details = nlohmann::json::array();

                for (const auto& asset : account_info["assets"]) {
                    std::string ccy = asset.value("asset", "");
                    std::string avail_bal = asset.value("availableBalance", "0");
                    std::string wallet_bal = asset.value("walletBalance", "0");

                    // 冻结金额 = 钱包余额 - 可用余额
                    double wallet = std::stod(wallet_bal);
                    double avail = std::stod(avail_bal);
                    double frozen = wallet - avail;
                    if (frozen < 0) frozen = 0;

                    // 只返回有余额的币种
                    if (wallet > 0 || avail > 0) {
                        details.push_back({
                            {"ccy", ccy},
                            {"availBal", avail_bal},
                            {"frozenBal", std::to_string(frozen)},
                            {"eq", wallet_bal},
                            {"eqUsd", wallet_bal}  // Binance 已经是 USD 计价
                        });
                    }
                }

                // 构造 OKX 格式的账户更新消息（便于策略端统一解析）
                report["data"] = {
                    {"totalEq", account_info.value("totalWalletBalance", "0")},
                    {"mgnRatio", "0"},
                    {"details", details}
                };

                Logger::instance().info(get_log_source(strategy_id), "[账户查询] ✓ Binance 余额查询成功，币种数: " + std::to_string(details.size()));
            } else {
                Logger::instance().info(get_log_source(strategy_id), "[账户查询] ✗ Binance 响应格式异常");
                return;
            }
        } catch (const std::exception& e) {
            Logger::instance().info(get_log_source(strategy_id), "[账户查询] ✗ 异常: " + std::string(e.what()));
            return;
        }
    } else {
        // OKX 账户查询
        okx::OKXRestAPI* api = get_okx_api_for_strategy(strategy_id);
        if (!api) {
            Logger::instance().info(get_log_source(strategy_id), "[账户查询] ✗ 策略未注册 OKX 账户");
            return;
        }

        try {
            auto account_info = api->get_account_balance();

            if (account_info.contains("data") && account_info["data"].is_array() && !account_info["data"].empty()) {
                report["data"] = account_info["data"][0];
                Logger::instance().info(get_log_source(strategy_id), "[账户查询] ✓ OKX 余额查询成功");
            } else {
                Logger::instance().info(get_log_source(strategy_id), "[账户查询] ✗ OKX 响应格式异常");
                return;
            }
        } catch (const std::exception& e) {
            Logger::instance().info(get_log_source(strategy_id), "[账户查询] ✗ 异常: " + std::string(e.what()));
            return;
        }
    }

    Logger::instance().info(get_log_source(strategy_id), "[账户查询] DEBUG: 调用 server.publish_report()...");
    server.publish_report(report);
    Logger::instance().info(get_log_source(strategy_id), "[账户查询] DEBUG: 回报已发送");
}

void process_query_positions(ZmqServer& server, const nlohmann::json& request) {
    std::string strategy_id = request.value("strategy_id", "");
    std::string exchange = request.value("exchange", "binance");  // 默认 Binance

    std::string exchange_lower = exchange;
    std::transform(exchange_lower.begin(), exchange_lower.end(),
                   exchange_lower.begin(), ::tolower);

    Logger::instance().info(get_log_source(strategy_id), "[持仓查询] 策略: " + strategy_id + " | 交易所: " + exchange);

    nlohmann::json report;
    report["type"] = "position_update";
    report["strategy_id"] = strategy_id;
    report["exchange"] = exchange;
    report["timestamp"] = current_timestamp_ms();

    if (exchange_lower == "binance") {
        binance::BinanceRestAPI* api = get_binance_api_for_strategy(strategy_id);
        if (!api) {
            Logger::instance().info(get_log_source(strategy_id), "[持仓查询] ✗ 策略未注册 Binance 账户");
            return;
        }

        // 辅助lambda：解析Binance持仓结果
        auto parse_binance_positions = [&](const nlohmann::json& positions) -> nlohmann::json {
            nlohmann::json pos_data = nlohmann::json::array();
            if (positions.is_array()) {
                for (const auto& pos : positions) {
                    double pos_amt = std::stod(pos.value("positionAmt", "0"));
                    if (pos_amt != 0) {
                        pos_data.push_back({
                            {"instId", pos.value("symbol", "")},
                            {"posSide", pos.value("positionSide", "BOTH")},
                            {"pos", pos.value("positionAmt", "0")},
                            {"avgPx", pos.value("entryPrice", "0")},
                            {"markPx", pos.value("markPrice", "0")},
                            {"upl", pos.value("unrealizedProfit", "0")},
                            {"lever", pos.value("leverage", "1")},
                            {"liqPx", pos.value("liquidationPrice", "0")}
                        });
                    }
                }
            }
            return pos_data;
        };

        // 辅助lambda：查询一次持仓（含sync_time重试）
        std::function<std::pair<bool, nlohmann::json>(bool)> query_positions_once =
            [&](bool do_sync_first) -> std::pair<bool, nlohmann::json> {
            try {
                if (do_sync_first) {
                    api->sync_server_time();
                }
                auto positions = api->get_positions();
                return {true, parse_binance_positions(positions)};
            } catch (const std::exception& e) {
                std::string err_msg = e.what();
                Logger::instance().error(get_log_source(strategy_id), "[持仓查询] ✗ 异常: " + err_msg);
                // -1021 ��间戳异常，重新同步后重试
                if (!do_sync_first && err_msg.find("-1021") != std::string::npos) {
                    Logger::instance().info(get_log_source(strategy_id), "[持仓查询] 时间戳异常，重新同步服务器时间后重试...");
                    return query_positions_once(true);
                }
                return {false, nlohmann::json::array()};
            }
        };

        auto [ok, pos_data] = query_positions_once(false);

        if (!ok) {
            // 查询彻底失败，发送错误回报
            report["error"] = true;
            report["error_msg"] = "持仓查询异常";
            report["data"] = nlohmann::json::array();
            server.publish_report(report);
            return;
        }

        // 查询成功但返回0持仓时，与账户监控缓存比对
        if (pos_data.empty()) {
            std::string acct_id = get_account_id(strategy_id);
            if (acct_id.empty()) acct_id = strategy_id;
            nlohmann::json cached = g_account_registry.get_account_positions(acct_id);
            if (!cached.empty() && cached.is_array() && cached.size() > 0) {
                Logger::instance().warn(get_log_source(strategy_id),
                    "[持仓查询] 查询返回0持仓，但账户监控缓存有 " + std::to_string(cached.size()) +
                    " 个持仓，可能查询异常，尝试重试...");
                // 重新同步时间后重试最多2次
                for (int retry = 0; retry < 2; retry++) {
                    auto [ok2, pos_data2] = query_positions_once(true);
                    if (ok2 && !pos_data2.empty()) {
                        pos_data = pos_data2;
                        Logger::instance().info(get_log_source(strategy_id),
                            "[持仓查询] ✓ 重试成功，获取到 " + std::to_string(pos_data.size()) + " 个持仓");
                        break;
                    }
                    Logger::instance().warn(get_log_source(strategy_id),
                        "[持仓查询] 重试第" + std::to_string(retry + 1) + "次仍返回0，继续...");
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                // 重试仍然为0，使用缓存持仓作为兜底
                if (pos_data.empty()) {
                    Logger::instance().warn(get_log_source(strategy_id),
                        "[持仓查询] 重试后仍为0，使用账户监控缓存持仓 (" + std::to_string(cached.size()) + " 个) 作为兜底");
                    // 将缓存的Binance格式转为统一格式
                    for (const auto& pos : cached) {
                        double notional = std::stod(pos.value("notional", "0"));
                        if (std::abs(notional) > 0.01) {
                            pos_data.push_back({
                                {"instId", pos.value("symbol", "")},
                                {"posSide", pos.value("positionSide", "BOTH")},
                                {"pos", pos.value("positionAmt", "0")},
                                {"avgPx", pos.value("entryPrice", "0")},
                                {"markPx", pos.value("markPrice", "0")},
                                {"upl", pos.value("unrealizedProfit", "0")},
                                {"lever", pos.value("leverage", "1")},
                                {"liqPx", pos.value("liquidationPrice", "0")}
                            });
                        }
                    }
                }
            }
        }

        report["data"] = pos_data;
        Logger::instance().info(get_log_source(strategy_id), "[持仓查询] ✓ Binance 持仓查询成功 (" + std::to_string(pos_data.size()) + " 个)");
    } else {
        // OKX 持仓查询
        okx::OKXRestAPI* api = get_okx_api_for_strategy(strategy_id);
        if (!api) {
            Logger::instance().info(get_log_source(strategy_id), "[持仓查询] ✗ 策略未注册 OKX 账户");
            return;
        }

        try {
            auto positions = api->get_positions();

            if (positions.contains("data") && positions["data"].is_array()) {
                report["data"] = positions["data"];
                Logger::instance().info(get_log_source(strategy_id), "[持仓查询] ✓ OKX 持仓查询成功");
            } else {
                Logger::instance().error(get_log_source(strategy_id), "[持仓查询] ✗ OKX 响应格式异常");
                report["error"] = true;
                report["error_msg"] = "OKX 响应格式异常";
                report["data"] = nlohmann::json::array();
                server.publish_report(report);
                return;
            }
        } catch (const std::exception& e) {
            Logger::instance().error(get_log_source(strategy_id), "[持仓查询] ✗ 异常: " + std::string(e.what()));
            report["error"] = true;
            report["error_msg"] = std::string(e.what());
            report["data"] = nlohmann::json::array();
            server.publish_report(report);
            return;
        }
    }

    server.publish_report(report);
}

void process_change_leverage(ZmqServer& server, const nlohmann::json& request) {
    std::string strategy_id = request.value("strategy_id", "");
    std::string exchange = request.value("exchange", "binance");
    std::string symbol = request.value("symbol", "");
    int leverage = request.value("leverage", 1);

    std::string exchange_lower = exchange;
    std::transform(exchange_lower.begin(), exchange_lower.end(),
                   exchange_lower.begin(), ::tolower);

    Logger::instance().info(get_log_source(strategy_id), "[杠杆调整] 交易所: " + exchange + " | 交易对: " + symbol + " | 杠杆: " + std::to_string(leverage) + "x");

    nlohmann::json report;
    report["type"] = "leverage_report";
    report["strategy_id"] = strategy_id;
    report["exchange"] = exchange;
    report["symbol"] = symbol;
    report["leverage"] = leverage;
    report["timestamp"] = current_timestamp_ms();

    if (exchange_lower == "binance") {
        binance::BinanceRestAPI* api = get_binance_api_for_strategy(strategy_id);
        if (!api) {
            report["status"] = "rejected";
            report["error_msg"] = "策略未注册 Binance 账户";
            Logger::instance().error(get_log_source(strategy_id), "[杠杆调整] ✗ 策略未注册 Binance 账户");
            server.publish_report(report);
            return;
        }

        try {
            auto response = api->change_leverage(symbol, leverage);

            // Binance 成功响应: {"leverage": 1, "maxNotionalValue": "...", "symbol": "BTCUSDT"}
            if (response.contains("leverage")) {
                int actual_leverage = response["leverage"].get<int>();
                report["status"] = "success";
                report["actual_leverage"] = actual_leverage;
                report["max_notional_value"] = response.value("maxNotionalValue", "");
                Logger::instance().info(get_log_source(strategy_id), "[杠杆调整] ✓ Binance " + symbol + " 杠杆已设置为 " + std::to_string(actual_leverage) + "x");
            } else {
                report["status"] = "rejected";
                report["error_msg"] = response.value("msg", "未知错误");
                Logger::instance().error(get_log_source(strategy_id), "[杠杆调整] ✗ Binance 响应异常: " + response.dump());
            }
        } catch (const std::exception& e) {
            report["status"] = "rejected";
            report["error_msg"] = std::string("异常: ") + e.what();
            Logger::instance().error(get_log_source(strategy_id), "[杠杆调整] ✗ 异常: " + std::string(e.what()));
        }
    } else {
        // OKX 暂不支持（OKX 杠杆通过账户设置）
        report["status"] = "rejected";
        report["error_msg"] = "OKX 杠杆调整暂不支持，请通过账户设置";
        Logger::instance().error(get_log_source(strategy_id), "[杠杆调整] ✗ OKX 杠杆调整暂不支持");
    }

    server.publish_report(report);
}

void process_order_request(ZmqServer& server, const nlohmann::json& request) {
    std::string type = request.value("type", "order_request");

    if (type == "order_request") {
        process_place_order(server, request);
    } else if (type == "batch_order_request") {
        process_batch_orders(server, request);
    } else if (type == "cancel_request") {
        process_cancel_order(server, request);
    } else if (type == "batch_cancel_request") {
        process_batch_cancel(server, request);
    } else if (type == "amend_request") {
        process_amend_order(server, request);
    } else if (type == "register_account") {
        process_register_account(server, request);
    } else if (type == "unregister_account") {
        process_unregister_account(server, request);
    } else if (type == "query_account") {
        process_query_account(server, request);
    } else if (type == "query_positions") {
        process_query_positions(server, request);
    } else if (type == "change_leverage") {
        process_change_leverage(server, request);
    } else if (type == "heartbeat") {
        // 策略心跳 - 轻量处理，不打日志避免刷屏
        std::string sid = request.value("strategy_id", "");
        if (!sid.empty()) {
            g_strategy_manager.record_heartbeat(sid);
        }
    } else {
        Logger::instance().warn("system", "[订单] 未知请求类型: " + type);
    }
}

void print_risk_config() {
    auto limits = g_risk_manager.get_limits();
    std::string risk_info = "[风控配置] "
        "单笔最大金额=" + std::to_string(limits.max_order_value) + "U"
        " | 单品种最大持仓=" + std::to_string(limits.max_position_value) + "U"
        " | 总敞口=" + std::to_string(limits.max_total_exposure) + "U"
        " | 每日亏损限制=" + std::to_string(limits.daily_loss_limit) + "U"
        " | 每分钟最大订单=" + std::to_string(limits.max_orders_per_minute);
    Logger::instance().info("system", risk_info);
}

} // namespace server
} // namespace trading
