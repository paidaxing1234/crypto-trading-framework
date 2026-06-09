# OKX 适配器实现

## 📁 文件结构

```
adapters/okx/
├── okx_adapter.h           # OKX适配器组件（头文件）
├── okx_rest_api.h          # REST API接口定义（头文件）
├── okx_rest_api.cpp        # REST API实现 ✅ NEW
├── okx_websocket.h         # WebSocket接口定义（头文件）
├── OKX_API使用说明.md      # API使用文档 ✅ NEW
└── README.md               # 本文档 ✅ NEW
```

## ✨ 新增功能

### 1. REST API 实现 (`okx_rest_api.cpp`)

完整的 REST API C++ 实现，包括：

- **签名算法**：HMAC SHA256 + Base64
- **HTTP 请求**：基于 libcurl
- **JSON 处理**：使用 nlohmann/json
- **时间戳生成**：ISO 8601 格式

### 2. get_account_instruments 接口

新增的账户交易产品查询接口：

```cpp
nlohmann::json get_account_instruments(
    const std::string& inst_type,      // 产品类型（必填）
    const std::string& inst_family,    // 交易品种（可选）
    const std::string& inst_id         // 产品ID（可选）
);
```

**功能**：
- 查询账户可交易的产品列表
- 获取产品详细信息（精度、限额等）
- 支持现货、杠杆、永续、交割、期权

**参考实现**：Python 版本 `adapters/okx/rest_api.py`

## 🚀 快速开始

### 1. 安装依赖

**macOS**:
```bash
brew install curl openssl nlohmann-json
```

**Ubuntu/Debian**:
```bash
sudo apt install libcurl4-openssl-dev libssl-dev nlohmann-json3-dev
```

### 2. 使用示例

```cpp
#include "adapters/okx/okx_rest_api.h"

using namespace trading::okx;

int main() {
    // 创建API客户端
    OKXRestAPI api(api_key, secret_key, passphrase, false);
    
    // 查询现货产品
    auto result = api.get_account_instruments("SPOT");
    
    if (result["code"] == "0") {
        std::cout << "产品数量: " << result["data"].size() << std::endl;
    }
    
    return 0;
}
```

### 3. 编译

在 `CMakeLists.txt` 中添加：

```cmake
# 查找依赖
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

# OKX适配器库
add_library(okx_adapter
    adapters/okx/okx_rest_api.cpp
    # 未来添加 okx_websocket.cpp
    # 未来添加 okx_adapter.cpp
)

target_link_libraries(okx_adapter
    PUBLIC trading_core
    PRIVATE CURL::libcurl
    PRIVATE OpenSSL::SSL
    PRIVATE OpenSSL::Crypto
    PRIVATE nlohmann_json::nlohmann_json
)
```

### 4. 运行测试

```bash
cd build
cmake ..
cmake --build .
./test_okx_api
```

## 📚 已实现的接口

### 交易接口

- ✅ `place_order()` - 下单
- ✅ `cancel_order()` - 撤单
- ✅ `cancel_batch_orders()` - 批量撤单
- ✅ `get_order()` - 查询订单
- ✅ `get_pending_orders()` - 查询未成交订单

### 账户接口

- ✅ `get_account_balance()` - 查询余额
- ✅ `get_positions()` - 查询持仓
- ✅ `get_account_instruments()` - 获取交易产品信息 ⭐ NEW

### 市场数据接口

- ✅ `get_candles()` - 查询K线

## ⏳ 待实现的接口

### REST API

- ⏳ `place_batch_orders()` - 批量下单
- ⏳ `amend_order()` - 修改订单
- ⏳ `get_orders_history()` - 查询历史订单
- ⏳ `get_bills()` - 账单流水查询
- ⏳ 更多市场数据接口...

### WebSocket

- ⏳ `okx_websocket.cpp` - WebSocket 实现
- ⏳ 公共频道订阅（ticker, trades, orderbook, kline）
- ⏳ 私有频道订阅（orders, positions, account）
- ⏳ 心跳和重连机制

### 适配器组件

- ⏳ `okx_adapter.cpp` - 统一适配器实现
- ⏳ 事件转换（OKX数据 → 框架事件）
- ⏳ 订单映射管理

## 🎯 设计特点

### 1. 与 Python 版本保持一致

- ✅ 相同的接口命名
- ✅ 相同的参数顺序
- ✅ 相同的返回格式
- ✅ 易于从 Python 迁移

### 2. 现代 C++ 特性

- ✅ 使用 `nlohmann::json` 处理 JSON
- ✅ 使用 `std::string` 管理字符串
- ✅ 使用异常处理错误
- ✅ 清晰的接口设计

### 3. 安全性

- ✅ HMAC SHA256 签名
- ✅ 时间戳防重放
- ✅ HTTPS 加密传输
- ✅ 参数验证

### 4. 性能

- ✅ 高效的 C++ 实现
- ✅ 最小化内存分配
- ✅ 复用 CURL 连接（可优化）

## 📖 文档

- `OKX_API使用说明.md` - 详细的 API 使用文档
- `examples/test_okx_api.cpp` - 完整的测试示例
- [OKX 官方文档](https://www.okx.com/docs-v5/zh/)

## 🔧 开发建议

### 添加新接口的步骤

1. **在头文件中添加声明** (`okx_rest_api.h`)
   ```cpp
   nlohmann::json your_new_api(...);
   ```

2. **在实现文件中添加实现** (`okx_rest_api.cpp`)
   ```cpp
   nlohmann::json OKXRestAPI::your_new_api(...) {
       // 构造参数
       // 调用 send_request
       // 返回结果
   }
   ```

3. **添加测试代码** (`examples/test_okx_api.cpp`)
   ```cpp
   auto result = api.your_new_api(...);
   // 验证结果
   ```

4. **更新文档** (`OKX_API使用说明.md`)
   - 添加接口说明
   - 添加使用示例
   - 添加参数说明

### 参考实现

所有新接口都可以参考 Python 版本的实现：
- `Real-account-trading-framework/python/adapters/okx/rest_api.py`

## ⚠️ 注意事项

1. **API 凭证**：不要将 API Key 硬编码，使用配置文件
2. **限速控制**：注意 API 调用频率限制
3. **错误处理**：始终检查返回的 code 字段
4. **网络异常**：捕获并处理网络异常
5. **线程安全**：当前实现不是线程安全的，多线程使用需要加锁

## 🔗 相关链接

- [框架 README](../../README.md)
- [架构说明](../../架构说明.md)
- [Python 版本对比](../../PYTHON_CPP_对比.md)
- [OKX API 官方文档](https://www.okx.com/docs-v5/zh/)

---

**版本**: v1.0.0  
**更新时间**: 2025-12-08  
**状态**: 🚧 开发中

