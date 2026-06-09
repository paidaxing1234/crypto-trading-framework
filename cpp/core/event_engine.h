#pragma once

#include "event.h"
#include <queue>
#include <unordered_map>
#include <vector>
#include <typeindex>
#include <memory>
#include <functional>
#include <stdexcept>
#include <mutex>
#include <any>
#include <cstdio>

namespace trading {

/**
 * @brief 事件引擎 - 事件驱动架构的核心
 * 
 * 职责：
 * 1. 接收事件（put方法）
 * 2. 分发事件给注册的监听器
 * 3. 管理全局时间戳
 * 4. 支持动态接口注入
 * 
 * 特性：
 * - 事件按顺序处理，保证一致性
 * - 支持类型化监听（只监听特定事件类型）
 * - 支持全局监听（监听所有事件）
 * - 防止监听器响应自己产生的事件（ignore_self）
 */
class EventEngine {
public:
    using Listener = Event::ListenerFunc;
    
    EventEngine() 
        : timestamp_(Event::current_timestamp())
        , dispatching_(false)
        , current_listener_id_(0) {
    }
    
    virtual ~EventEngine() = default;
    
    /**
     * @brief 注册事件监听器
     * 
     * @param event_type 事件类型（通过 typeid 获取）
     * @param listener 监听器函数
     * @param ignore_self 是否忽略自己产生的事件（防止死循环）
     * 
     * 用法：
     *   engine.register_listener(typeid(OrderEvent), 
     *       [](const Event::Ptr& e) { ... });
     */
    void register_listener(
        const std::type_index& event_type,
        Listener listener,
        bool ignore_self = true
    ) {
        if (dispatching_) {
            throw std::runtime_error("Cannot register while dispatching events");
        }
        
        size_t listener_id = next_listener_id_++;
        ListenerInfo info{listener, ignore_self, listener_id};
        listener_dict_[event_type].push_back(info);
    }
    
    /**
     * @brief 注册全局监听器（监听所有事件类型）
     * 
     * @param listener 监听器函数
     * @param ignore_self 是否忽略自己产生的事件
     * @param is_senior 是否为高优先级监听器（在类型监听器之前执行）
     */
    void register_global_listener(
        Listener listener,
        bool ignore_self = false,
        bool is_senior = false
    ) {
        if (dispatching_) {
            throw std::runtime_error("Cannot register while dispatching events");
        }
        
        size_t listener_id = next_listener_id_++;
        ListenerInfo info{listener, ignore_self, listener_id};
        
        if (is_senior) {
            senior_global_listeners_.push_back(info);
        } else {
            junior_global_listeners_.push_back(info);
        }
    }
    
    /**
     * @brief 推送事件到引擎
     * 
     * 事件推送后会：
     * 1. 标注来源引擎ID
     * 2. 标注/更新时间戳
     * 3. 标注产生者（当前监听器）
     * 4. 入队等待派发
     * 5. 如果不在派发中，立即派发
     */
    void put(Event::Ptr event) {
        if (!event) {
            throw std::invalid_argument("Event cannot be null");
        }
        
        // 标注来源引擎
        if (event->source() == nullptr) {
            event->set_source(this);
        }
        
        // 处理时间戳
        int64_t ts = event->timestamp();
        if (ts == 0) {
            // 事件没有时间戳，使用引擎当前时间
            event->set_timestamp(timestamp_);
        } else if (ts > timestamp_) {
            // 事件时间戳更新，推进引擎时间
            timestamp_ = ts;
        }
        
        // 标注产生者
        event->set_producer_id(current_listener_id_);
        
        // 入队
        queue_.push(event);
        
        // 如果不在派发中，立即开始派发
        if (!dispatching_) {
            drain();
        }
    }
    
    /**
     * @brief 手动更新引擎时间戳
     */
    void update_timestamp(int64_t timestamp) {
        if (timestamp > timestamp_) {
            timestamp_ = timestamp;
        }
    }
    
    /**
     * @brief 获取当前引擎时间戳
     */
    int64_t timestamp() const {
        return timestamp_;
    }
    
    /**
     * @brief 动态注入接口（可供策略等组件调用）
     * 
     * 用法：
     *   engine.inject("get_position", [](){ return position; });
     *   auto pos = engine.call<double>("get_position");
     */
    template<typename Func>
    void inject(const std::string& name, Func func) {
        injected_functions_[name] = func;
    }
    
    template<typename RetType, typename... Args>
    RetType call(const std::string& name, Args&&... args) {
        auto it = injected_functions_.find(name);
        if (it == injected_functions_.end()) {
            throw std::runtime_error("Function not found: " + name);
        }
        
        auto func = std::any_cast<std::function<RetType(Args...)>>(it->second);
        return func(std::forward<Args>(args)...);
    }

protected:
    /**
     * @brief 派发事件队列中的所有事件
     * 
     * 派发顺序：
     * 1. Senior全局监听器
     * 2. 类型特定监听器
     * 3. Junior全局监听器
     */
    void drain() {
        dispatching_ = true;
        
        while (!queue_.empty()) {
            Event::Ptr event = queue_.front();
            queue_.pop();
            
            // 获取事件类型
            std::type_index event_type = typeid(*event);
            
            // 收集所有要执行的监听器
            std::vector<ListenerInfo> listeners;
            
            // 1. Senior全局监听器
            listeners.insert(listeners.end(), 
                senior_global_listeners_.begin(), 
                senior_global_listeners_.end());
            
            // 2. 类型特定监听器
            auto it = listener_dict_.find(event_type);
            if (it != listener_dict_.end()) {
                listeners.insert(listeners.end(), 
                    it->second.begin(), 
                    it->second.end());
            }
            
            // 3. Junior全局监听器
            listeners.insert(listeners.end(), 
                junior_global_listeners_.begin(), 
                junior_global_listeners_.end());
            
            // 执行监听器
            for (const auto& info : listeners) {
                // 如果设置了ignore_self，且事件是自己产生的，跳过
                if (info.ignore_self && event->producer_id() == info.id) {
                    continue;
                }
                
                // 设置当前监听器ID
                current_listener_id_ = info.id;
                
                // 执行监听器
                try {
                    info.listener(event);
                } catch (const std::exception& e) {
                    // 监听器异常不应中断事件派发
                    fprintf(stderr, "Error in listener %zu: %s\n", info.id, e.what());
                }
                
                current_listener_id_ = 0;
            }
        }
        
        dispatching_ = false;
    }

private:
    struct ListenerInfo {
        Listener listener;
        bool ignore_self;
        size_t id;
    };
    
    int64_t timestamp_;                                      // 当前引擎时间戳
    std::queue<Event::Ptr> queue_;                           // 事件队列
    bool dispatching_;                                       // 是否正在派发
    size_t current_listener_id_;                             // 当前监听器ID
    size_t next_listener_id_ = 1;                            // 下一个监听器ID
    
    // 监听器字典：{事件类型: [监听器列表]}
    std::unordered_map<std::type_index, std::vector<ListenerInfo>> listener_dict_;
    
    // 全局监听器
    std::vector<ListenerInfo> senior_global_listeners_;      // 高优先级
    std::vector<ListenerInfo> junior_global_listeners_;      // 低优先级
    
    // 动态注入的函数
    std::unordered_map<std::string, std::any> injected_functions_;
};

/**
 * @brief 组件抽象基类
 * 
 * 所有功能模块都应继承此类，实现标准的生命周期管理
 * 
 * 生命周期：
 * 1. 构造函数: 初始化配置参数
 * 2. start(engine): 启动组件，注册监听器
 * 3. stop(): 停止组件，清理资源
 */
class Component {
public:
    virtual ~Component() = default;
    
    /**
     * @brief 启动组件
     * 
     * 在这里：
     * - 保存引擎引用
     * - 注册事件监听器
     * - 初始化运行时资源
     */
    virtual void start(EventEngine* engine) = 0;
    
    /**
     * @brief 停止组件
     * 
     * 在这里：
     * - 清理资源
     * - 关闭连接
     * - 保存状态
     */
    virtual void stop() = 0;
    
protected:
    EventEngine* engine_ = nullptr;
};

} // namespace trading

