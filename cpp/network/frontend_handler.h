#pragma once

/**
 * @file frontend_handler.h
 * @brief 前端请求处理器（独立线程，非阻塞）
 *
 * 功能：
 * - 独立线程处理前端账户管理请求
 * - 不阻塞主交易线程
 * - 通过 ZeroMQ REP socket 接收请求
 */

#include <string>
#include <thread>
#include <atomic>
#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include "../trading/account_registry.h"

namespace trading {
namespace server {

/**
 * @brief 前端请求处理器
 */
class FrontendHandler {
public:
    FrontendHandler(AccountRegistry& registry)
        : registry_(registry), running_(false) {}

    ~FrontendHandler() { stop(); }

    /**
     * @brief 启动处理器（独立线程）
     */
    bool start(const std::string& endpoint = "tcp://*:5556") {
        if (running_) return false;

        endpoint_ = endpoint;
        running_ = true;

        thread_ = std::thread([this]() {
            run();
        });

        return true;
    }

    /**
     * @brief 停止处理器
     */
    void stop() {
        if (!running_) return;
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        zmq::context_t context(1);
        zmq::socket_t socket(context, zmq::socket_type::rep);
        socket.bind(endpoint_);

        std::cout << "[前端] 监听端口: " << endpoint_ << "\n";

        while (running_) {
            zmq::message_t request;

            // 非阻塞接收
            auto result = socket.recv(request, zmq::recv_flags::dontwait);
            if (!result) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // 解析请求
            std::string msg_str(static_cast<char*>(request.data()), request.size());
            nlohmann::json response;

            try {
                auto msg = nlohmann::json::parse(msg_str);
                response = handle_request(msg);
            } catch (const std::exception& e) {
                response = {{"status", "error"}, {"message", e.what()}};
            }

            // 发送响应
            std::string response_str = response.dump();
            socket.send(zmq::buffer(response_str), zmq::send_flags::none);
        }
    }

    nlohmann::json handle_request(const nlohmann::json& msg) {
        std::string type = msg.value("type", "");

        if (type == "register_account") {
            return handle_register(msg);
        } else if (type == "unregister_account") {
            return handle_unregister(msg);
        } else if (type == "list_accounts") {
            return handle_list();
        } else {
            return {{"status", "error"}, {"message", "Unknown request type"}};
        }
    }

    nlohmann::json handle_register(const nlohmann::json& msg) {
        std::string strategy_id = msg.value("strategy_id", "");
        std::string exchange = msg.value("exchange", "okx");
        std::string api_key = msg.value("api_key", "");
        std::string secret_key = msg.value("secret_key", "");
        std::string passphrase = msg.value("passphrase", "");
        bool is_testnet = msg.value("is_testnet", true);

        if (api_key.empty() || secret_key.empty()) {
            return {{"status", "error"}, {"message", "Missing api_key or secret_key"}};
        }

        ExchangeType ex_type = string_to_exchange_type(exchange);
        bool success = false;

        if (strategy_id.empty()) {
            // 默认账户
            if (ex_type == ExchangeType::OKX) {
                registry_.set_default_okx_account(api_key, secret_key, passphrase, is_testnet);
                success = true;
            } else if (ex_type == ExchangeType::BINANCE) {
                registry_.set_default_binance_account(api_key, secret_key, is_testnet);
                success = true;
            }
        } else {
            // 策略账户
            success = registry_.register_account(
                strategy_id, ex_type, api_key, secret_key, passphrase, is_testnet
            );
        }

        return {
            {"status", success ? "success" : "error"},
            {"message", success ? "Account registered" : "Registration failed"}
        };
    }

    nlohmann::json handle_unregister(const nlohmann::json& msg) {
        std::string strategy_id = msg.value("strategy_id", "");
        std::string exchange = msg.value("exchange", "okx");

        if (strategy_id.empty()) {
            return {{"status", "error"}, {"message", "Missing strategy_id"}};
        }

        ExchangeType ex_type = string_to_exchange_type(exchange);
        bool success = registry_.unregister_account(strategy_id, ex_type);

        return {
            {"status", success ? "success" : "error"},
            {"message", success ? "Account unregistered" : "Account not found"}
        };
    }

    nlohmann::json handle_list() {
        return {
            {"status", "success"},
            {"okx_count", registry_.okx_count()},
            {"binance_count", registry_.binance_count()},
            {"total", registry_.count()}
        };
    }

    AccountRegistry& registry_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::string endpoint_;
};

} // namespace server
} // namespace trading
