#pragma once

/**
 * @file secure_frontend_handler.h
 * @brief 带认证的前端请求处理器
 *
 * 功能：
 * - 用户登录/登出
 * - Token验证
 * - 权限检查
 * - 账户管理（需要认证）
 */

#include <string>
#include <thread>
#include <atomic>
#include <zmq.hpp>
#include <nlohmann/json.hpp>
#include "auth_manager.h"
#include "../trading/account_registry.h"

namespace trading {
namespace server {

/**
 * @brief 带认证的前端请求处理器
 */
class SecureFrontendHandler {
public:
    SecureFrontendHandler(AccountRegistry& registry)
        : registry_(registry), running_(false) {}

    ~SecureFrontendHandler() { stop(); }

    /**
     * @brief 启动处理器
     */
    bool start(const std::string& endpoint = "tcp://*:5556") {
        if (running_) return false;

        endpoint_ = endpoint;
        running_ = true;

        thread_ = std::thread([this]() { run(); });
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

    /**
     * @brief 获取认证管理器（用于配置）
     */
    auth::AuthManager& auth_manager() { return auth_; }

private:
    void run() {
        zmq::context_t context(1);
        zmq::socket_t socket(context, zmq::socket_type::rep);
        socket.bind(endpoint_);

        std::cout << "[安全前端] 监听端口: " << endpoint_ << "\n";

        while (running_) {
            zmq::message_t request;

            auto result = socket.recv(request, zmq::recv_flags::dontwait);
            if (!result) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::string msg_str(static_cast<char*>(request.data()), request.size());
            nlohmann::json response;

            try {
                auto msg = nlohmann::json::parse(msg_str);
                response = handle_request(msg);
            } catch (const std::exception& e) {
                response = {{"status", "error"}, {"code", 500}, {"message", e.what()}};
            }

            std::string response_str = response.dump();
            socket.send(zmq::buffer(response_str), zmq::send_flags::none);
        }
    }

    nlohmann::json handle_request(const nlohmann::json& msg) {
        std::string type = msg.value("type", "");

        // 公开接口（无需认证）
        if (type == "login") {
            return handle_login(msg);
        }

        // 需要认证的接口
        std::string token = msg.value("token", "");
        auth::TokenInfo token_info;

        if (!auth_.verify_token(token, token_info)) {
            return {{"status", "error"}, {"code", 401}, {"message", "Unauthorized"}};
        }

        // 路由请求
        if (type == "logout") {
            return handle_logout(token);
        } else if (type == "get_user_info") {
            return handle_get_user_info(token_info);
        } else if (type == "change_password") {
            return handle_change_password(msg, token_info);
        } else if (type == "register_account") {
            return handle_register_account(msg, token_info);
        } else if (type == "unregister_account") {
            return handle_unregister_account(msg, token_info);
        } else if (type == "list_accounts") {
            return handle_list_accounts(token_info);
        } else if (type == "add_user") {
            return handle_add_user(msg, token_info);
        } else if (type == "list_users") {
            return handle_list_users(token_info);
        } else {
            return {{"status", "error"}, {"code", 400}, {"message", "Unknown request type"}};
        }
    }

    // ==================== 认证接口 ====================

    nlohmann::json handle_login(const nlohmann::json& msg) {
        std::string username = msg.value("username", "");
        std::string password = msg.value("password", "");

        if (username.empty() || password.empty()) {
            return {{"status", "error"}, {"code", 400}, {"message", "Missing username or password"}};
        }

        std::string token = auth_.login(username, password);
        if (token.empty()) {
            return {{"status", "error"}, {"code", 401}, {"message", "Invalid credentials"}};
        }

        // 获取用户角色
        auth::TokenInfo info;
        auth_.verify_token(token, info);

        return {
            {"status", "success"},
            {"code", 200},
            {"token", token},
            {"user", {
                {"username", username},
                {"role", auth::AuthManager::role_to_string(info.role)}
            }}
        };
    }

    nlohmann::json handle_logout(const std::string& token) {
        auth_.logout(token);
        return {{"status", "success"}, {"code", 200}, {"message", "Logged out"}};
    }

    nlohmann::json handle_get_user_info(const auth::TokenInfo& token_info) {
        return {
            {"status", "success"},
            {"code", 200},
            {"user", {
                {"username", token_info.username},
                {"role", auth::AuthManager::role_to_string(token_info.role)}
            }}
        };
    }

    nlohmann::json handle_change_password(const nlohmann::json& msg, const auth::TokenInfo& token_info) {
        std::string old_password = msg.value("old_password", "");
        std::string new_password = msg.value("new_password", "");

        if (old_password.empty() || new_password.empty()) {
            return {{"status", "error"}, {"code", 400}, {"message", "Missing password"}};
        }

        if (new_password.length() < 6) {
            return {{"status", "error"}, {"code", 400}, {"message", "Password too short (min 6 chars)"}};
        }

        if (auth_.change_password(token_info.username, old_password, new_password)) {
            return {{"status", "success"}, {"code", 200}, {"message", "Password changed"}};
        }

        return {{"status", "error"}, {"code", 400}, {"message", "Invalid old password"}};
    }

    // ==================== 账户管理接口 ====================

    nlohmann::json handle_register_account(const nlohmann::json& msg, const auth::TokenInfo& token_info) {
        // 权限检查：需要TRADER或更高权限
        if (!auth_.has_permission(token_info.role, "trade")) {
            return {{"status", "error"}, {"code", 403}, {"message", "Permission denied"}};
        }

        std::string strategy_id = msg.value("strategy_id", "");
        std::string exchange = msg.value("exchange", "okx");
        std::string api_key = msg.value("api_key", "");
        std::string secret_key = msg.value("secret_key", "");
        std::string passphrase = msg.value("passphrase", "");
        bool is_testnet = msg.value("is_testnet", true);

        if (api_key.empty() || secret_key.empty()) {
            return {{"status", "error"}, {"code", 400}, {"message", "Missing api_key or secret_key"}};
        }

        ExchangeType ex_type = string_to_exchange_type(exchange);
        bool success = false;

        if (strategy_id.empty()) {
            if (ex_type == ExchangeType::OKX) {
                registry_.set_default_okx_account(api_key, secret_key, passphrase, is_testnet);
                success = true;
            } else if (ex_type == ExchangeType::BINANCE) {
                registry_.set_default_binance_account(api_key, secret_key, is_testnet);
                success = true;
            }
        } else {
            success = registry_.register_account(
                strategy_id, ex_type, api_key, secret_key, passphrase, is_testnet
            );
        }

        return {
            {"status", success ? "success" : "error"},
            {"code", success ? 200 : 500},
            {"message", success ? "Account registered" : "Registration failed"}
        };
    }

    nlohmann::json handle_unregister_account(const nlohmann::json& msg, const auth::TokenInfo& token_info) {
        if (!auth_.has_permission(token_info.role, "trade")) {
            return {{"status", "error"}, {"code", 403}, {"message", "Permission denied"}};
        }

        std::string strategy_id = msg.value("strategy_id", "");
        std::string exchange = msg.value("exchange", "okx");

        if (strategy_id.empty()) {
            return {{"status", "error"}, {"code", 400}, {"message", "Missing strategy_id"}};
        }

        ExchangeType ex_type = string_to_exchange_type(exchange);
        bool success = registry_.unregister_account(strategy_id, ex_type);

        return {
            {"status", success ? "success" : "error"},
            {"code", success ? 200 : 404},
            {"message", success ? "Account unregistered" : "Account not found"}
        };
    }

    nlohmann::json handle_list_accounts(const auth::TokenInfo& token_info) {
        if (!auth_.has_permission(token_info.role, "view")) {
            return {{"status", "error"}, {"code", 403}, {"message", "Permission denied"}};
        }

        return {
            {"status", "success"},
            {"code", 200},
            {"okx_count", registry_.okx_count()},
            {"binance_count", registry_.binance_count()},
            {"total", registry_.count()}
        };
    }

    // ==================== 用户管理接口（管理员） ====================

    nlohmann::json handle_add_user(const nlohmann::json& msg, const auth::TokenInfo& token_info) {
        // 只有管理员可以添加用户
        if (token_info.role != auth::UserRole::SUPER_ADMIN && token_info.role != auth::UserRole::ADMIN) {
            return {{"status", "error"}, {"code", 403}, {"message", "Admin permission required"}};
        }

        std::string username = msg.value("username", "");
        std::string password = msg.value("password", "");
        std::string role_str = msg.value("role", "VIEWER");

        if (username.empty() || password.empty()) {
            return {{"status", "error"}, {"code", 400}, {"message", "Missing username or password"}};
        }

        if (password.length() < 6) {
            return {{"status", "error"}, {"code", 400}, {"message", "Password too short (min 6 chars)"}};
        }

        auth::UserRole role = auth::AuthManager::string_to_role(role_str);

        // 普通管理员不能创建超级管理员
        if (token_info.role == auth::UserRole::ADMIN && role == auth::UserRole::SUPER_ADMIN) {
            return {{"status", "error"}, {"code", 403}, {"message", "Cannot create SUPER_ADMIN"}};
        }

        if (auth_.add_user(username, password, role)) {
            return {{"status", "success"}, {"code", 200}, {"message", "User created"}};
        }

        return {{"status", "error"}, {"code", 409}, {"message", "User already exists"}};
    }

    nlohmann::json handle_list_users(const auth::TokenInfo& token_info) {
        if (token_info.role != auth::UserRole::SUPER_ADMIN && token_info.role != auth::UserRole::ADMIN) {
            return {{"status", "error"}, {"code", 403}, {"message", "Admin permission required"}};
        }

        return {
            {"status", "success"},
            {"code", 200},
            {"users", auth_.get_users()}
        };
    }

    AccountRegistry& registry_;
    auth::AuthManager auth_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::string endpoint_;
};

} // namespace server
} // namespace trading
