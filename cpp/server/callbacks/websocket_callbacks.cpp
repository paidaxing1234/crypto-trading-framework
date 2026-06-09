/**
 * @file websocket_callbacks.cpp
 * @brief WebSocket 回调设置模块实现
 */

#include "websocket_callbacks.h"
#include "../config/server_config.h"
#include "../managers/redis_recorder.h"
#include "../../adapters/okx/okx_websocket.h"
#include "../../adapters/binance/binance_websocket.h"
#include "../../network/websocket_server.h"
#include <functional>

using namespace trading::okx;

namespace trading {
namespace server {

// 辅助函数：去掉 -SWAP 后缀用于前端显示
static std::string strip_swap_suffix(const std::string& symbol) {
    const std::string suffix = "-SWAP";
    if (symbol.size() > suffix.size() &&
        symbol.compare(symbol.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return symbol.substr(0, symbol.size() - suffix.size());
    }
    return symbol;
}

// 辅助函数：从 JSON 值中安全获取 double（支持字符串和数字类型）
static double json_to_double(const nlohmann::json& val, double default_val = 0.0) {
    if (val.is_string()) {
        const std::string& s = val.get<std::string>();
        if (s.empty()) return default_val;
        try {
            return std::stod(s);
        } catch (...) {
            return default_val;
        }
    } else if (val.is_number()) {
        return val.get<double>();
    }
    return default_val;
}

// 辅助函数：从 JSON 值中安全获取 int64_t（支持字符串和数字类型）
static int64_t json_to_int64(const nlohmann::json& val, int64_t default_val = 0) {
    if (val.is_string()) {
        const std::string& s = val.get<std::string>();
        if (s.empty()) return default_val;
        try {
            return std::stoll(s);
        } catch (...) {
            return default_val;
        }
    } else if (val.is_number()) {
        return val.get<int64_t>();
    }
    return default_val;
}

// 辅助函数：从 JSON 值中安全获取字符串（支持字符串和数字类型）
static std::string json_to_string(const nlohmann::json& val, const std::string& default_val = "") {
    if (val.is_string()) {
        return val.get<std::string>();
    } else if (val.is_number_integer()) {
        return std::to_string(val.get<int64_t>());
    } else if (val.is_number_float()) {
        return std::to_string(val.get<double>());
    } else if (val.is_number()) {
        // 通用数字类型处理
        return val.dump();
    }
    return default_val;
}

void setup_websocket_callbacks(ZmqServer& zmq_server) {
    // Trades 回调（公共频道）
    if (g_ws_public) {
        // OKX Ticker 回调（原始JSON格式）
        g_ws_public->set_ticker_callback([&zmq_server](const nlohmann::json& raw) {
            g_okx_ticker_count++;

            std::string symbol = "";
            if (raw.contains("instId")) {
                symbol = json_to_string(raw["instId"]);
            }
            std::string display_symbol = strip_swap_suffix(symbol);

            nlohmann::json msg = {
                {"type", "ticker"},
                {"exchange", "okx"},
                {"symbol", display_symbol},
                {"timestamp_ns", current_timestamp_ns()}
            };

            // 从原始数据中提取字段（支持字符串和数字类型）
            if (raw.contains("last")) msg["price"] = json_to_double(raw["last"]);
            if (raw.contains("ts")) msg["timestamp"] = json_to_int64(raw["ts"]);
            if (raw.contains("high24h")) msg["high_24h"] = json_to_double(raw["high24h"]);
            if (raw.contains("low24h")) msg["low_24h"] = json_to_double(raw["low24h"]);
            if (raw.contains("open24h")) msg["open_24h"] = json_to_double(raw["open24h"]);
            if (raw.contains("vol24h")) msg["volume_24h"] = json_to_double(raw["vol24h"]);

            // 发布到 OKX 专用通道
            zmq_server.publish_okx_market(msg, MessageType::TICKER);
            // 同时发布到统一通道（兼容旧客户端）
            zmq_server.publish_ticker(msg);

            if (g_frontend_server) {
                g_frontend_server->send_event("ticker", msg);
            }
        });

        // OKX Trade 回调（原始JSON格式）
        g_ws_public->set_trade_callback([&zmq_server](const nlohmann::json& raw) {
            g_trade_count++;
            g_okx_trade_count++;

            std::string symbol = "";
            if (raw.contains("symbol")) {
                symbol = json_to_string(raw["symbol"]);
            } else if (raw.contains("instId")) {
                symbol = json_to_string(raw["instId"]);
            }

            nlohmann::json msg = {
                {"type", "trade"},
                {"exchange", "okx"},
                {"symbol", symbol},
                {"timestamp_ns", current_timestamp_ns()}
            };

            if (raw.contains("tradeId")) msg["trade_id"] = json_to_string(raw["tradeId"]);
            if (raw.contains("px")) msg["price"] = json_to_double(raw["px"]);
            if (raw.contains("sz")) msg["quantity"] = json_to_double(raw["sz"]);
            if (raw.contains("side")) msg["side"] = json_to_string(raw["side"]);
            if (raw.contains("ts")) msg["timestamp"] = json_to_int64(raw["ts"]);

            // 发布到 OKX 专用通道
            zmq_server.publish_okx_market(msg, MessageType::TRADE);
            // 同时发布到统一通道
            zmq_server.publish_ticker(msg);

            // Redis 录制 Trade 数据
            if (g_redis_recorder && g_redis_recorder->is_running()) {
                g_redis_recorder->record_trade(symbol, "okx", msg);
            }

            // 转发给前端 WebSocket（每10条发送一次，避免过多数据）
            static int trade_counter = 0;
            if (++trade_counter % 10 == 0 && g_frontend_server) {
                g_frontend_server->send_event("trade", msg);
            }
        });

        // OKX 深度数据回调（原始JSON格式）- 注意：目前OKX没有订阅深度
        g_ws_public->set_orderbook_callback([&zmq_server](const nlohmann::json& raw) {
            g_orderbook_count++;

            std::string symbol = "";
            if (raw.contains("symbol")) {
                symbol = json_to_string(raw["symbol"]);
            }
            std::string channel = "";
            if (raw.contains("channel")) {
                channel = json_to_string(raw["channel"]);
            } else {
                channel = "books5";
            }
            std::string action = "";
            if (raw.contains("action")) {
                action = json_to_string(raw["action"]);
            } else {
                action = "snapshot";
            }

            nlohmann::json bids = nlohmann::json::array();
            nlohmann::json asks = nlohmann::json::array();

            if (raw.contains("bids") && raw["bids"].is_array()) {
                for (const auto& bid : raw["bids"]) {
                    if (bid.is_array() && bid.size() >= 2) {
                        double price = json_to_double(bid[0]);
                        double size = json_to_double(bid[1]);
                        bids.push_back({price, size});
                    }
                }
            }

            if (raw.contains("asks") && raw["asks"].is_array()) {
                for (const auto& ask : raw["asks"]) {
                    if (ask.is_array() && ask.size() >= 2) {
                        double price = json_to_double(ask[0]);
                        double size = json_to_double(ask[1]);
                        asks.push_back({price, size});
                    }
                }
            }

            nlohmann::json msg = {
                {"type", "orderbook"},
                {"exchange", "okx"},
                {"symbol", symbol},
                {"channel", channel},
                {"action", action},
                {"bids", bids},
                {"asks", asks},
                {"timestamp_ns", current_timestamp_ns()}
            };

            if (raw.contains("ts")) msg["timestamp"] = json_to_int64(raw["ts"]);

            // 计算最优价格
            if (!bids.empty()) {
                msg["best_bid_price"] = bids[0][0];
                msg["best_bid_size"] = bids[0][1];
            }
            if (!asks.empty()) {
                msg["best_ask_price"] = asks[0][0];
                msg["best_ask_size"] = asks[0][1];
            }
            if (!bids.empty() && !asks.empty()) {
                double best_bid = bids[0][0].get<double>();
                double best_ask = asks[0][0].get<double>();
                msg["mid_price"] = (best_bid + best_ask) / 2.0;
                msg["spread"] = best_ask - best_bid;
            }

            // 发布到 OKX 专用通道
            zmq_server.publish_okx_market(msg, MessageType::DEPTH);
            // 同时发布到统一通道
            zmq_server.publish_depth(msg);

            // Redis 录制 Orderbook 数据
            if (g_redis_recorder && g_redis_recorder->is_running()) {
                g_redis_recorder->record_orderbook(symbol, "okx", msg);
            }
        });

        // OKX 资金费率回调（原始JSON格式）
        g_ws_public->set_funding_rate_callback([&zmq_server](const nlohmann::json& raw) {
            g_funding_rate_count++;

            std::string inst_id = "";
            if (raw.contains("instId")) {
                inst_id = json_to_string(raw["instId"]);
            }
            std::string inst_type = "";
            if (raw.contains("instType")) {
                inst_type = json_to_string(raw["instType"]);
            }

            nlohmann::json msg = {
                {"type", "funding_rate"},
                {"exchange", "okx"},
                {"symbol", inst_id},
                {"inst_type", inst_type},
                {"timestamp_ns", current_timestamp_ns()}
            };

            // 从原始数据中提取字段（OKX资金费率字段）- 支持字符串和数字类型
            if (raw.contains("fundingRate")) msg["funding_rate"] = json_to_double(raw["fundingRate"]);
            if (raw.contains("nextFundingRate")) msg["next_funding_rate"] = json_to_double(raw["nextFundingRate"]);
            if (raw.contains("fundingTime")) msg["funding_time"] = json_to_int64(raw["fundingTime"]);
            if (raw.contains("nextFundingTime")) msg["next_funding_time"] = json_to_int64(raw["nextFundingTime"]);
            if (raw.contains("minFundingRate")) msg["min_funding_rate"] = json_to_double(raw["minFundingRate"]);
            if (raw.contains("maxFundingRate")) msg["max_funding_rate"] = json_to_double(raw["maxFundingRate"]);
            if (raw.contains("interestRate")) msg["interest_rate"] = json_to_double(raw["interestRate"]);
            if (raw.contains("impactValue")) msg["impact_value"] = json_to_double(raw["impactValue"]);
            if (raw.contains("premium")) msg["premium"] = json_to_double(raw["premium"]);
            if (raw.contains("settState")) msg["sett_state"] = json_to_string(raw["settState"]);
            if (raw.contains("settFundingRate")) msg["sett_funding_rate"] = json_to_double(raw["settFundingRate"]);
            if (raw.contains("method")) msg["method"] = json_to_string(raw["method"]);
            if (raw.contains("formulaType")) msg["formula_type"] = json_to_string(raw["formulaType"]);
            if (raw.contains("ts")) msg["timestamp"] = json_to_int64(raw["ts"]);

            // 发布到 OKX 专用通道
            zmq_server.publish_okx_market(msg, MessageType::TICKER);
            // 同时发布到统一通道
            zmq_server.publish_ticker(msg);

            // Redis 录制 Funding Rate 数据
            if (g_redis_recorder && g_redis_recorder->is_running()) {
                g_redis_recorder->record_funding_rate(inst_id, "okx", msg);
            }
        });
    }

    // OKX K线回调（原始JSON格式）
    if (g_ws_business) {
        g_ws_business->set_kline_callback([&zmq_server](const nlohmann::json& raw) {
            // 检查是否为已确认的K线（confirm字段）- 支持字符串和数字类型
            // OKX K线: confirm=0 表示未完结（实时更新），confirm=1 表示已完结
            // 只发布已完结的K线（confirm=1）
            if (raw.contains("confirm")) {
                bool is_confirmed = false;
                if (raw["confirm"].is_number()) {
                    is_confirmed = (raw["confirm"].get<int>() == 1);
                } else if (raw["confirm"].is_string()) {
                    std::string confirm_str = raw["confirm"].get<std::string>();
                    is_confirmed = (confirm_str == "1");
                }
                if (!is_confirmed) {
                    return;  // 跳过未完结的K线
                }
            }

            g_kline_count++;
            g_okx_kline_count++;

            std::string symbol = "";
            if (raw.contains("symbol")) {
                symbol = json_to_string(raw["symbol"]);
            }
            std::string interval = "";
            if (raw.contains("interval")) {
                interval = json_to_string(raw["interval"]);
            }

            nlohmann::json msg = {
                {"type", "kline"},
                {"exchange", "okx"},
                {"symbol", symbol},
                {"interval", interval},
                {"timestamp_ns", current_timestamp_ns()}
            };

            // 从原始数据中提取字段（支持字符串和数字类型）
            if (raw.contains("o")) msg["open"] = json_to_double(raw["o"]);
            if (raw.contains("h")) msg["high"] = json_to_double(raw["h"]);
            if (raw.contains("l")) msg["low"] = json_to_double(raw["l"]);
            if (raw.contains("c")) msg["close"] = json_to_double(raw["c"]);
            if (raw.contains("vol")) msg["volume"] = json_to_double(raw["vol"]);
            if (raw.contains("ts")) msg["timestamp"] = json_to_int64(raw["ts"]);

            // 发布到 OKX 专用通道
            zmq_server.publish_okx_market(msg, MessageType::KLINE);
            // 同时发布到统一通道
            zmq_server.publish_kline(msg);

            // Redis 录制 K线 数据
            if (g_redis_recorder && g_redis_recorder->is_running()) {
                g_redis_recorder->record_kline(symbol, interval, "okx", msg);
            }
        });
    }

    // 订单推送回调（私有频道）
    if (g_ws_private) {
        g_ws_private->set_order_callback([&zmq_server](const Order::Ptr& order) {
            nlohmann::json msg = {
                {"type", "order_update"},
                {"exchange", "okx"},
                {"symbol", order->symbol()},
                {"exchange_order_id", order->exchange_order_id()},
                {"client_order_id", order->client_order_id()},
                {"side", order->side() == OrderSide::BUY ? "buy" : "sell"},
                {"order_type", order->order_type() == OrderType::MARKET ? "market" : "limit"},
                {"price", order->price()},
                {"quantity", order->quantity()},
                {"filled_quantity", order->filled_quantity()},
                {"filled_price", order->filled_price()},
                {"status", order_state_to_string(order->state())},
                {"timestamp", current_timestamp_ms()},
                {"timestamp_ns", current_timestamp_ns()}
            };

            zmq_server.publish_report(msg);

            // 发送到前端 WebSocket（实盘订单更新）
            if (g_frontend_server) {
                g_frontend_server->send_event("order_update", msg);
            }
        });

        // 账户更新回调
        g_ws_private->set_account_callback([&zmq_server](const nlohmann::json& acc) {
            nlohmann::json msg = {
                {"type", "account_update"},
                {"exchange", "okx"},
                {"data", acc},
                {"timestamp", current_timestamp_ms()}
            };
            zmq_server.publish_report(msg);

            // 发送到前端 WebSocket（实盘账户更新）
            if (g_frontend_server) {
                g_frontend_server->send_event("account_update", msg);
            }
        });

        // 持仓更新回调
        g_ws_private->set_position_callback([&zmq_server](const nlohmann::json& pos) {
            nlohmann::json msg = {
                {"type", "position_update"},
                {"exchange", "okx"},
                {"data", pos},
                {"timestamp", current_timestamp_ms()}
            };
            zmq_server.publish_report(msg);

            // 发送到前端 WebSocket（实盘持仓更新）
            if (g_frontend_server) {
                g_frontend_server->send_event("position_update", msg);
            }
        });
    }
}

void setup_binance_websocket_callbacks(ZmqServer& zmq_server) {
    // Binance 回调（原始JSON格式）
    if (g_binance_ws_market) {
        // Binance Ticker 回调（原始JSON格式）- !ticker@arr
        g_binance_ws_market->set_ticker_callback([&zmq_server](const nlohmann::json& raw) {
            g_binance_ticker_count++;

            // Binance ticker 字段: s(symbol), c(close/last), h(high), l(low), o(open), v(volume), E(event time)
            std::string symbol = "";
            if (raw.contains("s")) {
                symbol = json_to_string(raw["s"]);
            }

            nlohmann::json msg = {
                {"type", "ticker"},
                {"exchange", "binance"},
                {"symbol", symbol},
                {"timestamp_ns", current_timestamp_ns()}
            };

            if (raw.contains("c")) msg["price"] = json_to_double(raw["c"]);
            if (raw.contains("E")) msg["timestamp"] = json_to_int64(raw["E"]);
            if (raw.contains("h")) msg["high_24h"] = json_to_double(raw["h"]);
            if (raw.contains("l")) msg["low_24h"] = json_to_double(raw["l"]);
            if (raw.contains("o")) msg["open_24h"] = json_to_double(raw["o"]);
            if (raw.contains("v")) msg["volume_24h"] = json_to_double(raw["v"]);

            // 发布到 Binance 专用通道
            zmq_server.publish_binance_market(msg, MessageType::TICKER);
            // 同时发布到统一通道
            zmq_server.publish_ticker(msg);

            if (g_frontend_server) {
                g_frontend_server->send_event("ticker", msg);
            }
        });

        // Binance Trade 回调（原始JSON格式）- 注意：目前Binance没有订阅trade
        g_binance_ws_market->set_trade_callback([&zmq_server](const nlohmann::json& raw) {
            g_trade_count++;

            // Binance trade 字段: s(symbol), t(trade id), p(price), q(quantity), m(is buyer maker), T(trade time)
            std::string symbol = "";
            if (raw.contains("s")) {
                symbol = json_to_string(raw["s"]);
            }

            nlohmann::json msg = {
                {"type", "trade"},
                {"exchange", "binance"},
                {"symbol", symbol},
                {"timestamp_ns", current_timestamp_ns()}
            };

            if (raw.contains("t")) msg["trade_id"] = std::to_string(json_to_int64(raw["t"]));
            if (raw.contains("p")) msg["price"] = json_to_double(raw["p"]);
            if (raw.contains("q")) msg["quantity"] = json_to_double(raw["q"]);
            if (raw.contains("m")) {
                if (raw["m"].is_boolean()) {
                    msg["side"] = raw["m"].get<bool>() ? "sell" : "buy";
                } else {
                    msg["side"] = json_to_string(raw["m"]) == "true" ? "sell" : "buy";
                }
            }
            if (raw.contains("T")) msg["timestamp"] = json_to_int64(raw["T"]);

            // 发布到 Binance 专用通道
            zmq_server.publish_binance_market(msg, MessageType::TRADE);
            // 同时发布到统一通道
            zmq_server.publish_ticker(msg);

            // Redis 录制 Trade 数据
            if (g_redis_recorder && g_redis_recorder->is_running()) {
                g_redis_recorder->record_trade(symbol, "binance", msg);
            }

            static int binance_trade_counter = 0;
            if (++binance_trade_counter % 10 == 0 && g_frontend_server) {
                g_frontend_server->send_event("trade", msg);
            }
        });

        // Binance K线回调（原始JSON格式）
        // 支持两种格式：普通 kline 和 continuous_kline（连续合约K线）
        g_binance_ws_market->set_kline_callback([&zmq_server](const nlohmann::json& raw) {
            g_kline_count++;
            g_binance_kline_count++;

            // continuous_kline 格式: ps(交易对), ct(合约类型), k(K线数据)
            // 普通 kline 格式: s(交易对), k(K线数据)
            std::string symbol = "";
            if (raw.contains("ps")) {
                symbol = json_to_string(raw["ps"]);
            } else if (raw.contains("s")) {
                symbol = json_to_string(raw["s"]);
            }

            // 将 symbol 转换为大写（Binance 格式）
            std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);

            nlohmann::json msg = {
                {"type", "kline"},
                {"exchange", "binance"},
                {"symbol", symbol},
                {"timestamp_ns", current_timestamp_ns()}
            };

            if (raw.contains("k")) {
                const auto& k = raw["k"];
                if (k.contains("i")) msg["interval"] = json_to_string(k["i"]);
                if (k.contains("o")) msg["open"] = json_to_double(k["o"]);
                if (k.contains("h")) msg["high"] = json_to_double(k["h"]);
                if (k.contains("l")) msg["low"] = json_to_double(k["l"]);
                if (k.contains("c")) msg["close"] = json_to_double(k["c"]);
                if (k.contains("v")) msg["volume"] = json_to_double(k["v"]);
                if (k.contains("q")) msg["amount"] = json_to_double(k["q"]);
                if (k.contains("Q")) msg["buy_amount"] = json_to_double(k["Q"]);
                if (k.contains("n")) msg["trades"] = json_to_int64(k["n"]);
                if (k.contains("t")) msg["timestamp"] = json_to_int64(k["t"]);
            }

            // 发布到 Binance 专用通道
            zmq_server.publish_binance_market(msg, MessageType::KLINE);
            // 同时发布到统一通道
            zmq_server.publish_kline(msg);

            // Redis 录制 K线 数据（仅当 K 线完结时保存，x=true 表示已完结）
            if (g_redis_recorder && g_redis_recorder->is_running()) {
                bool is_closed = false;
                if (raw.contains("k") && raw["k"].contains("x")) {
                    is_closed = raw["k"]["x"].get<bool>();
                }
                if (is_closed) {
                    std::string interval = msg.value("interval", "1m");
                    g_redis_recorder->record_kline(symbol, interval, "binance", msg);
                }
            }
        });

        // Binance 标记价格回调（原始JSON格式）- 注意：目前设在 g_binance_ws_market，但实际 markPrice 在 g_binance_ws_depth
        g_binance_ws_market->set_mark_price_callback([&zmq_server](const nlohmann::json& raw) {
            g_binance_markprice_count++;
            g_funding_rate_count++;

            // Binance markPrice 字段: s(symbol), p(markPrice), i(indexPrice), r(fundingRate), T(nextFundingTime), E(eventTime)
            std::string symbol = "";
            if (raw.contains("s")) {
                symbol = json_to_string(raw["s"]);
            }

            nlohmann::json msg = {
                {"type", "mark_price"},
                {"exchange", "binance"},
                {"symbol", symbol},
                {"timestamp_ns", current_timestamp_ns()}
            };

            if (raw.contains("p")) msg["mark_price"] = json_to_double(raw["p"]);
            if (raw.contains("i")) msg["index_price"] = json_to_double(raw["i"]);
            if (raw.contains("r")) msg["funding_rate"] = json_to_double(raw["r"]);
            if (raw.contains("T")) msg["next_funding_time"] = json_to_int64(raw["T"]);
            if (raw.contains("E")) msg["timestamp"] = json_to_int64(raw["E"]);

            // 发布到 Binance 专用通道
            zmq_server.publish_binance_market(msg, MessageType::TICKER);
            // 同时发布到统一通道
            zmq_server.publish_ticker(msg);

            // Redis 录制 Funding Rate 数据（Mark Price 包含资金费率）
            if (g_redis_recorder && g_redis_recorder->is_running() && msg.contains("funding_rate")) {
                g_redis_recorder->record_funding_rate(symbol, "binance", msg);
            }
        });
    }

    // Binance 用户数据流回调
    if (g_binance_ws_user) {
        // 账户更新回调
        g_binance_ws_user->set_account_update_callback([&zmq_server](const nlohmann::json& acc) {
            nlohmann::json msg = {
                {"type", "account_update"},
                {"exchange", "binance"},
                {"data", acc},
                {"timestamp", current_timestamp_ms()}
            };
            zmq_server.publish_report(msg);

            // 发送到前端 WebSocket（Binance 实盘账户更新）
            if (g_frontend_server) {
                g_frontend_server->send_event("account_update", msg);
            }

            // 从 Binance ACCOUNT_UPDATE 事件中提取持仓数据，转换为统一格式发送 position_update
            // Binance ACCOUNT_UPDATE 格式: { "a": { "P": [ { "s": symbol, "pa": positionAmt, "ep": entryPrice, ... } ] } }
            if (acc.contains("a") && acc["a"].contains("P") && acc["a"]["P"].is_array()) {
                nlohmann::json pos_data = nlohmann::json::array();
                for (const auto& p : acc["a"]["P"]) {
                    std::string symbol = p.value("s", "");
                    std::string pos_amt = p.value("pa", "0");
                    if (symbol.empty()) continue;

                    pos_data.push_back({
                        {"instId", symbol},
                        {"posSide", p.value("ps", "BOTH")},
                        {"pos", pos_amt},
                        {"avgPx", p.value("ep", "0")},
                        {"markPx", "0"},
                        {"upl", p.value("up", "0")},
                        {"lever", "1"},
                        {"liqPx", "0"}
                    });
                }

                if (!pos_data.empty()) {
                    nlohmann::json pos_msg = {
                        {"type", "position_update"},
                        {"exchange", "binance"},
                        {"data", pos_data},
                        {"timestamp", current_timestamp_ms()}
                    };
                    zmq_server.publish_report(pos_msg);

                    if (g_frontend_server) {
                        g_frontend_server->send_event("position_update", pos_msg);
                    }
                }
            }
        });

        // 订单成交更新回调
        g_binance_ws_user->set_order_trade_update_callback([&zmq_server](const nlohmann::json& order) {
            nlohmann::json msg = {
                {"type", "order_update"},
                {"exchange", "binance"},
                {"data", order},
                {"timestamp", current_timestamp_ms()}
            };
            zmq_server.publish_report(msg);

            // 发送到前端 WebSocket（Binance 实盘订单更新）
            if (g_frontend_server) {
                g_frontend_server->send_event("order_update", msg);
            }
        });
    }

    // Binance markPrice 专用连接（g_binance_ws_depth 实际用于 !markPrice@arr）
    if (g_binance_ws_depth) {
        g_binance_ws_depth->set_mark_price_callback([&zmq_server](const nlohmann::json& raw) {
            g_binance_markprice_count++;
            g_funding_rate_count++;

            // Binance markPrice 字段: s(symbol), p(markPrice), i(indexPrice), r(fundingRate), T(nextFundingTime), E(eventTime)
            std::string symbol = "";
            if (raw.contains("s")) {
                symbol = json_to_string(raw["s"]);
            }

            nlohmann::json msg = {
                {"type", "mark_price"},
                {"exchange", "binance"},
                {"symbol", symbol},
                {"timestamp_ns", current_timestamp_ns()}
            };

            if (raw.contains("p")) msg["mark_price"] = json_to_double(raw["p"]);
            if (raw.contains("i")) msg["index_price"] = json_to_double(raw["i"]);
            if (raw.contains("r")) msg["funding_rate"] = json_to_double(raw["r"]);
            if (raw.contains("T")) msg["next_funding_time"] = json_to_int64(raw["T"]);
            if (raw.contains("E")) msg["timestamp"] = json_to_int64(raw["E"]);

            // 发布到 Binance 专用通道
            zmq_server.publish_binance_market(msg, MessageType::TICKER);
            // 同时发布到统一通道
            zmq_server.publish_ticker(msg);

            // Redis 录制 Funding Rate 数据（Mark Price 包含资金费率）
            if (g_redis_recorder && g_redis_recorder->is_running() && msg.contains("funding_rate")) {
                g_redis_recorder->record_funding_rate(symbol, "binance", msg);
            }
        });
    }
}

void setup_binance_kline_callback(
    binance::BinanceWebSocket* ws,
    ZmqServer& zmq_server,
    std::function<void(const std::string& symbol, int64_t timestamp)> on_closed_kline
) {
    if (!ws) return;

    ws->set_kline_callback([&zmq_server, on_closed_kline](const nlohmann::json& raw) {
        // continuous_kline 格式: ps(交易对), ct(合约类型), k(K线数据)
        // 普通 kline 格式: s(交易对), k(K线数据)
        std::string symbol = "";
        if (raw.contains("ps")) {
            symbol = json_to_string(raw["ps"]);
        } else if (raw.contains("s")) {
            symbol = json_to_string(raw["s"]);
        }

        // 将 symbol 转换为大写（Binance 格式）
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);

        nlohmann::json msg = {
            {"type", "kline"},
            {"exchange", "binance"},
            {"symbol", symbol},
            {"timestamp_ns", current_timestamp_ns()}
        };

        if (raw.contains("k")) {
            const auto& k = raw["k"];
            if (k.contains("i")) msg["interval"] = json_to_string(k["i"]);
            if (k.contains("o")) msg["open"] = json_to_double(k["o"]);
            if (k.contains("h")) msg["high"] = json_to_double(k["h"]);
            if (k.contains("l")) msg["low"] = json_to_double(k["l"]);
            if (k.contains("c")) msg["close"] = json_to_double(k["c"]);
            if (k.contains("v")) msg["volume"] = json_to_double(k["v"]);
            if (k.contains("q")) msg["amount"] = json_to_double(k["q"]);
            if (k.contains("Q")) msg["buy_amount"] = json_to_double(k["Q"]);
            if (k.contains("n")) msg["trades"] = json_to_int64(k["n"]);
            if (k.contains("t")) msg["timestamp"] = json_to_int64(k["t"]);
        }

        // 检查 K线 是否完结（x=true 表示已完结）
        bool is_closed = false;
        if (raw.contains("k") && raw["k"].contains("x")) {
            is_closed = raw["k"]["x"].get<bool>();
        }

        // 仅当 K线 完结时才发布到 ZMQ 和 Redis（与 OKX 行为一致）
        if (is_closed) {
            g_kline_count++;
            g_binance_kline_count++;

            // 发布到 Binance 专用通道
            zmq_server.publish_binance_market(msg, MessageType::KLINE);
            // 同时发布到统一通道
            zmq_server.publish_kline(msg);

            if (on_closed_kline) {
                on_closed_kline(symbol, msg.value("timestamp", 0LL));
            }

            // Redis 录制 K线 数据
            if (g_redis_recorder && g_redis_recorder->is_running()) {
                std::string interval = msg.value("interval", "1m");
                g_redis_recorder->record_kline(symbol, interval, "binance", msg);
            }
        }
    });
}

void setup_binance_kline_callback(binance::BinanceWebSocket* ws, ZmqServer& zmq_server) {
    setup_binance_kline_callback(ws, zmq_server, nullptr);
}

} // namespace server
} // namespace trading
