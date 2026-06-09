# 日志系统使用说明

## 概述

前端日志系统提供了两种视图模式和完整的日志管理功能，支持与C++后端实时通信。

## 主要功能

### 1. 双视图模式

#### 控制台视图 (Console View)
- **特点**: 类似终端的实时滚动日志显示
- **适用场景**: 实时监控、调试、快速查看最新日志
- **功能**:
  - 实时滚动显示
  - 彩色日志级别标识
  - 自动滚动开关
  - 全屏模式
  - 快速过滤

#### 表格视图 (Table View)
- **特点**: 结构化的日志列表展示
- **适用场景**: 日志分析、搜索、详细查看
- **功能**:
  - 多维度筛选
  - 分页显示
  - 详细信息查看
  - 统计数据展示

### 2. 日志接收

#### 从C++后端接收日志

系统通过WebSocket自动接收来自C++实盘框架的日志消息：

```javascript
// 后端发送的日志格式
{
  "type": "log",
  "data": {
    "level": "info",        // debug | info | warning | error
    "source": "backend",    // 日志来源
    "message": "交易执行成功",
    "timestamp": 1702345678000,
    "data": {               // 可选的附加数据
      "orderId": "12345",
      "symbol": "BTC-USDT"
    }
  }
}
```

#### C++后端集成示例

```cpp
// C++ UI服务器发送日志到前端
void sendLogToFrontend(const std::string& level, const std::string& message) {
    json log_msg = {
        {"type", "log"},
        {"timestamp", getCurrentTimestamp()},
        {"data", {
            {"level", level},
            {"source", "backend"},
            {"message", message}
        }}
    };
    
    // 通过WebSocket广播给所有连接的前端
    broadcastMessage(log_msg.dump());
}

// 使用示例
sendLogToFrontend("info", "策略已启动");
sendLogToFrontend("warning", "行情数据延迟");
sendLogToFrontend("error", "订单提交失败");
```

### 3. 日志发送

#### 从前端发送日志到后端

前端可以向C++后端发送日志消息，用于：
- 前端错误报告
- 用户操作记录
- 性能监控数据
- 调试信息

```javascript
// 使用LogStore发送日志
import { useLogStore } from '@/stores/log'

const logStore = useLogStore()

// 发送日志到后端
logStore.sendLogToBackend('info', '用户登录成功', {
  userId: '123',
  loginTime: Date.now()
})

logStore.sendLogToBackend('error', '网络请求失败', {
  url: '/api/orders',
  error: 'Network timeout'
})
```

#### 使用LogSender组件

在控制台视图中，可以使用LogSender组件手动发送日志：

1. 选择日志级别
2. 输入日志消息
3. (可选) 添加JSON格式的附加数据
4. 点击"发送"按钮

或使用快速发送按钮发送预定义的日志。

#### C++后端接收前端日志

```cpp
// C++ UI服务器接收前端日志
void handleFrontendLog(const json& message) {
    auto data = message["data"];
    std::string level = data["level"];
    std::string msg = data["message"];
    std::string source = data["source"];
    
    // 记录到文件或数据库
    logger->log(level, "[Frontend] " + msg);
    
    // 可选：触发特定处理逻辑
    if (level == "error") {
        // 发送告警通知
        sendAlert("前端错误: " + msg);
    }
}

// WebSocket消息处理
void onWebSocketMessage(const std::string& msg) {
    json message = json::parse(msg);
    std::string action = message["action"];
    
    if (action == "frontend_log") {
        handleFrontendLog(message);
    }
}
```

### 4. 日志配置

#### 前端配置

通过控制台视图的"配置"按钮可以设置：

- **后端日志级别**: 设置C++后端的最低日志级别
- **最大日志数量**: 前端保存的最大日志条数（防止内存溢出）
- **日志来源过滤**: 选择需要接收的日志来源
- **显示选项**: 时间戳、来源、彩色输出

#### 发送配置到后端

```javascript
// 前端发送日志配置到后端
wsClient.send('set_log_config', {
  level: 'info',
  sources: ['system', 'strategy', 'order']
})
```

#### C++后端处理配置

```cpp
// C++ UI服务器处理日志配置
void handleLogConfig(const json& message) {
    auto data = message["data"];
    std::string level = data["level"];
    auto sources = data["sources"];
    
    // 更新日志级别
    logger->setLevel(level);
    
    // 更新日志来源过滤
    for (const auto& source : sources) {
        logger->enableSource(source);
    }
    
    // 发送确认响应
    json response = {
        {"type", "response"},
        {"data", {
            {"success", true},
            {"message", "日志配置已更新"}
        }}
    };
    
    sendMessage(response.dump());
}
```

## WebSocket消息协议

### 前端 → 后端

#### 1. 发送日志
```json
{
  "action": "frontend_log",
  "timestamp": 1702345678000,
  "data": {
    "level": "info",
    "source": "frontend",
    "message": "用户点击了交易按钮",
    "data": {
      "button": "buy",
      "symbol": "BTC-USDT"
    }
  }
}
```

#### 2. 设置日志配置
```json
{
  "action": "set_log_config",
  "timestamp": 1702345678000,
  "data": {
    "level": "info",
    "sources": ["system", "strategy", "order"]
  }
}
```

### 后端 → 前端

#### 1. 日志消息
```json
{
  "type": "log",
  "timestamp": 1702345678000,
  "data": {
    "level": "info",
    "source": "backend",
    "message": "订单已提交",
    "data": {
      "orderId": "12345"
    }
  }
}
```

#### 2. 快照中的批量日志
```json
{
  "type": "snapshot",
  "timestamp": 1702345678000,
  "data": {
    "logs": [
      {
        "level": "info",
        "source": "system",
        "message": "系统启动",
        "timestamp": 1702345670000
      },
      {
        "level": "info",
        "source": "strategy",
        "message": "策略加载完成",
        "timestamp": 1702345675000
      }
    ]
  }
}
```

## 日志级别说明

| 级别    | 用途                     | 颜色标识 |
|---------|--------------------------|----------|
| debug   | 调试信息，详细的运行状态 | 灰色     |
| info    | 一般信息，正常操作记录   | 青色     |
| warning | 警告信息，需要注意的问题 | 黄色     |
| error   | 错误信息，系统异常       | 红色     |

## 日志来源说明

| 来源     | 说明                   |
|----------|------------------------|
| frontend | 前端应用               |
| backend  | C++后端                |
| system   | 系统级别操作           |
| strategy | 交易策略               |
| order    | 订单管理               |
| account  | 账户管理               |
| market   | 行情数据               |

## 最佳实践

### 1. 合理使用日志级别

```javascript
// ✅ 正常操作使用 info
logStore.sendLogToBackend('info', '用户登录成功')

// ✅ 潜在问题使用 warning
logStore.sendLogToBackend('warning', '接口响应时间超过2秒')

// ✅ 严重错误使用 error
logStore.sendLogToBackend('error', 'API请求失败', { code: 500 })

// ✅ 开发调试使用 debug
logStore.sendLogToBackend('debug', '组件渲染完成', { componentName: 'OrderList' })
```

### 2. 包含上下文信息

```javascript
// ✅ 好的日志 - 包含完整上下文
logStore.sendLogToBackend('error', '订单提交失败', {
  orderId: '12345',
  symbol: 'BTC-USDT',
  price: 50000,
  quantity: 0.1,
  error: 'Insufficient balance'
})

// ❌ 不好的日志 - 信息不足
logStore.sendLogToBackend('error', '失败了')
```

### 3. 避免敏感信息

```javascript
// ❌ 不要记录敏感信息
logStore.sendLogToBackend('info', '用户登录', {
  password: '123456',  // ❌ 不要记录密码
  apiKey: 'abc123'     // ❌ 不要记录API密钥
})

// ✅ 正确的做法
logStore.sendLogToBackend('info', '用户登录', {
  userId: '123',
  loginTime: Date.now()
})
```

### 4. 控制日志数量

```javascript
// ❌ 避免在循环中大量记录日志
for (let i = 0; i < 10000; i++) {
  logStore.sendLogToBackend('debug', `处理第${i}条数据`)  // ❌
}

// ✅ 批量记录或定期记录
logStore.sendLogToBackend('info', '批量处理完成', {
  totalCount: 10000,
  successCount: 9999,
  failCount: 1
})
```

## 性能考虑

1. **前端日志缓存**: 最多保存10000条日志，超出自动删除最旧的
2. **WebSocket延迟**: 平均1-4ms
3. **日志快照频率**: 每100ms推送一次批量日志
4. **增量事件**: 重要日志立即推送，不等待快照周期

## 故障排查

### 日志不显示

1. 检查WebSocket连接状态（查看控制台工具栏的连接状态）
2. 检查日志级别过滤设置
3. 检查日志来源过滤设置
4. 查看浏览器控制台是否有错误

### 日志发送失败

1. 确认WebSocket已连接
2. 检查后端是否正常运行
3. 查看网络请求是否被拦截
4. 检查消息格式是否正确

### 性能问题

1. 降低后端日志级别（减少日志量）
2. 减少前端最大日志数量
3. 禁用不需要的日志来源
4. 定期清空日志

## 示例：完整集成流程

### 前端代码

```javascript
// 在组件中使用日志
<script setup>
import { useLogStore } from '@/stores/log'
import { onMounted } from 'vue'

const logStore = useLogStore()

onMounted(() => {
  // 发送启动日志
  logStore.sendLogToBackend('info', '订单管理页面已加载')
})

function handleOrderSubmit(order) {
  try {
    // 提交订单逻辑...
    
    // 记录成功日志
    logStore.sendLogToBackend('info', '订单提交成功', {
      orderId: order.id,
      symbol: order.symbol
    })
  } catch (error) {
    // 记录错误日志
    logStore.sendLogToBackend('error', '订单提交失败', {
      error: error.message,
      order: order
    })
  }
}
</script>
```

### C++后端代码

```cpp
#include "ui_server.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class UIServer {
public:
    void sendLog(const std::string& level, const std::string& message, 
                 const json& data = nullptr) {
        json log_msg = {
            {"type", "log"},
            {"timestamp", getCurrentTimestamp()},
            {"data", {
                {"level", level},
                {"source", "backend"},
                {"message", message}
            }}
        };
        
        if (!data.is_null()) {
            log_msg["data"]["data"] = data;
        }
        
        broadcastToAllClients(log_msg.dump());
    }
    
    void onMessage(const std::string& msg) {
        try {
            json message = json::parse(msg);
            std::string action = message["action"];
            
            if (action == "frontend_log") {
                handleFrontendLog(message);
            } else if (action == "set_log_config") {
                handleLogConfig(message);
            }
        } catch (const std::exception& e) {
            sendLog("error", "消息处理失败: " + std::string(e.what()));
        }
    }
    
private:
    void handleFrontendLog(const json& message) {
        auto data = message["data"];
        std::string level = data["level"];
        std::string msg = data["message"];
        
        // 记录到文件
        logger_->log("[Frontend] [" + level + "] " + msg);
        
        // 发送确认
        json response = {
            {"type", "response"},
            {"data", {
                {"success", true},
                {"message", "日志已接收"}
            }}
        };
        sendMessage(response.dump());
    }
};
```

## 总结

新的日志系统提供了：
- ✅ 双向日志通信（前端 ↔️ C++后端）
- ✅ 实时滚动的控制台视图
- ✅ 结构化的表格视图
- ✅ 灵活的日志配置
- ✅ 完整的过滤和搜索功能
- ✅ 日志导出功能
- ✅ 低延迟的WebSocket通信

使用这个系统可以方便地监控整个交易系统的运行状态，快速定位和解决问题。

