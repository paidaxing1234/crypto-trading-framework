/**
 * @file frontend_command_handler.h
 * @brief 前端 WebSocket 命令处理模块（含认证）
 */

#pragma once

#include <nlohmann/json.hpp>

namespace trading {
namespace server {

/**
 * @brief 处理前端 WebSocket 命令
 */
void handle_frontend_command(int client_id, const nlohmann::json& message);

} // namespace server
} // namespace trading
