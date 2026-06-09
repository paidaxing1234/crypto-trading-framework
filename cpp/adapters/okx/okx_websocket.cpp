/**
 * @file okx_websocket.cpp
 * @brief OKX WebSocket 客户端实现
 *
 * 使用公共 WebSocketClient 实现 WebSocket 连接
 *
 * @author Sequence Team
 * @date 2024-12
 */

#include "okx_websocket.h"
#include "../../network/ws_client.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <atomic>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace trading {
namespace okx {

// ==================== Debug 日志辅助函数 ====================

static std::mutex debug_log_mutex;
static std::ofstream debug_log_file;
static std::ofstream reconnect_log_file;  // 新增：重连专用日志文件
static bool debug_log_initialized = false;
static bool reconnect_log_initialized = false;  // 新增：重连日志初始化标志

static void init_debug_log() {
    if (!debug_log_initialized) {
        debug_log_file.open("/tmp/okx_websocket_debug.log", std::ios::app);
        if (debug_log_file.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            debug_log_file << "\n========================================\n";
            debug_log_file << "OKX WebSocket Debug Log Started at: "
                          << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S")
                          << "\n========================================\n" << std::flush;
            debug_log_initialized = true;
        }
    }
}

// 新增：初始化重连日志文件
static void init_reconnect_log() {
    if (!reconnect_log_initialized) {
        // 通过可执行文件路径推导logs目录
        std::string exe_dir = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path().string();
        std::string log_dir = exe_dir + "/logs";
        std::filesystem::create_directories(log_dir);

        reconnect_log_file.open(log_dir + "/okxchonglian.txt", std::ios::app);
        if (reconnect_log_file.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            reconnect_log_file << "\n========================================\n";
            reconnect_log_file << "OKX WebSocket 重连日志 Started at: "
                              << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S")
                              << "\n========================================\n" << std::flush;
            reconnect_log_initialized = true;
        }
    }
}

static void write_debug_log(const std::string& message) {
    std::lock_guard<std::mutex> lock(debug_log_mutex);
    init_debug_log();

    if (debug_log_file.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        debug_log_file << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S")
                      << "." << std::setfill('0') << std::setw(3) << ms.count()
                      << " " << message << std::endl;
    }

    // 同时输出到控制台
    std::cout << message << std::endl;
}

// 新增：写入重连日志（同时写入debug日志和重连专用日志）
static void write_reconnect_log(const std::string& message) {
    std::lock_guard<std::mutex> lock(debug_log_mutex);
    init_debug_log();
    init_reconnect_log();

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream timestamp;
    timestamp << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count();

    std::string log_line = timestamp.str() + " " + message;

    // 写入debug日志
    if (debug_log_file.is_open()) {
        debug_log_file << log_line << std::endl;
    }

    // 写入重连专用日志
    if (reconnect_log_file.is_open()) {
        reconnect_log_file << log_line << std::endl;
        reconnect_log_file.flush();  // 立即刷新，确保实时写入
    }

    // 同时输出到控制台
    std::cout << message << std::endl;
}

// ==================== Base64编码辅助函数 ====================

static std::string base64_encode(const unsigned char* buffer, size_t length) {
    static const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    
    std::string result;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];
    
    while (length--) {
        char_array_3[i++] = *(buffer++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for(i = 0; i < 4; i++)
                result += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    
    if (i) {
        for(j = i; j < 3; j++)
            char_array_3[j] = '\0';
        
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        
        for (j = 0; j < i + 1; j++)
            result += base64_chars[char_array_4[j]];
        
        while(i++ < 3)
            result += '=';
    }
    
    return result;
}

// ==================== JSON 安全转换辅助函数 ====================

static std::string safe_get_string(const nlohmann::json& obj, const std::string& key, const std::string& default_val = "") {
    if (!obj.contains(key)) return default_val;
    const auto& val = obj[key];
    if (val.is_string()) {
        return val.get<std::string>();
    } else if (val.is_number()) {
        return std::to_string(val.get<double>());
    }
    return default_val;
}

// ==================== OKXWebSocket 实现 ====================

OKXWebSocket::OKXWebSocket(
    const std::string& api_key,
    const std::string& secret_key,
    const std::string& passphrase,
    bool is_testnet,
    WsEndpointType endpoint_type,
    const core::WebSocketConfig& ws_config
)
    : api_key_(api_key)
    , secret_key_(secret_key)
    , passphrase_(passphrase)
    , is_testnet_(is_testnet)
    , endpoint_type_(endpoint_type)
    , ws_config_(ws_config)
    , impl_(std::make_shared<core::WebSocketClient>(ws_config))
{
    ws_url_ = build_ws_url();
}

OKXWebSocket::~OKXWebSocket() {
    disconnect();
}

std::string OKXWebSocket::build_ws_url() const {
    std::string base = is_testnet_ ? "wss://wspap.okx.com:8443" : "wss://ws.okx.com:8443";
    
    switch (endpoint_type_) {
        case WsEndpointType::PUBLIC:
            return base + "/ws/v5/public";
        case WsEndpointType::BUSINESS:
            return base + "/ws/v5/business";
        case WsEndpointType::PRIVATE:
            return base + "/ws/v5/private";
        default:
            return base + "/ws/v5/public";
    }
}

bool OKXWebSocket::connect() {
    if (is_connected_.load()) {
        std::cout << "[WebSocket] 已经连接" << std::endl;
        return true;
    }

    std::cout << "[WebSocket] 连接到: " << ws_url_ << std::endl;

    // 设置消息回调
    impl_->set_message_callback([this](const std::string& msg) {
        on_message(msg);
    });

    // 设置断开连接回调（标记需要重连）
    impl_->set_close_callback([this]() {
        try {
            // ★ DEBUG: 记录连接断开时间和原因
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);

            std::ostringstream oss;
            oss << "[OKX-DEBUG] ❌ WebSocket连接断开！时间: "
                << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S")
                << " | 连接状态: " << (is_connected_.load() ? "已连接" : "未连接")
                << " | 登录状态: " << (is_logged_in_.load() ? "已登录" : "未登录")
                << " | 重连启用: " << (reconnect_enabled_.load() ? "是" : "否");
            write_reconnect_log(oss.str());

            // 立即更新状态标志
            is_connected_.store(false);
            is_logged_in_.store(false);
            is_running_.store(false);  // 停止心跳线程，避免重连时join()阻塞

            // 检查并设置重连标志
            bool reconnect_enabled = reconnect_enabled_.load();
            write_reconnect_log(std::string("[OKX-DEBUG] 检查重连启用状态: ") + (reconnect_enabled ? "true" : "false"));

            if (reconnect_enabled) {
                need_reconnect_.store(true);
                write_reconnect_log("[OKX-DEBUG] ✓ 已设置 need_reconnect_ = true，等待监控线程处理");
                std::cout << "[OKXWebSocket] 连接断开，将由监控线程处理重连" << std::endl;
            } else {
                write_reconnect_log("[OKX-DEBUG] ✗ 自动重连已禁用，不设置重连标志");
            }

            write_reconnect_log("[OKX-DEBUG] close_callback 执行完成");
        } catch (const std::exception& e) {
            write_reconnect_log(std::string("[OKX-DEBUG] ❌ close_callback 异常: ") + e.what());
        } catch (...) {
            write_reconnect_log("[OKX-DEBUG] ❌ close_callback 未知异常");
        }
    });

    // 设置连接失败回调（标记需要重连）
    impl_->set_fail_callback([this]() {
        // ★ DEBUG: 记录连接失败时间
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << "[OKX-DEBUG] ❌ WebSocket连接失败！时间: "
            << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S");
        write_reconnect_log(oss.str());

        is_connected_.store(false);
        is_logged_in_.store(false);
        is_running_.store(false);  // 停止心跳线程，避免重连时join()阻塞
        if (reconnect_enabled_.load()) {
            need_reconnect_.store(true);
            std::cout << "[OKXWebSocket] 连接失败，将由监控线程处理重连" << std::endl;
        }
    });

    bool success = impl_->connect(ws_url_);
    is_connected_.store(success);
    is_running_.store(success);
    need_reconnect_.store(false);

    if (success) {
        // 启动心跳线程
        heartbeat_thread_ = std::make_unique<std::thread>([this]() {
            int sleep_counter = 0;
            while (is_running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                sleep_counter++;
                if (sleep_counter >= 150) {  // 150 × 100ms = 15秒心跳
                    sleep_counter = 0;
                    if (is_connected_.load()) {
                        send_ping();
                    }
                }
            }
            std::cout << "[WebSocket] 心跳线程已退出" << std::endl;
        });

        // 启动重连监控线程（独立线程，不在 websocketpp 回调中）
        if (!reconnect_monitor_thread_ && reconnect_enabled_.load()) {
            reconnect_monitor_thread_ = std::make_unique<std::thread>([this]() {
                write_reconnect_log("[OKX-DEBUG] 重连监控线程已启动");
                std::cout << "[OKXWebSocket] 重连监控线程已启动" << std::endl;

                int check_counter = 0;
                // 修复：只检查reconnect_enabled_，确保线程持续运行
                while (reconnect_enabled_.load()) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    check_counter++;

                    // 每分钟输出一次状态（60次 × 1秒 = 60秒）
                    if (check_counter % 60 == 0) {
                        std::ostringstream status_oss;
                        status_oss << "[OKX-DEBUG] 监控线程状态检查 - "
                                  << "is_running: " << is_running_.load()
                                  << ", is_connected: " << is_connected_.load()
                                  << ", reconnect_enabled: " << reconnect_enabled_.load()
                                  << ", need_reconnect: " << need_reconnect_.load();
                        write_debug_log(status_oss.str());
                    }

                    // 检查重连标志
                    bool need_reconnect = need_reconnect_.load();
                    if (!need_reconnect) {
                        continue;  // 没有重连需求，继续等待
                    }

                    // 开始重连流程
                    write_reconnect_log("[OKX-DEBUG] ✓ 检测到 need_reconnect_ = true，准备开始重连");
                    need_reconnect_.store(false);
                    write_reconnect_log("[OKX-DEBUG] 监控线程检测到 need_reconnect_ = true，开始重连...");
                    std::cout << "[OKXWebSocket] 监控线程检测到断开，开始重连..." << std::endl;

                        // ===== 安全重连：不主动调用 disconnect() =====
                        // websocketpp 在连接断开后可能还在进行内部清理
                        // 直接调用 disconnect() 可能导致 double free
                        // 让 connect() 方法自己处理旧连接的清理

                        // 1. ⭐ 先清除回调，防止在重连过程中触发旧回调
                        write_reconnect_log("[OKX-DEBUG] 步骤1: 清除旧回调");
                        impl_->clear_callbacks();

                        // 2. 不主动调用 disconnect()，避免 double free
                        // impl_->connect() 内部会安全地清理旧连接
                        write_reconnect_log("[OKX-DEBUG] 步骤2: 准备重新建立连接");
                        std::cout << "[OKXWebSocket] 准备重新建立连接..." << std::endl;

                        // 3. 等待底层 socket 完全释放 (TIME_WAIT)
                        write_reconnect_log("[OKX-DEBUG] 步骤3: 等待3秒让底层socket释放");
                        std::this_thread::sleep_for(std::chrono::seconds(3));

                        // 4. 重新设置回调
                        write_reconnect_log("[OKX-DEBUG] 步骤4: 重新设置回调函数");
                        impl_->set_message_callback([this](const std::string& msg) {
                            on_message(msg);
                        });

                        impl_->set_close_callback([this]() {
                            is_connected_.store(false);
                            is_logged_in_.store(false);
                            is_running_.store(false);  // 停止心跳线程
                            if (reconnect_enabled_.load()) {
                                need_reconnect_.store(true);
                                write_reconnect_log("[OKX-DEBUG] 连接再次断开，设置 need_reconnect_ = true");
                                std::cout << "[OKXWebSocket] 连接断开，将由监控线程处理重连" << std::endl;
                            }
                        });

                        impl_->set_fail_callback([this]() {
                            is_connected_.store(false);
                            is_logged_in_.store(false);
                            is_running_.store(false);  // 停止心跳线程
                            if (reconnect_enabled_.load()) {
                                need_reconnect_.store(true);
                                write_reconnect_log("[OKX-DEBUG] 连接失败，设置 need_reconnect_ = true");
                                std::cout << "[OKXWebSocket] 连接失败，将由监控线程处理重连" << std::endl;
                            }
                        });

                        // 5. 复用 impl_ 进行连接（ws_client.cpp 中会安全地清理旧连接）
                        write_reconnect_log("[OKX-DEBUG] 步骤5: 调用 impl_->connect() 尝试重连");
                        std::cout << "[OKXWebSocket] 尝试重新连接..." << std::endl;
                        if (impl_->connect(ws_url_)) {
                            is_connected_.store(true);
                            write_reconnect_log("[OKX-DEBUG] ✅ impl_->connect() 返回成功");
                            std::cout << "[OKXWebSocket] ✅ 重连成功" << std::endl;

                            // 重连成功，重置失败计数器
                            reconnect_fail_count_.store(0);
                            first_reconnect_fail_time_.store(0);
                            network_alert_sent_.store(false);

                            // 等待连接完全建立
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));

                            // ⭐ 重要修复：重连成功后重新启动心跳线程
                            // 先确保旧的心跳线程已经完全退出（在设置 is_running_ = true 之前）
                            if (heartbeat_thread_ && heartbeat_thread_->joinable()) {
                                heartbeat_thread_->join();
                                heartbeat_thread_.reset();
                            }

                            // 现在可以安全地设置 is_running_ = true 并启动新的心跳线程
                            is_running_.store(true);
                            // 启动新的心跳线程
                            heartbeat_thread_ = std::make_unique<std::thread>([this]() {
                                    int sleep_counter = 0;
                                    while (is_running_.load()) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                        sleep_counter++;
                                        if (sleep_counter >= 150) {  // 150 × 100ms = 15秒心跳
                                            sleep_counter = 0;
                                            if (is_connected_.load()) {
                                                send_ping();
                                            }
                                        }
                                    }
                                    std::cout << "[WebSocket] 心跳线程已退出" << std::endl;
                                });
                            write_reconnect_log("[OKX-DEBUG] ✅ 心跳线程已重新启动");
                            std::cout << "[OKXWebSocket] ✅ 心跳线程已重新启动" << std::endl;

                            // 等待连接完全建立（重要！）
                            write_reconnect_log("[OKX-DEBUG] 等待1秒让连接完全建立");
                            std::this_thread::sleep_for(std::chrono::seconds(1));

                            // 私有频道需要重新登录
                            if (endpoint_type_ == WsEndpointType::PRIVATE && !api_key_.empty()) {
                                write_reconnect_log("[OKX-DEBUG] 步骤6: 私有频道，开始重新登录");
                                login();
                                // 等待登录完成
                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            }

                            // 重新订阅
                            write_reconnect_log("[OKX-DEBUG] 步骤7: 开始重新订阅所有频道");
                            resubscribe_all();

                            // 等待订阅请求发送完成
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            write_reconnect_log("[OKX-DEBUG] ✅ 重连流程全部完成");
                            std::cout << "[OKXWebSocket] ✅ 重连流程完成，已重新订阅所有频道" << std::endl;
                        } else {
                            write_reconnect_log("[OKX-DEBUG] ❌ impl_->connect() 返回失败，设置 need_reconnect_ = true 稍后重试");
                            std::cerr << "[OKXWebSocket] ❌ 重连失败，稍后重试" << std::endl;
                            need_reconnect_.store(true);

                            // 追踪重连失败（不发送告警，由 trading_server 监控）
                            int fail_count = reconnect_fail_count_.fetch_add(1) + 1;
                            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();

                            // 记录第一次失败时间
                            if (first_reconnect_fail_time_.load() == 0) {
                                first_reconnect_fail_time_.store(now_ms);
                                std::cout << "[OKXWebSocket] 开始追踪重连失败，失败次数: " << fail_count << "\n";
                            }
                        }
                }
                write_reconnect_log("[OKX-DEBUG] 重连监控线程已退出");
                std::cout << "[OKXWebSocket] 重连监控线程已退出" << std::endl;
            });
        }
    }

    return success;
}

void OKXWebSocket::disconnect() {
    // 防止重复断开
    bool expected = false;
    if (!is_disconnected_.compare_exchange_strong(expected, true)) {
        // 已经断开过了，直接返回
        return;
    }

    // 禁用自动重连
    reconnect_enabled_.store(false);
    need_reconnect_.store(false);

    is_running_.store(false);
    is_connected_.store(false);

    // 等待监控线程退出
    if (reconnect_monitor_thread_ && reconnect_monitor_thread_->joinable()) {
        reconnect_monitor_thread_->join();
        reconnect_monitor_thread_.reset();
    }

    if (heartbeat_thread_ && heartbeat_thread_->joinable()) {
        heartbeat_thread_->join();
        heartbeat_thread_.reset();
    }

    if (impl_) {
        // ⭐ 先清除回调，防止断开过程中触发回调导致问题
        impl_->clear_callbacks();
        // 直接重置 impl_，这会触发其析构函数的 shutdown()
        // 这样在 OKXWebSocket 析构时就不会再有 impl_ 需要处理
        impl_.reset();
        // ⭐ 等待一小段时间，确保 WebSocketClient 完全销毁
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[WebSocket] 已断开连接" << std::endl;
}

void OKXWebSocket::set_auto_reconnect(bool enabled) {
    reconnect_enabled_.store(enabled);
    if (!enabled) {
        need_reconnect_.store(false);
    }
}

void OKXWebSocket::resubscribe_all() {
    // 第一步：在锁内收集订阅数据（避免死锁）
    std::vector<std::pair<std::string, std::vector<std::string>>> channels_to_subscribe;

    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        if (subscriptions_.empty()) {
            write_debug_log("[OKX-DEBUG] resubscribe_all: 没有需要重新订阅的频道");
            std::cout << "[WebSocket] 没有需要重新订阅的频道" << std::endl;
            return;
        }

        std::ostringstream oss;
        oss << "[OKX-DEBUG] resubscribe_all: 开始重新订阅 " << subscriptions_.size() << " 个频道";
        write_debug_log(oss.str());
        std::cout << "[WebSocket] 重新订阅 " << subscriptions_.size() << " 个频道..." << std::endl;

        // 收集所有订阅信息，按频道分组
        std::unordered_map<std::string, std::vector<std::string>> channel_symbols;

        for (const auto& [key, value] : subscriptions_) {
            // key 格式: "channel:instId" 或 "channel:instType:instId"
            size_t pos = key.find(':');
            if (pos != std::string::npos) {
                std::string channel = key.substr(0, pos);
                std::string rest = key.substr(pos + 1);
                channel_symbols[channel].push_back(rest);
            }
        }

        // 将数据复制到临时容器
        for (const auto& [channel, symbols] : channel_symbols) {
            channels_to_subscribe.push_back({channel, symbols});
        }
    }  // 释放锁

    // 第二步：在锁外执行订阅（这些方法会获取锁，避免死锁）
    for (const auto& [channel, symbols] : channels_to_subscribe) {
        if (channel.find("candle") != std::string::npos) {
            // K线频道
            std::string bar = channel.substr(6);  // 去掉 "candle" 前缀
            std::vector<std::string> inst_ids;
            for (const auto& s : symbols) {
                inst_ids.push_back(s);
            }
            if (!inst_ids.empty()) {
                subscribe_klines_batch(inst_ids, bar);
            }
        } else if (channel == "tickers") {
            std::vector<std::string> inst_ids;
            for (const auto& s : symbols) {
                inst_ids.push_back(s);
            }
            if (!inst_ids.empty()) {
                subscribe_tickers_batch(inst_ids);
            }
        } else if (channel == "trades") {
            std::vector<std::string> inst_ids;
            for (const auto& s : symbols) {
                inst_ids.push_back(s);
            }
            if (!inst_ids.empty()) {
                subscribe_trades_batch(inst_ids);
            }
        } else if (channel.find("books") != std::string::npos || channel == "bbo-tbt") {
            std::vector<std::string> inst_ids;
            for (const auto& s : symbols) {
                inst_ids.push_back(s);
            }
            if (!inst_ids.empty()) {
                subscribe_orderbooks_batch(inst_ids, channel);
            }
        } else {
            // 其他频道逐个订阅
            for (const auto& s : symbols) {
                send_subscribe(channel, s);
            }
        }
    }

    write_debug_log("[OKX-DEBUG] resubscribe_all: 所有订阅请求已发送");
}

void OKXWebSocket::login() {
    if (api_key_.empty() || secret_key_.empty() || passphrase_.empty()) {
        std::cerr << "[WebSocket] 登录需要提供 api_key, secret_key, passphrase" << std::endl;
        return;
    }

    std::string timestamp = get_timestamp();
    std::string sign = create_signature(timestamp);

    nlohmann::json login_msg = {
        {"op", "login"},
        {"args", {{
            {"apiKey", api_key_},
            {"passphrase", passphrase_},
            {"timestamp", timestamp},
            {"sign", sign}
        }}}
    };

    std::cout << "[WebSocket] 发送登录请求..." << std::endl;
    send_message(login_msg);
}

bool OKXWebSocket::wait_for_login(int timeout_ms) {
    std::unique_lock<std::mutex> lock(login_mutex_);
    if (is_logged_in_.load()) {
        return true;
    }
    return login_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
        return is_logged_in_.load();
    });
}

std::string OKXWebSocket::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()
    ).count();
    return std::to_string(seconds);
}

std::string OKXWebSocket::create_signature(const std::string& timestamp) {
    std::string message = timestamp + "GET" + "/users/self/verify";
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    HMAC(
        EVP_sha256(),
        secret_key_.c_str(),
        secret_key_.length(),
        (unsigned char*)message.c_str(),
        message.length(),
        hash,
        nullptr
    );
    
    return base64_encode(hash, SHA256_DIGEST_LENGTH);
}

bool OKXWebSocket::send_message(const nlohmann::json& msg) {
    if (!is_connected_.load()) return false;
    return impl_->send(msg.dump());
}

void OKXWebSocket::send_ping() {
    // ★ DEBUG: 记录心跳发送
    static auto last_ping_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_ping_time).count();

    std::ostringstream oss;
    oss << "[OKX-DEBUG] 发送 ping 心跳 (距上次: " << elapsed << "秒, 连接状态: "
        << (is_connected_.load() ? "已连接" : "未连接") << ")";
    write_debug_log(oss.str());

    last_ping_time = now;

    impl_->send("ping");
}

// ==================== 订阅方法 ====================

void OKXWebSocket::send_subscribe(const std::string& channel, const std::string& inst_id,
                                  const std::string& extra_key, const std::string& extra_value) {
    nlohmann::json arg = {{"channel", channel}};
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!extra_key.empty() && !extra_value.empty()) {
        arg[extra_key] = extra_value;
    }
    
    nlohmann::json msg = {
        {"op", "subscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 订阅: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = channel + ":" + inst_id;
        subscriptions_[key] = inst_id;
    }
}

void OKXWebSocket::send_unsubscribe(const std::string& channel, const std::string& inst_id,
                                    const std::string& extra_key, const std::string& extra_value) {
    nlohmann::json arg = {{"channel", channel}};
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!extra_key.empty() && !extra_value.empty()) {
        arg[extra_key] = extra_value;
    }
    
    nlohmann::json msg = {
        {"op", "unsubscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 取消订阅: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = channel + ":" + inst_id;
        subscriptions_.erase(key);
    }
}

// 公共频道
void OKXWebSocket::subscribe_ticker(const std::string& inst_id) {
    send_subscribe("tickers", inst_id);
}

void OKXWebSocket::subscribe_tickers_by_type(const std::string& inst_type) {
    // 订阅全市场行情，使用 instType 参数
    send_subscribe("tickers", "", "instType", inst_type);
}

void OKXWebSocket::unsubscribe_ticker(const std::string& inst_id) {
    send_unsubscribe("tickers", inst_id);
}

void OKXWebSocket::subscribe_trades(const std::string& inst_id) {
    send_subscribe("trades", inst_id);
}

void OKXWebSocket::unsubscribe_trades(const std::string& inst_id) {
    send_unsubscribe("trades", inst_id);
}

void OKXWebSocket::subscribe_orderbook(const std::string& inst_id, const std::string& channel) {
    send_subscribe(channel, inst_id);
}

void OKXWebSocket::unsubscribe_orderbook(const std::string& inst_id, const std::string& channel) {
    send_unsubscribe(channel, inst_id);
}

// K线频道
void OKXWebSocket::subscribe_kline(const std::string& inst_id, KlineInterval interval) {
    std::string channel = kline_interval_to_channel(interval);
    send_subscribe(channel, inst_id);
}

void OKXWebSocket::subscribe_kline(const std::string& inst_id, const std::string& bar) {
    KlineInterval interval = string_to_kline_interval(bar);
    subscribe_kline(inst_id, interval);
}

void OKXWebSocket::unsubscribe_kline(const std::string& inst_id, KlineInterval interval) {
    std::string channel = kline_interval_to_channel(interval);
    send_unsubscribe(channel, inst_id);
}

void OKXWebSocket::unsubscribe_kline(const std::string& inst_id, const std::string& bar) {
    KlineInterval interval = string_to_kline_interval(bar);
    unsubscribe_kline(inst_id, interval);
}

// 批量订阅方法
void OKXWebSocket::subscribe_klines_batch(const std::vector<std::string>& inst_ids, const std::string& bar) {
    if (inst_ids.empty()) return;

    std::string channel = "candle" + bar;
    nlohmann::json args = nlohmann::json::array();

    for (const auto& inst_id : inst_ids) {
        args.push_back({{"channel", channel}, {"instId", inst_id}});
    }

    nlohmann::json msg = {{"op", "subscribe"}, {"args", args}};
    std::cout << "[WebSocket] 批量订阅K线: " << inst_ids.size() << " 个币种, 周期=" << bar << std::endl;

    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        for (const auto& inst_id : inst_ids) {
            subscriptions_[channel + ":" + inst_id] = inst_id;
        }
    }
}

void OKXWebSocket::subscribe_tickers_batch(const std::vector<std::string>& inst_ids) {
    if (inst_ids.empty()) return;

    nlohmann::json args = nlohmann::json::array();
    for (const auto& inst_id : inst_ids) {
        args.push_back({{"channel", "tickers"}, {"instId", inst_id}});
    }

    nlohmann::json msg = {{"op", "subscribe"}, {"args", args}};
    std::cout << "[WebSocket] 批量订阅Ticker: " << inst_ids.size() << " 个币种" << std::endl;

    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        for (const auto& inst_id : inst_ids) {
            subscriptions_["tickers:" + inst_id] = inst_id;
        }
    }
}

void OKXWebSocket::subscribe_trades_batch(const std::vector<std::string>& inst_ids) {
    if (inst_ids.empty()) return;

    nlohmann::json args = nlohmann::json::array();
    for (const auto& inst_id : inst_ids) {
        args.push_back({{"channel", "trades"}, {"instId", inst_id}});
    }

    nlohmann::json msg = {{"op", "subscribe"}, {"args", args}};
    std::cout << "[WebSocket] 批量订阅Trades: " << inst_ids.size() << " 个币种" << std::endl;

    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        for (const auto& inst_id : inst_ids) {
            subscriptions_["trades:" + inst_id] = inst_id;
        }
    }
}

void OKXWebSocket::subscribe_orderbooks_batch(const std::vector<std::string>& inst_ids, const std::string& channel) {
    if (inst_ids.empty()) return;

    nlohmann::json args = nlohmann::json::array();
    for (const auto& inst_id : inst_ids) {
        args.push_back({{"channel", channel}, {"instId", inst_id}});
    }

    nlohmann::json msg = {{"op", "subscribe"}, {"args", args}};
    std::cout << "[WebSocket] 批量订阅深度(" << channel << "): " << inst_ids.size() << " 个币种" << std::endl;

    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        for (const auto& inst_id : inst_ids) {
            subscriptions_[channel + ":" + inst_id] = inst_id;
        }
    }
}

void OKXWebSocket::subscribe_trades_all(const std::string& inst_id) {
    send_subscribe("trades-all", inst_id);
}

void OKXWebSocket::unsubscribe_trades_all(const std::string& inst_id) {
    send_unsubscribe("trades-all", inst_id);
}

// 持仓总量频道
void OKXWebSocket::subscribe_open_interest(const std::string& inst_id) {
    send_subscribe("open-interest", inst_id);
}

void OKXWebSocket::unsubscribe_open_interest(const std::string& inst_id) {
    send_unsubscribe("open-interest", inst_id);
}

// 标记价格频道
void OKXWebSocket::subscribe_mark_price(const std::string& inst_id) {
    send_subscribe("mark-price", inst_id);
}

void OKXWebSocket::unsubscribe_mark_price(const std::string& inst_id) {
    send_unsubscribe("mark-price", inst_id);
}

void OKXWebSocket::subscribe_funding_rate(const std::string& inst_id) {
    send_subscribe("funding-rate", inst_id);
}

void OKXWebSocket::unsubscribe_funding_rate(const std::string& inst_id) {
    send_unsubscribe("funding-rate", inst_id);
}

// ==================== 私有频道 ====================

void OKXWebSocket::subscribe_orders(
    const std::string& inst_type,
    const std::string& inst_id,
    const std::string& inst_family
) {
    nlohmann::json arg = {
        {"channel", "orders"},
        {"instType", inst_type}
    };
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!inst_family.empty()) {
        arg["instFamily"] = inst_family;
    }
    
    nlohmann::json msg = {
        {"op", "subscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 订阅订单频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "orders:" + inst_type;
        if (!inst_id.empty()) key += ":" + inst_id;
        subscriptions_[key] = inst_type;
    }
}

void OKXWebSocket::unsubscribe_orders(
    const std::string& inst_type,
    const std::string& inst_id,
    const std::string& inst_family
) {
    nlohmann::json arg = {
        {"channel", "orders"},
        {"instType", inst_type}
    };
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!inst_family.empty()) {
        arg["instFamily"] = inst_family;
    }
    
    nlohmann::json msg = {
        {"op", "unsubscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 取消订阅订单频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "orders:" + inst_type;
        if (!inst_id.empty()) key += ":" + inst_id;
        subscriptions_.erase(key);
    }
}

void OKXWebSocket::subscribe_positions(
    const std::string& inst_type,
    const std::string& inst_id,
    const std::string& inst_family,
    int update_interval
) {
    nlohmann::json arg = {
        {"channel", "positions"},
        {"instType", inst_type}
    };
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!inst_family.empty()) {
        arg["instFamily"] = inst_family;
    }
    
    // 如果指定了 update_interval，添加 extraParams
    if (update_interval >= 0) {
        nlohmann::json extra_params = {
            {"updateInterval", std::to_string(update_interval)}
        };
        arg["extraParams"] = extra_params.dump();
    }
    
    nlohmann::json msg = {
        {"op", "subscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 订阅持仓频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "positions:" + inst_type;
        if (!inst_id.empty()) key += ":" + inst_id;
        if (!inst_family.empty()) key += ":" + inst_family;
        subscriptions_[key] = inst_type;
    }
}

void OKXWebSocket::unsubscribe_positions(
    const std::string& inst_type,
    const std::string& inst_id,
    const std::string& inst_family
) {
    nlohmann::json arg = {
        {"channel", "positions"},
        {"instType", inst_type}
    };
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!inst_family.empty()) {
        arg["instFamily"] = inst_family;
    }
    
    nlohmann::json msg = {
        {"op", "unsubscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 取消订阅持仓频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "positions:" + inst_type;
        if (!inst_id.empty()) key += ":" + inst_id;
        subscriptions_.erase(key);
    }
}

void OKXWebSocket::subscribe_account(const std::string& ccy, int update_interval) {
    nlohmann::json arg = {
        {"channel", "account"}
    };
    
    if (!ccy.empty()) {
        arg["ccy"] = ccy;
    }
    
    // 如果指定了 update_interval，添加 extraParams
    if (update_interval == 0) {
        nlohmann::json extra_params = {
            {"updateInterval", "0"}
        };
        arg["extraParams"] = extra_params.dump();
    }
    
    nlohmann::json msg = {
        {"op", "subscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 订阅账户频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "account";
        if (!ccy.empty()) key += ":" + ccy;
        subscriptions_[key] = ccy.empty() ? "all" : ccy;
    }
}

void OKXWebSocket::unsubscribe_account(const std::string& ccy) {
    if (ccy.empty()) {
        nlohmann::json msg = {
            {"op", "unsubscribe"},
            {"args", {{{"channel", "account"}}}}
        };
        send_message(msg);
    } else {
        send_unsubscribe("account", "", "ccy", ccy);
    }
}

// 账户余额和持仓频道
void OKXWebSocket::subscribe_balance_and_position() {
    nlohmann::json arg = {
        {"channel", "balance_and_position"}
    };
    
    nlohmann::json msg = {
        {"op", "subscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 订阅账户余额和持仓频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        subscriptions_["balance_and_position"] = "all";
    }
}

void OKXWebSocket::unsubscribe_balance_and_position() {
    nlohmann::json msg = {
        {"op", "unsubscribe"},
        {"args", {{{"channel", "balance_and_position"}}}}
    };
    
    std::cout << "[WebSocket] 取消订阅账户余额和持仓频道" << std::endl;
    send_message(msg);
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_.erase("balance_and_position");
}

// Spread订单频道
void OKXWebSocket::subscribe_sprd_orders(const std::string& sprd_id) {
    nlohmann::json arg = {
        {"channel", "sprd-orders"}
    };
    
    if (!sprd_id.empty()) {
        arg["sprdId"] = sprd_id;
    }
    
    nlohmann::json msg = {
        {"op", "subscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 订阅Spread订单频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "sprd-orders";
        if (!sprd_id.empty()) key += ":" + sprd_id;
        subscriptions_[key] = sprd_id.empty() ? "all" : sprd_id;
    }
}

void OKXWebSocket::unsubscribe_sprd_orders(const std::string& sprd_id) {
    nlohmann::json arg = {
        {"channel", "sprd-orders"}
    };
    
    if (!sprd_id.empty()) {
        arg["sprdId"] = sprd_id;
    }
    
    nlohmann::json msg = {
        {"op", "unsubscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 取消订阅Spread订单频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "sprd-orders";
        if (!sprd_id.empty()) key += ":" + sprd_id;
        subscriptions_.erase(key);
    }
}

// Spread成交数据频道
void OKXWebSocket::subscribe_sprd_trades(const std::string& sprd_id) {
    nlohmann::json arg = {
        {"channel", "sprd-trades"}
    };
    
    if (!sprd_id.empty()) {
        arg["sprdId"] = sprd_id;
    }
    
    nlohmann::json msg = {
        {"op", "subscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 订阅Spread成交数据频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "sprd-trades";
        if (!sprd_id.empty()) key += ":" + sprd_id;
        subscriptions_[key] = sprd_id.empty() ? "all" : sprd_id;
    }
}

void OKXWebSocket::unsubscribe_sprd_trades(const std::string& sprd_id) {
    nlohmann::json arg = {
        {"channel", "sprd-trades"}
    };
    
    if (!sprd_id.empty()) {
        arg["sprdId"] = sprd_id;
    }
    
    nlohmann::json msg = {
        {"op", "unsubscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 取消订阅Spread成交数据频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "sprd-trades";
        if (!sprd_id.empty()) key += ":" + sprd_id;
        subscriptions_.erase(key);
    }
}

// ==================== 策略委托订单频道 ====================

void OKXWebSocket::subscribe_orders_algo(
    const std::string& inst_type,
    const std::string& inst_id,
    const std::string& inst_family
) {
    nlohmann::json arg = {
        {"channel", "orders-algo"},
        {"instType", inst_type}
    };
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!inst_family.empty()) {
        arg["instFamily"] = inst_family;
    }
    
    nlohmann::json msg = {
        {"op", "subscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 订阅策略委托订单频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "orders-algo:" + inst_type;
        if (!inst_id.empty()) key += ":" + inst_id;
        if (!inst_family.empty()) key += ":" + inst_family;
        subscriptions_[key] = inst_type;
    }
}

void OKXWebSocket::unsubscribe_orders_algo(
    const std::string& inst_type,
    const std::string& inst_id,
    const std::string& inst_family
) {
    nlohmann::json arg = {
        {"channel", "orders-algo"},
        {"instType", inst_type}
    };
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!inst_family.empty()) {
        arg["instFamily"] = inst_family;
    }
    
    nlohmann::json msg = {
        {"op", "unsubscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 取消订阅策略委托订单频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "orders-algo:" + inst_type;
        if (!inst_id.empty()) key += ":" + inst_id;
        if (!inst_family.empty()) key += ":" + inst_family;
        subscriptions_.erase(key);
    }
}

// ==================== 高级策略委托订单频道 ====================

void OKXWebSocket::subscribe_algo_advance(
    const std::string& inst_type,
    const std::string& inst_id,
    const std::string& algo_id
) {
    nlohmann::json arg = {
        {"channel", "algo-advance"},
        {"instType", inst_type}
    };
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!algo_id.empty()) {
        arg["algoId"] = algo_id;
    }
    
    nlohmann::json msg = {
        {"op", "subscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 订阅高级策略委托订单频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "algo-advance:" + inst_type;
        if (!inst_id.empty()) key += ":" + inst_id;
        if (!algo_id.empty()) key += ":" + algo_id;
        subscriptions_[key] = inst_type;
    }
}

void OKXWebSocket::unsubscribe_algo_advance(
    const std::string& inst_type,
    const std::string& inst_id,
    const std::string& algo_id
) {
    nlohmann::json arg = {
        {"channel", "algo-advance"},
        {"instType", inst_type}
    };
    
    if (!inst_id.empty()) {
        arg["instId"] = inst_id;
    }
    if (!algo_id.empty()) {
        arg["algoId"] = algo_id;
    }
    
    nlohmann::json msg = {
        {"op", "unsubscribe"},
        {"args", {arg}}
    };
    
    std::cout << "[WebSocket] 取消订阅高级策略委托订单频道: " << msg.dump() << std::endl;
    
    if (send_message(msg)) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::string key = "algo-advance:" + inst_type;
        if (!inst_id.empty()) key += ":" + inst_id;
        if (!algo_id.empty()) key += ":" + algo_id;
        subscriptions_.erase(key);
    }
}

// ==================== WebSocket下单实现 ====================

std::string OKXWebSocket::place_order_ws(
    const std::string& inst_id,
    const std::string& td_mode,
    const std::string& side,
    const std::string& ord_type,
    const std::string& sz,
    const std::string& px,
    const std::string& ccy,
    const std::string& cl_ord_id,
    const std::string& tag,
    const std::string& pos_side,
    bool reduce_only,
    const std::string& tgt_ccy,
    bool ban_amend,
    const std::string& request_id
) {
    // 生成请求ID
    std::string req_id = request_id;
    if (req_id.empty()) {
        req_id = std::to_string(request_id_counter_.fetch_add(1));
    }
    
    // 构建订单参数
    nlohmann::json order_arg = {
        {"instId", inst_id},
        {"tdMode", td_mode},
        {"side", side},
        {"ordType", ord_type},
        {"sz", sz}
    };
    
    // 添加可选参数
    if (!px.empty()) {
        order_arg["px"] = px;
    }
    
    if (!ccy.empty()) {
        order_arg["ccy"] = ccy;
    }
    
    if (!cl_ord_id.empty()) {
        order_arg["clOrdId"] = cl_ord_id;
    }
    
    if (!tag.empty()) {
        order_arg["tag"] = tag;
    }
    
    if (!pos_side.empty()) {
        order_arg["posSide"] = pos_side;
    }
    
    if (reduce_only) {
        order_arg["reduceOnly"] = true;
    }
    
    if (!tgt_ccy.empty()) {
        order_arg["tgtCcy"] = tgt_ccy;
    }
    
    if (ban_amend) {
        order_arg["banAmend"] = true;
    }
    
    // 构建完整的WebSocket消息
    nlohmann::json msg = {
        {"id", req_id},
        {"op", "order"},
        {"args", {order_arg}}
    };
    
    std::cout << "[WebSocket] 发送下单请求 (ID=" << req_id << "): " << msg.dump() << std::endl;
    
    if (!send_message(msg)) {
        std::cerr << "[WebSocket] ❌ 发送下单请求失败" << std::endl;
        return "";
    }
    
    return req_id;
}

std::string OKXWebSocket::place_batch_orders_ws(
    const std::vector<nlohmann::json>& orders,
    const std::string& request_id
) {
    if (orders.empty()) {
        std::cerr << "[WebSocket] ❌ 批量下单参数为空" << std::endl;
        return "";
    }
    
    if (orders.size() > 20) {
        std::cerr << "[WebSocket] ❌ 批量下单最多支持20笔订单，当前: " << orders.size() << std::endl;
        return "";
    }
    
    // 生成请求ID
    std::string req_id = request_id;
    if (req_id.empty()) {
        req_id = std::to_string(request_id_counter_.fetch_add(1));
    }
    
    // 构建完整的WebSocket消息
    nlohmann::json msg = {
        {"id", req_id},
        {"op", "batch-orders"},
        {"args", orders}
    };
    
    std::cout << "[WebSocket] 发送批量下单请求 (ID=" << req_id << "): " 
              << orders.size() << " 笔订单" << std::endl;
    
    if (!send_message(msg)) {
        std::cerr << "[WebSocket] ❌ 发送批量下单请求失败" << std::endl;
        return "";
    }
    
    return req_id;
}

std::vector<std::string> OKXWebSocket::get_subscribed_channels() const {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    std::vector<std::string> result;
    for (const auto& pair : subscriptions_) {
        result.push_back(pair.first);
    }
    return result;
}

// ==================== 消息处理 ====================

void OKXWebSocket::on_message(const std::string& message) {
    // 处理pong响应
    if (message == "pong") {
        // ★ DEBUG: 记录收到 pong
        static auto last_pong_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_pong_time).count();

        std::ostringstream oss;
        oss << "[OKX-DEBUG] 收到 pong 响应 (距上次: " << elapsed << "秒)";
        write_debug_log(oss.str());

        last_pong_time = now;
        return;
    }

    try {
        nlohmann::json data = nlohmann::json::parse(message);
        
        // 调用原始消息回调
        if (raw_callback_) {
            raw_callback_(data);
        }
        
        // 数据推送日志已关闭
        // if (data.contains("data") && data.contains("arg")) { }
        
        // 处理下单响应（包含id和op字段）
        if (data.contains("id") && data.contains("op")) {
            std::string op = data["op"];
            std::string id = data["id"];
            std::string code = data.value("code", "");
            std::string msg = data.value("msg", "");
            
            if (op == "order" || op == "batch-orders") {
                // 打印下单响应信息
                if (code == "0") {
                    std::cout << "[WebSocket] ✅ 下单成功 (ID=" << id << ")";
                    if (data.contains("data") && !data["data"].empty()) {
                        std::cout << ", 订单数: " << data["data"].size();
                        for (const auto& order : data["data"]) {
                            std::string ord_id = order.value("ordId", "");
                            std::string s_code = order.value("sCode", "");
                            if (!ord_id.empty()) {
                                std::cout << ", ordId=" << ord_id;
                            }
                            if (s_code != "0") {
                                std::string s_msg = order.value("sMsg", "");
                                std::cout << ", 错误: " << s_msg << " (sCode=" << s_code << ")";
                            }
                        }
                    }
                    std::cout << std::endl;
                } else {
                    std::cerr << "[WebSocket] ❌ 下单失败 (ID=" << id << "): " 
                              << msg << " (code=" << code << ")" << std::endl;
                }
                
                // 调用下单回调
                if (place_order_callback_) {
                    place_order_callback_(data);
                }
                return;
            }
        }
        
        // 处理事件消息（订阅响应/错误）
        if (data.contains("event")) {
            std::string event = data["event"];
            
            if (event == "subscribe") {
                std::cout << "[WebSocket] ✅ 订阅成功: " << data["arg"].dump() << std::endl;
            } else if (event == "unsubscribe") {
                std::cout << "[WebSocket] ✅ 取消订阅成功: " << data["arg"].dump() << std::endl;
            } else if (event == "login") {
                if (data.value("code", "") == "0") {
                    is_logged_in_.store(true);
                    login_cv_.notify_all();  // 通知等待登录的线程
                    std::cout << "[WebSocket] ✅ 登录成功" << std::endl;
                    if (login_callback_) {
                        login_callback_(true, "");
                    }
                } else {
                    std::string msg = data.value("msg", "");
                    std::cerr << "[WebSocket] ❌ 登录失败: " << msg << std::endl;
                    login_cv_.notify_all();  // 登录失败也要通知
                    if (login_callback_) {
                        login_callback_(false, msg);
                    }
                }
            } else if (event == "error") {
                std::cerr << "[WebSocket] ❌ 错误: " << safe_get_string(data, "msg")
                          << " (code: " << safe_get_string(data, "code") << ")" << std::endl;
            }
            return;
        }
        
        // 处理数据推送
        if (data.contains("arg") && data.contains("data")) {
            const auto& arg = data["arg"];
            std::string channel = safe_get_string(arg, "channel");
            std::string inst_id = safe_get_string(arg, "instId");
            
            // 收到数据推送（日志已关闭）
            // std::cout << "[WebSocket] 收到数据推送 - 频道: " << channel;
            
            // 根据频道类型解析数据
            if (channel == "tickers") {
                parse_ticker(data["data"], inst_id);
            } else if (channel == "trades" || channel == "trades-all") {
                parse_trade(data["data"], inst_id);
            } else if (channel.find("books") != std::string::npos || channel == "bbo-tbt") {
                // 深度频道：books, books5, bbo-tbt, books-l2-tbt, books50-l2-tbt, books-elp
                std::string action = data.value("action", "snapshot");  // snapshot 或 update
                parse_orderbook(data["data"], inst_id, channel, action);
            } else if (channel.find("candle") != std::string::npos) {
                parse_kline(data["data"], inst_id, channel);
            } else if (channel == "orders") {
                parse_order(data["data"]);
            } else if (channel == "positions") {
                parse_position(data["data"]);
            } else if (channel == "account") {
                parse_account(data["data"]);
            } else if (channel == "balance_and_position") {
                parse_balance_and_position(data["data"]);
            } else if (channel == "open-interest") {
                parse_open_interest(data["data"]);
            } else if (channel == "mark-price") {
                parse_mark_price(data["data"]);
            } else if (channel == "funding-rate") {
                parse_funding_rate(data["data"]);
            } else if (channel == "sprd-orders") {
                parse_sprd_order(data["data"]);
            } else if (channel == "sprd-trades") {
                parse_sprd_trade(data["data"]);
            } else {
                std::cout << "[WebSocket] ⚠️ 未识别的频道: " << channel << std::endl;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[WebSocket] 解析消息失败: " << e.what() << std::endl;
    }
}

void OKXWebSocket::parse_ticker(const nlohmann::json& data, const std::string& inst_id) {
    if (!ticker_callback_ || !data.is_array() || data.empty()) return;

    for (const auto& item : data) {
        try {
            // 直接传递原始JSON，添加exchange和symbol信息
            nlohmann::json raw_data = item;
            raw_data["exchange"] = "okx";
            raw_data["symbol"] = inst_id;

            ticker_callback_(raw_data);

        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] 解析Ticker失败: " << e.what() << std::endl;
        }
    }
}

void OKXWebSocket::parse_trade(const nlohmann::json& data, const std::string& inst_id) {
    if (!trade_callback_ || !data.is_array() || data.empty()) return;

    for (const auto& item : data) {
        try {
            // 直接传递原始JSON，添加exchange和symbol信息
            nlohmann::json raw_data = item;
            raw_data["exchange"] = "okx";
            raw_data["symbol"] = inst_id;

            trade_callback_(raw_data);

        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] 解析Trade失败: " << e.what() << std::endl;
        }
    }
}

void OKXWebSocket::parse_orderbook(const nlohmann::json& data, const std::string& inst_id,
                                    const std::string& channel, const std::string& action) {
    if (!orderbook_callback_ || !data.is_array() || data.empty()) return;

    try {
        const auto& item = data[0];

        // 直接传递原始JSON，添加exchange、symbol和action信息
        nlohmann::json raw_data = item;
        raw_data["exchange"] = "okx";
        raw_data["symbol"] = inst_id;
        raw_data["channel"] = channel;
        raw_data["action"] = action;

        orderbook_callback_(raw_data);

    } catch (const std::exception& e) {
        std::cerr << "[WebSocket] 解析OrderBook失败: " << e.what() << std::endl;
    }
}

void OKXWebSocket::parse_kline(const nlohmann::json& data, const std::string& inst_id, const std::string& channel) {
    if (!kline_callback_ || !data.is_array() || data.empty()) return;

    // 从channel提取interval（如 "candle1m" -> "1m"）
    std::string interval = channel.substr(6);  // 去掉 "candle" 前缀

    // ★ DEBUG: 记录每个币种的K线接收情况
    static std::map<std::string, uint64_t> kline_count_per_symbol;
    static std::map<std::string, std::chrono::steady_clock::time_point> last_kline_time_per_symbol;

    for (const auto& item : data) {
        try {
            // OKX K线数据格式: [ts, o, h, l, c, vol, volCcy, volCcyQuote, confirm]
            if (!item.is_array() || item.size() < 6) continue;

            // 构造原始JSON数据
            nlohmann::json raw_data;
            raw_data["exchange"] = "okx";
            raw_data["symbol"] = inst_id;
            raw_data["interval"] = interval;
            raw_data["ts"] = item[0];
            raw_data["o"] = item[1];
            raw_data["h"] = item[2];
            raw_data["l"] = item[3];
            raw_data["c"] = item[4];
            raw_data["vol"] = item[5];
            if (item.size() > 6) raw_data["volCcy"] = item[6];
            if (item.size() > 7) raw_data["volCcyQuote"] = item[7];
            if (item.size() > 8) raw_data["confirm"] = item[8];

            // ★ DEBUG: 统计每个币种的K线接收
            std::string key = inst_id + ":" + interval;
            kline_count_per_symbol[key]++;
            last_kline_time_per_symbol[key] = std::chrono::steady_clock::now();

            kline_callback_(raw_data);

        } catch (const std::exception& e) {
            // 静默忽略解析错误，避免污染日志
            // std::cerr << "[WebSocket] 解析Kline失败: " << e.what() << std::endl;
        }
    }
}

// 辅助函数：安全地解析可能为空字符串的数字字段
namespace {
    double safe_stod(const nlohmann::json& item, const std::string& key, double default_value = 0.0) {
        if (!item.contains(key)) {
            return default_value;
        }
        
        std::string value_str = item[key].get<std::string>();
        if (value_str.empty()) {
            return default_value;
        }
        
        try {
            return std::stod(value_str);
        } catch (const std::exception&) {
            return default_value;
        }
    }
    
    int64_t safe_stoll(const nlohmann::json& item, const std::string& key, int64_t default_value = 0) {
        if (!item.contains(key)) {
            return default_value;
        }
        
        std::string value_str = item[key].get<std::string>();
        if (value_str.empty()) {
            return default_value;
        }
        
        try {
            return std::stoll(value_str);
        } catch (const std::exception&) {
            return default_value;
        }
    }
}

void OKXWebSocket::parse_order(const nlohmann::json& data) {
    // 调试日志
    if (!order_callback_) {
        std::cerr << "[WebSocket] ⚠️ 订单回调未设置！" << std::endl;
        return;
    }
    
    if (!data.is_array()) {
        std::cerr << "[WebSocket] ⚠️ 订单数据不是数组格式: " << data.dump() << std::endl;
        return;
    }
    
    if (data.empty()) {
        std::cout << "[WebSocket] ⚠️ 订单数据为空数组" << std::endl;
        return;
    }
    
    // std::cout << "[WebSocket] 开始解析订单数据，共 " << data.size() << " 条" << std::endl;
    
    for (const auto& item : data) {
        try {
            // 解析订单类型
            OrderType order_type = OrderType::LIMIT;
            std::string ord_type = item.value("ordType", "limit");
            if (ord_type == "market") order_type = OrderType::MARKET;
            else if (ord_type == "post_only") order_type = OrderType::POST_ONLY;
            else if (ord_type == "fok") order_type = OrderType::FOK;
            else if (ord_type == "ioc") order_type = OrderType::IOC;
            
            // 解析订单方向
            OrderSide side = item.value("side", "buy") == "buy" ? OrderSide::BUY : OrderSide::SELL;
            
            // 安全地解析数量和价格（市价单的px可能为空字符串）
            double sz = safe_stod(item, "sz", 0.0);
            double px = safe_stod(item, "px", 0.0);
            
            // 创建订单对象
            auto order = std::make_shared<Order>(
                item.value("instId", ""),
                order_type,
                side,
                sz,
                px,
                "okx"
            );
            
            order->set_client_order_id(item.value("clOrdId", ""));
            order->set_exchange_order_id(item.value("ordId", ""));
            
            // 解析订单状态
            std::string state = item.value("state", "");
            if (state == "live") {
                order->set_state(OrderState::ACCEPTED);
            } else if (state == "partially_filled") {
                order->set_state(OrderState::PARTIALLY_FILLED);
            } else if (state == "filled") {
                order->set_state(OrderState::FILLED);
            } else if (state == "canceled") {
                order->set_state(OrderState::CANCELLED);
            }
            
            // 设置成交信息（使用安全解析函数）
            double fill_sz = safe_stod(item, "fillSz", 0.0);
            if (fill_sz > 0.0) {
                order->set_filled_quantity(fill_sz);
            }
            
            double avg_px = safe_stod(item, "avgPx", 0.0);
            if (avg_px > 0.0) {
                order->set_filled_price(avg_px);
            }
            
            double fee = safe_stod(item, "fee", 0.0);
            if (fee != 0.0) {
                order->set_fee(fee);
            }
            
            if (item.contains("feeCcy")) {
                order->set_fee_currency(item["feeCcy"].get<std::string>());
            }
            
            // 设置时间（使用安全解析函数）
            int64_t c_time = safe_stoll(item, "cTime", 0);
            if (c_time > 0) {
                order->set_create_time(c_time);
            }
            
            int64_t u_time = safe_stoll(item, "uTime", 0);
            if (u_time > 0) {
                order->set_update_time(u_time);
            }
            
            // 调用回调
            if (order_callback_) {
                order_callback_(order);
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] ❌ 解析Order失败: " << e.what() << std::endl;
            std::cerr << "[WebSocket] 原始数据: " << item.dump(2) << std::endl;
        }
    }
}

void OKXWebSocket::parse_position(const nlohmann::json& data) {
    // 调试日志
    if (!position_callback_) {
        std::cerr << "[WebSocket] ⚠️ 持仓回调未设置！" << std::endl;
        return;
    }
    
    if (!data.is_array()) {
        std::cerr << "[WebSocket] ⚠️ 持仓数据不是数组格式: " << data.dump() << std::endl;
        return;
    }
    
    if (data.empty()) {
        std::cout << "[WebSocket] ⚠️ 持仓数据为空数组（可能没有持仓）" << std::endl;
        // 即使数据为空，也调用回调，传递空数组
        position_callback_(data);
        return;
    }

    // 创建一个数组来存储所有持仓
    nlohmann::json positions_array = nlohmann::json::array();

    for (const auto& item : data) {
        try {
            positions_array.push_back(item);
        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] ❌ 解析Position失败: " << e.what() << std::endl;
        }
    }

    // 调用回调，传递所有持仓数据
    if (!positions_array.empty()) {
        position_callback_(positions_array);
    }
}

void OKXWebSocket::parse_account(const nlohmann::json& data) {
    // 调试日志
    if (!account_callback_) {
        std::cerr << "[WebSocket] ⚠️ 账户回调未设置！" << std::endl;
        return;
    }
    
    if (!data.is_array()) {
        std::cerr << "[WebSocket] ⚠️ 账户数据不是数组格式: " << data.dump() << std::endl;
        return;
    }
    
    if (data.empty()) {
        std::cout << "[WebSocket] ⚠️ 账户数据为空数组" << std::endl;
        return;
    }
    
    // std::cout << "[WebSocket] 开始解析账户数据，共 " << data.size() << " 条" << std::endl;
    
    for (const auto& item : data) {
        try {
            // 账户数据比较复杂，直接传递原始JSON给回调
            // 用户可以根据需要解析以下字段:
            // - totalEq: 总权益（美元）
            // - isoEq: 逐仓仓位权益（美元）
            // - adjEq: 有效保证金（美元）
            // - ordFroz: 下单冻结的保证金（美元）
            // - imr: 初始保证金（美元）
            // - mmr: 维持保证金（美元）
            // - mgnRatio: 保证金率
            // - notionalUsd: 以美元计的持仓价值
            // - details: 各币种详情数组
            //   - ccy: 币种
            //   - eq: 币种权益
            //   - cashBal: 现金余额
            //   - availEq: 可用权益
            //   - availBal: 可用余额
            //   - frozenBal: 冻结余额
            //   - ordFrozen: 下单冻结
            //   - upl: 未实现盈亏
            // - uTime: 更新时间
            
            // 账户更新日志已关闭
            
            account_callback_(item);
            
        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] ❌ 解析Account失败: " << e.what() << std::endl;
            std::cerr << "[WebSocket] 原始数据: " << item.dump(2) << std::endl;
        }
    }
}

void OKXWebSocket::parse_balance_and_position(const nlohmann::json& data) {
    // 调试日志
    if (!balance_and_position_callback_) {
        std::cerr << "[WebSocket] ⚠️ 账户余额和持仓回调未设置！" << std::endl;
        return;
    }
    
    if (!data.is_array()) {
        std::cerr << "[WebSocket] ⚠️ balance_and_position数据不是数组格式: " << data.dump() << std::endl;
        return;
    }
    
    if (data.empty()) {
        std::cout << "[WebSocket] ⚠️ balance_and_position数据为空数组" << std::endl;
        return;
    }
    
    std::cout << "[WebSocket] 开始解析账户余额和持仓数据，共 " << data.size() << " 条" << std::endl;
    
    for (const auto& item : data) {
        try {
            // balance_and_position 数据结构:
            // - pTime: 推送时间
            // - eventType: 事件类型 (snapshot/delivered/exercised/transferred/filled/liquidation等)
            // - balData: 余额数据数组
            //   - ccy: 币种
            //   - cashBal: 币种余额
            //   - uTime: 更新时间
            // - posData: 持仓数据数组
            //   - posId: 持仓ID
            //   - instId: 产品ID
            //   - instType: 产品类型 (MARGIN/SWAP/FUTURES/OPTION)
            //   - mgnMode: 保证金模式 (isolated/cross)
            //   - posSide: 持仓方向 (long/short/net)
            //   - pos: 持仓数量
            //   - avgPx: 开仓均价
            //   - ccy: 占用保证金的币种
            //   - uTime: 更新时间
            // - trades: 成交数据数组
            //   - instId: 产品ID
            //   - tradeId: 成交ID
            
            std::string p_time = item.value("pTime", "");
            std::string event_type = item.value("eventType", "");
            
            // 统计余额和持仓数量
            size_t bal_count = 0;
            size_t pos_count = 0;
            size_t trade_count = 0;
            
            if (item.contains("balData") && item["balData"].is_array()) {
                bal_count = item["balData"].size();
            }
            if (item.contains("posData") && item["posData"].is_array()) {
                pos_count = item["posData"].size();
            }
            if (item.contains("trades") && item["trades"].is_array()) {
                trade_count = item["trades"].size();
            }
            
            std::cout << "[WebSocket] ✅ 账户余额和持仓更新: "
                      << "事件=" << event_type
                      << " | 余额数=" << bal_count
                      << " | 持仓数=" << pos_count;
            if (trade_count > 0) {
                std::cout << " | 成交数=" << trade_count;
            }
            if (!p_time.empty()) {
                std::cout << " | 时间=" << p_time;
            }
            std::cout << std::endl;
            
            // 打印余额详情
            if (item.contains("balData") && item["balData"].is_array()) {
                for (const auto& bal : item["balData"]) {
                    std::string ccy = bal.value("ccy", "");
                    std::string cash_bal = bal.value("cashBal", "");
                    std::cout << "[WebSocket]   余额: " << ccy << " = " << cash_bal << std::endl;
                }
            }
            
            // 打印持仓详情
            if (item.contains("posData") && item["posData"].is_array()) {
                for (const auto& pos : item["posData"]) {
                    std::string inst_id = pos.value("instId", "");
                    std::string pos_side = pos.value("posSide", "");
                    std::string pos_amt = pos.value("pos", "");
                    std::string avg_px = pos.value("avgPx", "");
                    std::cout << "[WebSocket]   持仓: " << inst_id 
                              << " | 方向=" << pos_side
                              << " | 数量=" << pos_amt
                              << " | 均价=" << avg_px << std::endl;
                }
            }
            
            // 调用回调
            balance_and_position_callback_(item);
            
        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] ❌ 解析balance_and_position失败: " << e.what() << std::endl;
            std::cerr << "[WebSocket] 原始数据: " << item.dump(2) << std::endl;
        }
    }
}

void OKXWebSocket::parse_open_interest(const nlohmann::json& data) {
    if (!open_interest_callback_ || !data.is_array() || data.empty()) return;

    for (const auto& item : data) {
        try {
            // 直接传递原始JSON，添加exchange信息
            nlohmann::json raw_data = item;
            raw_data["exchange"] = "okx";

            open_interest_callback_(raw_data);

        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] 解析OpenInterest失败: " << e.what() << std::endl;
        }
    }
}

void OKXWebSocket::parse_mark_price(const nlohmann::json& data) {
    if (!mark_price_callback_ || !data.is_array() || data.empty()) return;

    for (const auto& item : data) {
        try {
            // 直接传递原始JSON，添加exchange信息
            nlohmann::json raw_data = item;
            raw_data["exchange"] = "okx";

            mark_price_callback_(raw_data);

        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] 解析MarkPrice失败: " << e.what() << std::endl;
        }
    }
}

void OKXWebSocket::parse_funding_rate(const nlohmann::json& data) {
    if (!funding_rate_callback_ || !data.is_array() || data.empty()) return;

    for (const auto& item : data) {
        try {
            // 直接传递原始JSON，添加exchange信息
            nlohmann::json raw_data = item;
            raw_data["exchange"] = "okx";

            funding_rate_callback_(raw_data);

        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] 解析FundingRate失败: " << e.what() << std::endl;
        }
    }
}

void OKXWebSocket::parse_sprd_order(const nlohmann::json& data) {
    if (!order_callback_ || !data.is_array() || data.empty()) return;
    
    for (const auto& item : data) {
        try {
            // Spread订单数据结构与普通订单类似，但使用sprdId而不是instId
            // 解析订单类型
            OrderType order_type = OrderType::LIMIT;
            std::string ord_type = item.value("ordType", "limit");
            if (ord_type == "market") order_type = OrderType::MARKET;
            else if (ord_type == "post_only") order_type = OrderType::POST_ONLY;
            else if (ord_type == "fok") order_type = OrderType::FOK;
            else if (ord_type == "ioc") order_type = OrderType::IOC;
            
            // 解析订单方向
            OrderSide side = item.value("side", "buy") == "buy" ? OrderSide::BUY : OrderSide::SELL;
            
            // Spread订单使用sprdId作为symbol
            std::string sprd_id = item.value("sprdId", "");
            
            // 安全地解析数量和价格
            double sz = safe_stod(item, "sz", 0.0);
            double px = safe_stod(item, "px", 0.0);
            
            // 创建订单对象（使用sprdId作为symbol）
            auto order = std::make_shared<Order>(
                sprd_id,
                order_type,
                side,
                sz,
                px,
                "okx"
            );
            
            order->set_client_order_id(item.value("clOrdId", ""));
            order->set_exchange_order_id(item.value("ordId", ""));
            
            // 解析订单状态
            std::string state = item.value("state", "");
            if (state == "live") {
                order->set_state(OrderState::ACCEPTED);
            } else if (state == "partially_filled") {
                order->set_state(OrderState::PARTIALLY_FILLED);
            } else if (state == "filled") {
                order->set_state(OrderState::FILLED);
            } else if (state == "canceled") {
                order->set_state(OrderState::CANCELLED);
            }
            
            // 设置成交信息（使用安全解析函数）
            double acc_fill_sz = safe_stod(item, "accFillSz", 0.0);
            if (acc_fill_sz > 0.0) {
                order->set_filled_quantity(acc_fill_sz);
            }
            
            double avg_px = safe_stod(item, "avgPx", 0.0);
            if (avg_px > 0.0) {
                order->set_filled_price(avg_px);
            }
            
            // 设置时间（使用安全解析函数）
            int64_t c_time = safe_stoll(item, "cTime", 0);
            if (c_time > 0) {
                order->set_create_time(c_time);
            }
            
            int64_t u_time = safe_stoll(item, "uTime", 0);
            if (u_time > 0) {
                order->set_update_time(u_time);
            }
            
            std::cout << "[WebSocket] 收到Spread订单: " << sprd_id 
                      << " | 订单ID: " << order->exchange_order_id()
                      << " | 状态: " << state << std::endl;
            
            order_callback_(order);
            
        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] 解析Spread订单失败: " << e.what() << std::endl;
        }
    }
}

void OKXWebSocket::parse_sprd_trade(const nlohmann::json& data) {
    if (!spread_trade_callback_ || !data.is_array() || data.empty()) return;

    for (const auto& item : data) {
        try {
            // 直接传递原始JSON，添加exchange信息
            nlohmann::json raw_data = item;
            raw_data["exchange"] = "okx";

            spread_trade_callback_(raw_data);

        } catch (const std::exception& e) {
            std::cerr << "[WebSocket] 解析Spread成交失败: " << e.what() << std::endl;
        }
    }
}

} // namespace okx
} // namespace trading

