/**
 * @file query_handler.h
 * @brief 查询处理模块
 */

#pragma once

#include <nlohmann/json.hpp>

namespace trading {
namespace server {

/**
 * @brief 处理查询请求
 */
nlohmann::json handle_query(const nlohmann::json& request);

} // namespace server
} // namespace trading
