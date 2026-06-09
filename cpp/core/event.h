#pragma once

#include <memory>
#include <functional>
#include <chrono>
#include <string>
#include <typeindex>

namespace trading {

// 前向声明
class EventEngine;

/**
 * @brief 事件基类
 * 
 * 所有事件都包含：
 * 1. timestamp: 事件发生时间戳（毫秒）
 * 2. source: 事件来源（事件引擎ID）
 * 3. producer: 事件产生者（监听器函数的标识）
 */
class Event {
public:
    using Ptr = std::shared_ptr<Event>;
    using ListenerFunc = std::function<void(const Event::Ptr&)>;
    
    Event() 
        : timestamp_(0)
        , source_(nullptr)
        , producer_id_(0) {
    }
    
    virtual ~Event() noexcept = default;
    
    // 获取事件类型名称（用于调试）
    virtual std::string type_name() const { 
        return "Event"; 
    }
    
    // 时间戳（毫秒）
    int64_t timestamp() const { return timestamp_; }
    void set_timestamp(int64_t ts) { timestamp_ = ts; }
    
    // 事件来源引擎
    const EventEngine* source() const { return source_; }
    void set_source(const EventEngine* engine) { source_ = engine; }
    
    // 事件产生者ID
    size_t producer_id() const { return producer_id_; }
    void set_producer_id(size_t id) { producer_id_ = id; }
    
    // 浅拷贝事件
    virtual Event::Ptr copy() const {
        auto e = std::make_shared<Event>(*this);
        return e;
    }
    
    // 派生新事件（清空时间戳、来源、产生者）
    virtual Event::Ptr derive() const {
        auto e = copy();
        e->timestamp_ = 0;
        e->source_ = nullptr;
        e->producer_id_ = 0;
        return e;
    }
    
    // 获取当前时间戳（毫秒）
    static int64_t current_timestamp() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count();
    }

protected:
    int64_t timestamp_;          // Unix时间戳（毫秒）
    const EventEngine* source_;  // 事件引擎
    size_t producer_id_;         // 产生者ID
};

} // namespace trading

