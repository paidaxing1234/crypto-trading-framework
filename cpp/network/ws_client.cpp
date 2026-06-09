/**
 * @file ws_client.cpp
 * @brief 公共 WebSocket 客户端实现 - Perpetual Mode
 *
 * 核心修复：使用 start_perpetual() 模式
 * - IO 线程永久运行，不随连接销毁
 * - 避免 ASIO + SSL 的内存问题（stream truncated 导致的 double-free）
 * - 重连时只关闭旧连接，创建新连接，不销毁 Client
 *
 * @author Sequence Team
 * @date 2024-12
 */

#include "ws_client.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#ifdef USE_WEBSOCKETPP

#ifdef ASIO_STANDALONE
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
namespace asio = ::asio;
typedef websocketpp::client<websocketpp::config::asio_tls_client> WsClient;
typedef std::shared_ptr<asio::ssl::context> SslContextPtr;
#else
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
typedef websocketpp::client<websocketpp::config::asio_tls_client> WsClient;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> SslContextPtr;
#endif

namespace trading {
namespace core {

class WebSocketClient::Impl {
public:
    explicit Impl(const WebSocketConfig& config)
        : config_(config), is_connected_(false), stopped_(false), ping_running_(false),
          perpetual_running_(false) {

        if (config_.use_proxy) {
            std::cout << "[WebSocketClient] 默认使用 HTTP 代理: "
                      << config_.proxy_host << ":" << config_.proxy_port << std::endl;
        }

        // Perpetual Mode: 在构造时初始化 ASIO 并启动永久循环
        init_perpetual();
    }

    ~Impl() {
        // 完全停止
        shutdown();
    }

    /**
     * @brief 初始化 Perpetual 模式
     *
     * 调用 start_perpetual() 让 IO 线程永不退出
     * 即使没有连接，IO 服务也会保持运行
     */
    void init_perpetual() {
        if (perpetual_running_.load()) return;

        // 1. 配置日志级别 (静默模式，防止日志刷屏)
        client_.clear_access_channels(websocketpp::log::alevel::all);
        client_.clear_error_channels(websocketpp::log::elevel::all);
        client_.set_access_channels(websocketpp::log::alevel::connect);
        client_.set_access_channels(websocketpp::log::alevel::disconnect);

        // 2. 初始化 ASIO
        client_.init_asio();

        // 3. 关键：启动 perpetual 模式，IO 线程永不因没有工作而退出
        client_.start_perpetual();

        // 4. 设置 SSL/TLS 上下文处理器
        client_.set_tls_init_handler([this](websocketpp::connection_hdl) {
            return create_ssl_context();
        });

        // 5. 启动 IO 线程（这个线程会一直运行直到 shutdown）
        io_thread_ = std::make_unique<std::thread>([this]() {
            try {
                std::cout << "[WebSocketClient] IO Thread Started (Perpetual Mode)" << std::endl;
                client_.run();
                std::cout << "[WebSocketClient] IO Thread Exited" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[WebSocketClient] IO Loop Exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[WebSocketClient] IO Loop Unknown Crash" << std::endl;
            }
        });

        perpetual_running_.store(true);
        std::cout << "[WebSocketClient] Perpetual Mode Initialized" << std::endl;
    }

    SslContextPtr create_ssl_context() {
#ifdef ASIO_STANDALONE
        SslContextPtr ctx = std::make_shared<asio::ssl::context>(
            asio::ssl::context::tlsv12_client);
        ctx->set_default_verify_paths();
        ctx->set_options(asio::ssl::context::default_workarounds |
                         asio::ssl::context::no_sslv2 |
                         asio::ssl::context::no_sslv3 |
                         asio::ssl::context::single_dh_use);
        if (config_.verify_ssl) {
            ctx->set_verify_mode(asio::ssl::verify_peer);
        } else {
            ctx->set_verify_mode(asio::ssl::verify_none);
        }
#else
        SslContextPtr ctx = std::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12_client);
        ctx->set_default_verify_paths();
        ctx->set_options(boost::asio::ssl::context::default_workarounds |
                         boost::asio::ssl::context::no_sslv2 |
                         boost::asio::ssl::context::no_sslv3 |
                         boost::asio::ssl::context::single_dh_use);
        if (config_.verify_ssl) {
            ctx->set_verify_mode(boost::asio::ssl::verify_peer);
        } else {
            ctx->set_verify_mode(boost::asio::ssl::verify_none);
        }
#endif
        return ctx;
    }

    void set_proxy(const std::string& host, uint16_t port) {
        config_.proxy_host = host;
        config_.proxy_port = port;
        config_.use_proxy = true;
    }

    bool connect(const std::string& url) {
        // 1. 关闭旧连接（如果存在），但不销毁 client
        close_connection();
        stopped_.store(false);

        // 2. 确保 perpetual 模式已启动
        if (!perpetual_running_.load()) {
            init_perpetual();
        }

        // 3. 创建新连接
        websocketpp::lib::error_code ec;
        WsClient::connection_ptr new_con = client_.get_connection(url, ec);

        if (ec) {
            std::cerr << "[WebSocketClient] 连接错误: " << ec.message() << std::endl;
            return false;
        }

        // 保存连接指针
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            connection_ = new_con;
            connection_hdl_ = new_con->get_handle();
        }

        // 设置代理
        if (config_.use_proxy) {
            std::string proxy_uri = "http://" + config_.proxy_host + ":" + std::to_string(config_.proxy_port);
            new_con->set_proxy(proxy_uri);
        }

        // 设置消息回调
        new_con->set_message_handler(
            [this](websocketpp::connection_hdl, WsClient::message_ptr msg) {
                if (message_callback_) {
                    try {
                        message_callback_(msg->get_payload());
                    } catch (const std::exception& e) {
                        std::cerr << "[WebSocketClient] Message Callback Error: " << e.what() << std::endl;
                    }
                }
            });

        // 设置 ping 处理器（服务器发送 ping 帧时触发，websocketpp 会自动回复 pong）
        new_con->set_ping_handler([this](websocketpp::connection_hdl, std::string payload) -> bool {
            // 记录收到的 ping（调试用，生产环境可注释掉）
            static std::atomic<int> ping_count{0};
            int count = ++ping_count;
            if (count <= 5 || count % 100 == 0) {
                std::cout << "[WebSocketClient] 收到服务器 ping #" << count
                          << " (payload: " << payload.substr(0, 20) << ")" << std::endl;
            }
            // 返回 true 表示由 websocketpp 自动发送 pong 响应
            return true;
        });

        // 设置 pong 处理器（收到服务器对我们 ping 的响应时触发）
        new_con->set_pong_handler([this](websocketpp::connection_hdl, std::string) {
            // 收到服务器的 pong 响应（可选：更新心跳状态）
        });

        // 设置打开回调
        new_con->set_open_handler([this](websocketpp::connection_hdl) {
            {
                std::lock_guard<std::mutex> lock(connect_mutex_);
                is_connected_ = true;
            }
            connect_cv_.notify_one();
            std::cout << "[WebSocketClient] 连接成功" << std::endl;
        });

        // 设置关闭回调
        new_con->set_close_handler([this](websocketpp::connection_hdl) {
            is_connected_ = false;
            std::cout << "[WebSocketClient] 连接已关闭" << std::endl;

            // 清理连接指针（但不销毁 client）
            {
                std::lock_guard<std::mutex> lock(connection_mutex_);
                connection_.reset();
            }

            if (close_callback_) {
                close_callback_();
            }
        });

        // 设置失败回调
        new_con->set_fail_handler([this](websocketpp::connection_hdl) {
            {
                std::lock_guard<std::mutex> lock(connect_mutex_);
                is_connected_ = false;
            }
            connect_cv_.notify_one();
            std::cerr << "[WebSocketClient] 连接失败" << std::endl;

            // 清理连接指针（但不销毁 client）
            {
                std::lock_guard<std::mutex> lock(connection_mutex_);
                connection_.reset();
            }

            if (fail_callback_) {
                fail_callback_();
            }
        });

        // 4. 发起连接
        try {
            client_.connect(new_con);
        } catch (const std::exception& e) {
            std::cerr << "[WebSocketClient] Connect Call Failed: " << e.what() << std::endl;
            return false;
        }

        // 5. 等待连接完成
        {
            std::unique_lock<std::mutex> lock(connect_mutex_);
            bool connected = connect_cv_.wait_for(
                lock,
                std::chrono::seconds(config_.connect_timeout_sec),
                [this]() { return is_connected_.load(); }
            );
            if (!connected) {
                std::cerr << "[WebSocketClient] 连接超时" << std::endl;
            }
        }

        // 6. 连接成功后启动 ping 线程
        if (is_connected_ && config_.ping_interval_sec > 0) {
            start_ping_thread();
        }

        return is_connected_;
    }

    void disconnect() {
        close_connection();
    }

    /**
     * @brief 仅关闭当前连接，不销毁 Client
     *
     * 这是 Perpetual Mode 的关键：
     * - 关闭连接后，IO 线程仍在运行
     * - 可以随时创建新连接
     */
    void close_connection() {
        // 1. 停止 ping 线程
        stop_ping_thread();

        // 2. 标记为断开状态
        bool was_connected = is_connected_.exchange(false);
        stopped_.store(true);

        // 3. 关闭 WebSocket 连接
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            if (connection_) {
                if (was_connected) {
                    websocketpp::lib::error_code ec;
                    try {
                        client_.close(connection_->get_handle(),
                                      websocketpp::close::status::normal,
                                      "disconnect", ec);
                        // 不检查 ec，有时候连接已经关闭
                    } catch (...) {}
                }
                // 等待一小段时间让 close 完成处理
                connection_.reset();
            }
        }
    }

    /**
     * @brief 完全关闭（析构时调用）
     *
     * 停止 perpetual 模式，结束 IO 线程
     */
    void shutdown() {
        if (!perpetual_running_.load()) {
            return;  // 已经关闭
        }

        // 1. 标记为关闭中
        perpetual_running_.store(false);
        is_connected_.store(false);
        stopped_.store(true);

        // 2. 停止 ping 线程
        stop_ping_thread();

        // 3. 关闭连接（在 IO 线程中处理）
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            if (connection_) {
                websocketpp::lib::error_code ec;
                try {
                    client_.close(connection_->get_handle(),
                                  websocketpp::close::status::going_away,
                                  "shutdown", ec);
                } catch (...) {}
                connection_.reset();
            }
        }

        // 4. 停止 perpetual 模式（让 run() 可以退出）
        try {
            client_.stop_perpetual();
        } catch (...) {}

        // 5. 等待一小段时间让关闭消息处理完
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 6. 停止 ASIO（强制退出 run()）
        try {
            client_.stop();
        } catch (...) {}

        // 7. 等待 IO 线程结束
        if (io_thread_ && io_thread_->joinable()) {
            io_thread_->join();
        }
        io_thread_.reset();

        std::cout << "[WebSocketClient] Shutdown Complete" << std::endl;
    }

    bool send(const std::string& message) {
        if (!is_connected_.load()) return false;

        // 加锁获取连接指针的副本，防止多线程竞争
        WsClient::connection_ptr con_copy;
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            con_copy = connection_;
        }

        if (!con_copy) return false;

        websocketpp::lib::error_code ec;
        try {
            client_.send(con_copy->get_handle(), message, websocketpp::frame::opcode::text, ec);
        } catch (const std::exception& e) {
            std::cerr << "[WebSocketClient] 发送异常: " << e.what() << std::endl;
            return false;
        }

        if (ec) {
            std::cerr << "[WebSocketClient] 发送错误: " << ec.message() << std::endl;
            return false;
        }
        return true;
    }

    void set_message_callback(std::function<void(const std::string&)> callback) {
        message_callback_ = std::move(callback);
    }

    void set_close_callback(std::function<void()> callback) {
        close_callback_ = std::move(callback);
    }

    void set_fail_callback(std::function<void()> callback) {
        fail_callback_ = std::move(callback);
    }

    void clear_callbacks() {
        message_callback_ = nullptr;
        close_callback_ = nullptr;
        fail_callback_ = nullptr;
    }

    void safe_stop() {
        close_connection();
    }

    bool is_connected() const { return is_connected_; }

    const WebSocketConfig& get_config() const { return config_; }

private:
    void start_ping_thread() {
        if (ping_running_.load()) return;

        ping_running_.store(true);
        ping_thread_ = std::make_unique<std::thread>([this]() {
            while (ping_running_.load() && is_connected_.load()) {
                // 使用小间隔循环，以便能快速响应停止请求
                for (int i = 0; i < config_.ping_interval_sec * 10 && ping_running_.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                if (!ping_running_.load() || !is_connected_.load()) break;

                // 发送 WebSocket ping frame（使用互斥锁保护 connection_）
                WsClient::connection_ptr con_copy;
                {
                    std::lock_guard<std::mutex> lock(connection_mutex_);
                    con_copy = connection_;
                }

                if (con_copy) {
                    try {
                        websocketpp::lib::error_code ec;
                        client_.ping(con_copy->get_handle(), "keepalive", ec);
                        if (ec) {
                            std::cerr << "[WebSocketClient] Ping 发送失败: " << ec.message() << std::endl;
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[WebSocketClient] Ping 异常: " << e.what() << std::endl;
                    }
                }
            }
        });
    }

    void stop_ping_thread() {
        ping_running_.store(false);
        if (ping_thread_ && ping_thread_->joinable()) {
            ping_thread_->join();
            ping_thread_.reset();
        }
    }

    WebSocketConfig config_;

    // Perpetual Mode: Client 是直接成员，生命周期与 Impl 相同
    WsClient client_;

    // 连接句柄
    WsClient::connection_ptr connection_;
    websocketpp::connection_hdl connection_hdl_;
    mutable std::mutex connection_mutex_;  // 保护 connection_ 指针

    std::unique_ptr<std::thread> io_thread_;
    std::unique_ptr<std::thread> ping_thread_;

    std::atomic<bool> is_connected_;
    std::atomic<bool> stopped_;
    std::atomic<bool> ping_running_;
    std::atomic<bool> perpetual_running_;  // perpetual 模式是否已启动

    std::function<void(const std::string&)> message_callback_;
    std::function<void()> close_callback_;
    std::function<void()> fail_callback_;

    std::mutex connect_mutex_;
    std::condition_variable connect_cv_;
};

// WebSocketClient 公共接口实现
WebSocketClient::WebSocketClient(const WebSocketConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

WebSocketClient::~WebSocketClient() = default;

bool WebSocketClient::connect(const std::string& url) {
    return impl_->connect(url);
}

void WebSocketClient::disconnect() {
    impl_->disconnect();
}

bool WebSocketClient::send(const std::string& message) {
    return impl_->send(message);
}

bool WebSocketClient::is_connected() const {
    return impl_->is_connected();
}

void WebSocketClient::set_message_callback(std::function<void(const std::string&)> callback) {
    impl_->set_message_callback(std::move(callback));
}

void WebSocketClient::set_close_callback(std::function<void()> callback) {
    impl_->set_close_callback(std::move(callback));
}

void WebSocketClient::set_fail_callback(std::function<void()> callback) {
    impl_->set_fail_callback(std::move(callback));
}

void WebSocketClient::safe_stop() {
    impl_->safe_stop();
}

void WebSocketClient::clear_callbacks() {
    impl_->clear_callbacks();
}

void WebSocketClient::set_proxy(const std::string& host, uint16_t port) {
    impl_->set_proxy(host, port);
}

const WebSocketConfig& WebSocketClient::get_config() const {
    return impl_->get_config();
}

} // namespace core
} // namespace trading

#else
// WebSocket++ 未启用时编译错误
#error "USE_WEBSOCKETPP must be defined to use WebSocketClient"
#endif
