#pragma once

#include <functional>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <queue>
#include <condition_variable>
#include <nlohmann/json.hpp>

#ifdef USE_WEBSOCKETPP
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
typedef websocketpp::server<websocketpp::config::asio> WsServer;
typedef websocketpp::connection_hdl ConnectionHdl;
#endif

namespace trading {
namespace core {

class WebSocketServer {
public:
    using MessageCallback = std::function<void(int client_id, const nlohmann::json& message)>;
    using SnapshotGenerator = std::function<nlohmann::json()>;

    WebSocketServer();
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;
    WebSocketServer(WebSocketServer&&) = delete;
    WebSocketServer& operator=(WebSocketServer&&) = delete;

    bool start(const std::string& host, uint16_t port);
    void stop();
    bool is_running() const { return running_.load(); }

    void set_message_callback(MessageCallback callback);
    void set_snapshot_generator(SnapshotGenerator generator);
    void set_snapshot_interval(int interval_ms);

    void send_response(int client_id, bool success, const std::string& message, const nlohmann::json& data = {});
    void send_event(const std::string& event_type, const nlohmann::json& data);
    void send_log(const std::string& level, const std::string& source, const std::string& message);

private:
    struct PendingMessage {
        int client_id;
        nlohmann::json message;
    };

    void server_thread_func();
    void snapshot_thread_func();
    void handle_client_message(int client_id, const std::string& message);
    void broadcast_internal(const nlohmann::json& message);
    void send_to_client_internal(int client_id, const nlohmann::json& message);
    void process_message_queue();

    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{true};

    std::unique_ptr<std::thread> server_thread_;
    std::unique_ptr<std::thread> snapshot_thread_;

    std::mutex callback_mutex_;
    MessageCallback message_callback_;
    SnapshotGenerator snapshot_generator_;
    int snapshot_interval_ms_{100};

    std::mutex clients_mutex_;
#ifdef USE_WEBSOCKETPP
    std::map<int, ConnectionHdl> clients_;
#else
    std::map<int, void*> clients_;
#endif
    int next_client_id_{1};

    std::mutex message_queue_mutex_;
    std::condition_variable message_queue_cv_;
    std::queue<PendingMessage> message_queue_;

    std::string host_;
    uint16_t port_;

#ifdef USE_WEBSOCKETPP
    std::unique_ptr<WsServer> server_impl_;
    std::map<void*, int> hdl_to_id_;
#else
    void* server_impl_;
#endif
};

} // namespace core
} // namespace trading

