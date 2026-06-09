#pragma once

/**
 * @file proxy_config.h
 * @brief 公共代理配置
 *
 * 提供统一的代理配置，供 WebSocket 和 REST API 使用
 *
 * @author Sequence Team
 * @date 2024-12
 */

#include <string>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace trading {
namespace core {

/**
 * @brief 代理配置
 */
struct ProxyConfig {
    bool use_proxy = false;               // 是否使用代理
    std::string proxy_host = "127.0.0.1"; // 代理主机
    uint16_t proxy_port = 7890;           // 代理端口

    /**
     * @brief 获取代理 URL（http://host:port 格式）
     */
    std::string get_proxy_url() const {
        if (!use_proxy) return "";
        return "http://" + proxy_host + ":" + std::to_string(proxy_port);
    }

    /**
     * @brief 从环境变量加载代理配置
     * @return 如果找到环境变量代理则返回 true
     */
    bool load_from_env() {
        const char* proxy_env = std::getenv("https_proxy");
        if (!proxy_env) proxy_env = std::getenv("HTTPS_PROXY");
        if (!proxy_env) proxy_env = std::getenv("http_proxy");
        if (!proxy_env) proxy_env = std::getenv("HTTP_PROXY");
        if (!proxy_env) proxy_env = std::getenv("all_proxy");
        if (!proxy_env) proxy_env = std::getenv("ALL_PROXY");

        if (proxy_env && strlen(proxy_env) > 0) {
            // 解析代理 URL: http://host:port
            std::string url(proxy_env);
            size_t pos = url.find("://");
            if (pos != std::string::npos) {
                url = url.substr(pos + 3);
            }
            size_t colon = url.find(':');
            if (colon != std::string::npos) {
                proxy_host = url.substr(0, colon);
                proxy_port = static_cast<uint16_t>(std::stoi(url.substr(colon + 1)));
            } else {
                proxy_host = url;
                proxy_port = 7890;  // 默认端口
            }
            use_proxy = true;
            return true;
        }
        return false;
    }

    /**
     * @brief 获取默认代理配置（优先从环境变量加载）
     */
    static ProxyConfig get_default() {
        ProxyConfig config;
        config.load_from_env();
        return config;
    }
};

} // namespace core
} // namespace trading
