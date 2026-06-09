/**
 * @file subscription_manager.h
 * @brief 订阅管理模块
 */

#pragma once

#include <nlohmann/json.hpp>

namespace trading {
namespace server {

/**
 * @brief 处理订阅请求
 */
void handle_subscription(const nlohmann::json& request);

} // namespace server
} // namespace trading
