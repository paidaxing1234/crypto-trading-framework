#pragma once

/**
 * @file auth_manager.h
 * @brief 用户认证管理器
 *
 * 功能：
 * - 用户登录/登出
 * - JWT Token 生成和验证
 * - 密码哈希（SHA256+盐）
 * - 角色权限管理
 * - 用户持久化（JSON文件）
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>

namespace trading {
namespace auth {

/**
 * @brief 用户角色
 */
enum class UserRole {
    SUPER_ADMIN,        // 超级管理员
    STRATEGY_MANAGER    // 策略管理员
};

/**
 * @brief 用户信息
 */
struct UserInfo {
    std::string username;
    std::string password_hash;  // SHA256(password + salt)
    std::string salt;
    UserRole role;
    bool active;
    int64_t created_at;
    int64_t last_login;
    std::vector<std::string> allowed_strategies;  // 允许访问的策略ID列表（仅策略管理员）
};

/**
 * @brief Token信息
 */
struct TokenInfo {
    std::string username;
    UserRole role;
    int64_t expires_at;
};

/**
 * @brief 认证管理器
 */
class AuthManager {
public:
    AuthManager(const std::string& jwt_secret = "CHANGE_ME_SET_A_STRONG_JWT_SECRET")
        : jwt_secret_(jwt_secret), token_expiry_hours_(24) {
    }

    /**
     * @brief 设置用户配置目录并加载用户
     */
    void init_user_configs(const std::string& dir) {
        user_config_dir_ = dir;
        // 确保目录存在
        try {
            std::filesystem::create_directories(dir);
        } catch (...) {}

        load_users();

        // 如果没有超级管理员，创建默认管理员（安全提示：首次登录后请立即修改密码）
        bool has_super_admin = false;
        for (const auto& [_, user] : users_) {
            if (user.role == UserRole::SUPER_ADMIN) {
                has_super_admin = true;
                break;
            }
        }
        if (!has_super_admin) {
            add_user("admin", "CHANGE_ME_ON_FIRST_LOGIN", UserRole::SUPER_ADMIN);
        }
    }

    /**
     * @brief 用户登录
     * @return JWT Token，失败返回空字符串
     */
    std::string login(const std::string& username, const std::string& password) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = users_.find(username);
        if (it == users_.end()) {
            return "";  // 用户不存在
        }

        UserInfo& user = it->second;
        if (!user.active) {
            return "";  // 用户已禁用
        }

        // 验证密码
        std::string hash = hash_password(password, user.salt);
        if (hash != user.password_hash) {
            return "";  // 密码错误
        }

        // 更新最后登录时间
        user.last_login = current_timestamp();
        save_user_unlocked(username);

        // 生成Token
        return generate_token(username, user.role);
    }

    /**
     * @brief 验证Token
     * @return 验证成功返回true，并填充TokenInfo
     */
    bool verify_token(const std::string& token, TokenInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 检查是否在黑名单中（已登出）
        if (revoked_tokens_.find(token) != revoked_tokens_.end()) {
            return false;
        }

        // 检查Token缓存
        auto it = active_tokens_.find(token);
        if (it != active_tokens_.end()) {
            if (it->second.expires_at > current_timestamp()) {
                info = it->second;
                return true;
            }
            // Token已过期，移除
            active_tokens_.erase(it);
            return false;
        }

        // 尝试解析Token
        if (!parse_token(token, info)) {
            return false;
        }

        // 检查是否过期
        if (info.expires_at <= current_timestamp()) {
            return false;
        }

        // 缓存有效Token
        active_tokens_[token] = info;
        return true;
    }

    /**
     * @brief 登出（使Token失效）
     */
    void logout(const std::string& token) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_tokens_.erase(token);
        revoked_tokens_.insert(token);
    }

    /**
     * @brief 添加用户
     */
    bool add_user(const std::string& username, const std::string& password,
                  UserRole role, const std::vector<std::string>& allowed_strategies = {}) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (users_.find(username) != users_.end()) {
            return false;  // 用户已存在
        }

        UserInfo user;
        user.username = username;
        user.salt = generate_salt();
        user.password_hash = hash_password(password, user.salt);
        user.role = role;
        user.active = true;
        user.created_at = current_timestamp();
        user.last_login = 0;
        user.allowed_strategies = allowed_strategies;

        users_[username] = user;
        save_user_unlocked(username);
        return true;
    }

    /**
     * @brief 删除用户
     */
    bool delete_user(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = users_.find(username);
        if (it == users_.end()) {
            return false;
        }

        // 不允许删除超级管理员
        if (it->second.role == UserRole::SUPER_ADMIN) {
            return false;
        }

        // 吊销该用户所有token
        invalidate_user_tokens(username);

        // 删除配置文件
        if (!user_config_dir_.empty()) {
            std::string filepath = user_config_dir_ + "/" + username + ".json";
            try {
                std::filesystem::remove(filepath);
            } catch (...) {}
        }

        users_.erase(it);
        return true;
    }

    /**
     * @brief 更新用户的策略权限
     */
    bool update_user(const std::string& username,
                     const std::vector<std::string>& allowed_strategies) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = users_.find(username);
        if (it == users_.end()) {
            return false;
        }

        it->second.allowed_strategies = allowed_strategies;
        save_user_unlocked(username);
        return true;
    }

    /**
     * @brief 修改密码
     */
    bool change_password(const std::string& username, const std::string& old_password,
                         const std::string& new_password) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = users_.find(username);
        if (it == users_.end()) {
            return false;
        }

        UserInfo& user = it->second;

        // 验证旧密码
        if (hash_password(old_password, user.salt) != user.password_hash) {
            return false;
        }

        // 更新密码
        user.salt = generate_salt();
        user.password_hash = hash_password(new_password, user.salt);

        // 使该用户所有Token失效
        invalidate_user_tokens(username);

        // 持久化
        save_user_unlocked(username);

        return true;
    }

    /**
     * @brief 检查权限
     */
    bool has_permission(UserRole user_role, const std::string& action) {
        if (user_role == UserRole::SUPER_ADMIN) {
            return true;  // 超级管理员拥有所有权限
        }

        if (user_role == UserRole::STRATEGY_MANAGER) {
            // 策略管理员可以查看和执行策略操作（启动/停止）
            return action == "view" || action == "trade";
        }

        return false;
    }

    /**
     * @brief 获取用户信息
     */
    const UserInfo* get_user_info(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it == users_.end()) return nullptr;
        return &it->second;
    }

    /**
     * @brief 获取用户列表
     */
    nlohmann::json get_users() {
        std::lock_guard<std::mutex> lock(mutex_);

        nlohmann::json result = nlohmann::json::array();
        for (const auto& [username, user] : users_) {
            nlohmann::json u = {
                {"username", username},
                {"role", role_to_string(user.role)},
                {"active", user.active},
                {"created_at", user.created_at},
                {"last_login", user.last_login}
            };
            if (user.role == UserRole::STRATEGY_MANAGER) {
                u["allowed_strategies"] = user.allowed_strategies;
            }
            result.push_back(u);
        }
        return result;
    }

    /**
     * @brief 设置Token过期时间（小时）
     */
    void set_token_expiry(int hours) {
        token_expiry_hours_ = hours;
    }

    // 角色转换
    static std::string role_to_string(UserRole role) {
        switch (role) {
            case UserRole::SUPER_ADMIN: return "SUPER_ADMIN";
            case UserRole::STRATEGY_MANAGER: return "STRATEGY_MANAGER";
            default: return "UNKNOWN";
        }
    }

    static UserRole string_to_role(const std::string& str) {
        if (str == "SUPER_ADMIN") return UserRole::SUPER_ADMIN;
        return UserRole::STRATEGY_MANAGER;
    }

private:
    // 加载所有用户配置
    void load_users() {
        if (user_config_dir_.empty()) return;

        try {
            for (const auto& entry : std::filesystem::directory_iterator(user_config_dir_)) {
                if (entry.path().extension() != ".json") continue;

                std::ifstream f(entry.path());
                if (!f.is_open()) continue;

                try {
                    auto j = nlohmann::json::parse(f);
                    UserInfo user;
                    user.username = j.value("username", "");
                    user.password_hash = j.value("password_hash", "");
                    user.salt = j.value("salt", "");
                    user.role = string_to_role(j.value("role", "STRATEGY_MANAGER"));
                    user.active = j.value("active", true);
                    user.created_at = j.value("created_at", (int64_t)0);
                    user.last_login = j.value("last_login", (int64_t)0);

                    if (j.contains("allowed_strategies") && j["allowed_strategies"].is_array()) {
                        for (const auto& s : j["allowed_strategies"]) {
                            user.allowed_strategies.push_back(s.get<std::string>());
                        }
                    }

                    if (!user.username.empty() && !user.password_hash.empty()) {
                        users_[user.username] = user;
                    }
                } catch (...) {
                    // 跳过解析失败的文件
                }
            }
        } catch (...) {
            // 目录不存在或无法读取
        }
    }

    // 保存单个用户（不加锁，调用者需持有锁）
    void save_user_unlocked(const std::string& username) {
        if (user_config_dir_.empty()) return;

        auto it = users_.find(username);
        if (it == users_.end()) return;

        const UserInfo& user = it->second;
        nlohmann::json j = {
            {"username", user.username},
            {"password_hash", user.password_hash},
            {"salt", user.salt},
            {"role", role_to_string(user.role)},
            {"active", user.active},
            {"created_at", user.created_at},
            {"last_login", user.last_login},
            {"allowed_strategies", user.allowed_strategies}
        };

        std::string filepath = user_config_dir_ + "/" + username + ".json";
        std::ofstream f(filepath);
        if (f.is_open()) {
            f << j.dump(2);
        }
    }

    // 生成随机盐
    std::string generate_salt() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        std::stringstream ss;
        for (int i = 0; i < 16; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << dis(gen);
        }
        return ss.str();
    }

    // 密码哈希
    std::string hash_password(const std::string& password, const std::string& salt) {
        std::string data = password + salt;
        unsigned char hash[SHA256_DIGEST_LENGTH];

        SHA256((unsigned char*)data.c_str(), data.length(), hash);

        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        return ss.str();
    }

    // HMAC SHA256
    std::string hmac_sha256(const std::string& key, const std::string& data) {
        unsigned char hash[SHA256_DIGEST_LENGTH];

        HMAC(EVP_sha256(), key.c_str(), key.length(),
             (unsigned char*)data.c_str(), data.length(),
             hash, nullptr);

        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        return ss.str();
    }

    // Base64编码
    std::string base64_encode(const std::string& input) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        int val = 0, valb = -6;

        for (unsigned char c : input) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                result.push_back(chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
        while (result.size() % 4) result.push_back('=');
        return result;
    }

    // Base64解码
    std::string base64_decode(const std::string& input) {
        static const int T[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
        };

        std::string result;
        int val = 0, valb = -8;

        for (unsigned char c : input) {
            if (T[c] == -1) break;
            val = (val << 6) + T[c];
            valb += 6;
            if (valb >= 0) {
                result.push_back(char((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return result;
    }

    // 生成JWT Token
    std::string generate_token(const std::string& username, UserRole role) {
        int64_t expires = current_timestamp() + token_expiry_hours_ * 3600 * 1000;

        // Header
        nlohmann::json header = {{"alg", "HS256"}, {"typ", "JWT"}};

        // Payload
        nlohmann::json payload = {
            {"username", username},
            {"role", role_to_string(role)},
            {"exp", expires},
            {"iat", current_timestamp()}
        };

        std::string header_b64 = base64_encode(header.dump());
        std::string payload_b64 = base64_encode(payload.dump());

        // Signature
        std::string signature_input = header_b64 + "." + payload_b64;
        std::string signature = hmac_sha256(jwt_secret_, signature_input);
        std::string signature_b64 = base64_encode(signature);

        std::string token = header_b64 + "." + payload_b64 + "." + signature_b64;

        // 缓存Token
        TokenInfo info;
        info.username = username;
        info.role = role;
        info.expires_at = expires;
        active_tokens_[token] = info;

        return token;
    }

    // 解析JWT Token
    bool parse_token(const std::string& token, TokenInfo& info) {
        size_t pos1 = token.find('.');
        size_t pos2 = token.rfind('.');

        if (pos1 == std::string::npos || pos2 == std::string::npos || pos1 == pos2) {
            return false;
        }

        std::string header_b64 = token.substr(0, pos1);
        std::string payload_b64 = token.substr(pos1 + 1, pos2 - pos1 - 1);
        std::string signature_b64 = token.substr(pos2 + 1);

        // 验证签名
        std::string signature_input = header_b64 + "." + payload_b64;
        std::string expected_sig = base64_encode(hmac_sha256(jwt_secret_, signature_input));

        if (signature_b64 != expected_sig) {
            return false;
        }

        // 解析Payload
        try {
            std::string payload_str = base64_decode(payload_b64);
            auto payload = nlohmann::json::parse(payload_str);

            info.username = payload.value("username", "");
            info.role = string_to_role(payload.value("role", "STRATEGY_MANAGER"));
            info.expires_at = payload.value("exp", (int64_t)0);

            return true;
        } catch (...) {
            return false;
        }
    }

    // 使用户所有Token失效
    void invalidate_user_tokens(const std::string& username) {
        for (auto it = active_tokens_.begin(); it != active_tokens_.end();) {
            if (it->second.username == username) {
                it = active_tokens_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 获取当前时间戳（毫秒）
    int64_t current_timestamp() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    std::string jwt_secret_;
    std::string user_config_dir_;
    int token_expiry_hours_;
    std::unordered_map<std::string, UserInfo> users_;
    std::unordered_map<std::string, TokenInfo> active_tokens_;
    std::unordered_set<std::string> revoked_tokens_;
    std::mutex mutex_;
};

} // namespace auth
} // namespace trading
