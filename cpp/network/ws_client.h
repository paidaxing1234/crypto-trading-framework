#pragma once

/**
 * @file ws_client.h
 * @brief 公共 WebSocket 客户端
 *
 * 提供统一的 WebSocket 连接管理，供 OKX/Binance 等适配器使用
 *
 * @author Sequence Team
 * @date 2024-12
 */

#include <string>
#include <functional>
#include <memory>
#include <cstdint>

namespace trading {
namespace core {

/**
 * @brief WebSocket 配置
 */
struct WebSocketConfig {
    bool verify_ssl = false;              // SSL 证书验证（生产环境建议开启）
    bool use_proxy = false;               // 是否使用代理
    std::string proxy_host = "127.0.0.1"; // 代理主机
    uint16_t proxy_port = 7890;           // 代理端口
    int connect_timeout_sec = 5;          // 连接超时（秒）
    int ping_interval_sec = 30;           // 主动 ping 间隔（秒），0 表示禁用
};

/**
 * @brief WebSocket 客户端
 *
 * 封装 WebSocket++ 的底层实现，提供统一的接口
 */
class WebSocketClient {
public:
    explicit WebSocketClient(const WebSocketConfig& config = {});
    ~WebSocketClient();

    // 禁止拷贝
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    /**
     * @brief 连接到 WebSocket 服务器
     * @param url WebSocket URL (wss://...)
     * @return 连接是否成功
     */
    bool connect(const std::string& url);

    /**
     * @brief 断开连接
     */
    void disconnect();

    /**
     * @brief 发送消息
     * @param message 消息内容
     * @return 发送是否成功
     */
    bool send(const std::string& message);

    /**
     * @brief 是否已连接
     */
    bool is_connected() const;

    /**
     * @brief 设置消息回调
     */
    void set_message_callback(std::function<void(const std::string&)> callback);

    /**
     * @brief 设置连接关闭回调
     */
    void set_close_callback(std::function<void()> callback);

    /**
     * @brief 设置连接失败回调
     */
    void set_fail_callback(std::function<void()> callback);

    /**
     * @brief 安全停止（清理回调并等待线程退出）
     */
    void safe_stop();

    /**
     * @brief 清除所有回调
     */
    void clear_callbacks();

    /**
     * @brief 设置代理
     */
    void set_proxy(const std::string& host, uint16_t port);

    /**
     * @brief 获取当前配置
     */
    const WebSocketConfig& get_config() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
} // namespace trading
