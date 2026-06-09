#include "websocket_server.h"
#include <iostream>
#include <chrono>
#include <fstream>
#include <mutex>

namespace trading {
namespace core {

WebSocketServer::WebSocketServer()
#ifdef USE_WEBSOCKETPP
    : server_impl_(nullptr)
#else
    : server_impl_(nullptr)
#endif
{
}

WebSocketServer::~WebSocketServer() {
    stop();
}

bool WebSocketServer::start(const std::string& host, uint16_t port) {
    if (running_.load()) {
        std::cerr << "[WebSocketServer] 服务器已在运行中" << std::endl;
        return false;
    }

    host_ = host;
    port_ = port;

    std::cout << "[WebSocketServer] 正在启动WebSocket服务器..." << std::endl;
    std::cout << "[WebSocketServer] 监听地址: ws://" << host << ":" << port << std::endl;

    running_.store(true);
    stopped_.store(false);

    server_thread_ = std::make_unique<std::thread>(&WebSocketServer::server_thread_func, this);
    snapshot_thread_ = std::make_unique<std::thread>(&WebSocketServer::snapshot_thread_func, this);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "[WebSocketServer] WebSocket服务器已启动" << std::endl;

    return true;
}

void WebSocketServer::stop() {
    if (!running_.load() || stopped_.load()) {
        return;
    }

    std::cout << "[WebSocketServer] 正在停止WebSocket服务器..." << std::endl;

    stopped_.store(true);
    running_.store(false);

#ifdef USE_WEBSOCKETPP
    if (server_impl_) {
        server_impl_->stop();
    }
#endif

    message_queue_cv_.notify_all();

    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
        server_thread_.reset();
    }

    if (snapshot_thread_ && snapshot_thread_->joinable()) {
        snapshot_thread_->join();
        snapshot_thread_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.clear();
#ifdef USE_WEBSOCKETPP
        hdl_to_id_.clear();
#endif
    }

    std::cout << "[WebSocketServer] WebSocket服务器已停止" << std::endl;
}

void WebSocketServer::set_message_callback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    message_callback_ = std::move(callback);
}

void WebSocketServer::set_snapshot_generator(SnapshotGenerator generator) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    snapshot_generator_ = std::move(generator);
}

void WebSocketServer::set_snapshot_interval(int interval_ms) {
    snapshot_interval_ms_ = interval_ms;
}

void WebSocketServer::send_response(int client_id, bool success, const std::string& message, const nlohmann::json& data) {
    nlohmann::json response = {
        {"type", "response"},
        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()},
        {"data", {
            {"success", success},
            {"message", message}
        }}
    };

    if (!data.empty()) {
        for (auto& [key, value] : data.items()) {
            response["data"][key] = value;
        }
    }

    {
        std::lock_guard<std::mutex> lock(message_queue_mutex_);
        message_queue_.push({client_id, response});
    }
    message_queue_cv_.notify_one();
}

void WebSocketServer::send_event(const std::string& event_type, const nlohmann::json& data) {
    nlohmann::json event = {
        {"type", "event"},
        {"event_type", event_type},
        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()},
        {"data", data}
    };

    {
        std::lock_guard<std::mutex> lock(message_queue_mutex_);
        message_queue_.push({-1, event});
    }
    message_queue_cv_.notify_one();
}

void WebSocketServer::send_log(const std::string& level, const std::string& source, const std::string& message) {
    // 转换日志级别为小写（前端期望小写）
    std::string level_lower = level;
    for (auto& c : level_lower) {
        c = std::tolower(c);
    }
    // 去除尾部空格
    while (!level_lower.empty() && level_lower.back() == ' ') {
        level_lower.pop_back();
    }
    // WARN -> warning
    if (level_lower == "warn") {
        level_lower = "warning";
    }

    nlohmann::json log_msg = {
        {"type", "log"},
        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()},
        {"data", {
            {"level", level_lower},
            {"source", source},
            {"message", message}
        }}
    };

    {
        std::lock_guard<std::mutex> lock(message_queue_mutex_);
        message_queue_.push({-1, log_msg});
    }
    message_queue_cv_.notify_one();
}

void WebSocketServer::server_thread_func() {
    std::cout << "[WebSocketServer] 服务器线程启动" << std::endl;

#ifdef USE_WEBSOCKETPP
    try {
        server_impl_ = std::make_unique<WsServer>();

        server_impl_->set_reuse_addr(true);
        server_impl_->init_asio();

        // 关闭详细日志
        server_impl_->clear_access_channels(websocketpp::log::alevel::all);
        server_impl_->clear_error_channels(websocketpp::log::elevel::all);

        server_impl_->set_open_handler([this](ConnectionHdl hdl) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            int client_id = next_client_id_++;
            clients_[client_id] = hdl;
            hdl_to_id_[hdl.lock().get()] = client_id;
            std::cout << "[WebSocketServer] 客户端 " << client_id << " 已连接" << std::endl;
        });

        server_impl_->set_close_handler([this](ConnectionHdl hdl) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto ptr = hdl.lock().get();
            auto it = hdl_to_id_.find(ptr);
            if (it != hdl_to_id_.end()) {
                int client_id = it->second;
                clients_.erase(client_id);
                hdl_to_id_.erase(it);
                std::cout << "[WebSocketServer] 客户端 " << client_id << " 已断开" << std::endl;
            }
        });

        server_impl_->set_message_handler([this](ConnectionHdl hdl, WsServer::message_ptr msg) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto ptr = hdl.lock().get();
            auto it = hdl_to_id_.find(ptr);
            if (it != hdl_to_id_.end()) {
                handle_client_message(it->second, msg->get_payload());
            }
        });

        server_impl_->listen(port_);
        server_impl_->start_accept();

        std::thread msg_thread([this]() {
            while (running_.load()) {
                process_message_queue();
            }
        });

        server_impl_->run();

        if (msg_thread.joinable()) {
            msg_thread.join();
        }

    } catch (const std::exception& e) {
        std::cerr << "[WebSocketServer] 异常: " << e.what() << std::endl;
    }
#else
    while (running_.load()) {
        process_message_queue();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif

    std::cout << "[WebSocketServer] 服务器线程退出" << std::endl;
}

void WebSocketServer::snapshot_thread_func() {
    std::cout << "[WebSocketServer] 快照线程启动" << std::endl;

    while (running_.load()) {
        SnapshotGenerator generator;

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            generator = snapshot_generator_;
        }

        if (generator) {
            try {
                nlohmann::json snapshot = generator();

                nlohmann::json message = {
                    {"type", "snapshot"},
                    {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count()},
                    {"data", snapshot}
                };

                {
                    std::lock_guard<std::mutex> lock(message_queue_mutex_);
                    message_queue_.push({-1, message});
                }
                message_queue_cv_.notify_one();

            } catch (const std::exception& e) {
                std::cerr << "[WebSocketServer] 生成快照失败: " << e.what() << std::endl;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(snapshot_interval_ms_));
    }

    std::cout << "[WebSocketServer] 快照线程退出" << std::endl;
}

void WebSocketServer::process_message_queue() {
    std::unique_lock<std::mutex> lock(message_queue_mutex_);

    message_queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return !message_queue_.empty() || !running_.load();
    });

    while (!message_queue_.empty()) {
        PendingMessage msg = message_queue_.front();
        message_queue_.pop();

        lock.unlock();

        if (msg.client_id == -1) {
            broadcast_internal(msg.message);
        } else {
            send_to_client_internal(msg.client_id, msg.message);
        }

        lock.lock();
    }
}

void WebSocketServer::handle_client_message(int client_id, const std::string& message) {
    try {
        nlohmann::json json_msg = nlohmann::json::parse(message);

        // Debug: 写入 frontend.txt
        {
            static std::mutex dbg_mtx;
            std::lock_guard<std::mutex> lock(dbg_mtx);
            std::ofstream f("logs/frontend.txt", std::ios::app);
            if (f.is_open()) {
                std::string action = json_msg.value("action", "");
                std::string type = json_msg.value("type", "");
                f << "[WS收到] client=" << client_id << " type=" << type << " action=" << action << "\n";
                f.flush();
            }
        }

        MessageCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = message_callback_;
        }

        if (callback) {
            callback(client_id, json_msg);
        }
    } catch (const std::exception& e) {
        std::cerr << "[WebSocketServer] 解析消息失败: " << e.what() << std::endl;
    }
}

void WebSocketServer::broadcast_internal(const nlohmann::json& message) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

#ifdef USE_WEBSOCKETPP
    std::string msg_str = message.dump();
    for (auto& [id, hdl] : clients_) {
        try {
            server_impl_->send(hdl, msg_str, websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            std::cerr << "[WebSocketServer] 广播失败: " << e.what() << std::endl;
        }
    }
#else
    std::string msg_str = message.dump();
    if (msg_str.length() > 100) {
        std::cout << "[WebSocketServer] 广播消息: " << msg_str.substr(0, 100) << "..." << std::endl;
    } else {
        std::cout << "[WebSocketServer] 广播消息: " << msg_str << std::endl;
    }
#endif
}

void WebSocketServer::send_to_client_internal(int client_id, const nlohmann::json& message) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

#ifdef USE_WEBSOCKETPP
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        try {
            std::string msg_str = message.dump();
            server_impl_->send(it->second, msg_str, websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            std::cerr << "[WebSocketServer] 发送失败: " << e.what() << std::endl;
        }
    }
#else
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        std::string msg_str = message.dump();
        if (msg_str.length() > 100) {
            std::cout << "[WebSocketServer] 发送消息给客户端 " << client_id
                      << ": " << msg_str.substr(0, 100) << "..." << std::endl;
        } else {
            std::cout << "[WebSocketServer] 发送消息给客户端 " << client_id
                      << ": " << msg_str << std::endl;
        }
    } else {
        std::cerr << "[WebSocketServer] 客户端 " << client_id << " 不存在" << std::endl;
    }
#endif
}

} // namespace core
} // namespace trading
