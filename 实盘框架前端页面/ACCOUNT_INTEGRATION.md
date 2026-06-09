# 前端账户管理集成说明

## 概述

前端已集成多账户管理系统，支持：
- ✅ 多策略独立账户
- ✅ OKX 和 Binance 双交易所
- ✅ 通过 WebSocket 动态注册/注销
- ✅ 实盘/模拟盘切换

---

## 主要修改

### 1. 账户注册对话框 (`AddAccountDialog.vue`)

**新增字段**：
- `strategyId`: 策略标识符（留空则为默认账户）
- `exchange`: 交易所选择（okx / binance）
- `isTestnet`: 模拟盘开关

**动态表单**：
- OKX 需要 Passphrase
- Binance 不需要 Passphrase（自动隐藏）

### 2. 账户 API (`api/account.js`)

**注册账户**：
```javascript
async addAccount(data) {
  const msg = {
    type: 'register_account',
    strategy_id: data.strategyId || 'default',
    exchange: data.exchange,
    api_key: data.apiKey,
    secret_key: data.secretKey,
    passphrase: data.passphrase || '',
    is_testnet: data.isTestnet
  }
  wsClient.send(msg)
}
```

**注销账户**：
```javascript
async deleteAccount(id) {
  const msg = {
    type: 'unregister_account',
    strategy_id: id
  }
  wsClient.send(msg)
}
```

### 3. 账户列表页面 (`views/Account.vue`)

**显示优化**：
- 交易所标签（OKX / Binance）
- 策略 ID 显示
- 实盘/模拟盘状态
- 简化操作按钮（同步 / 注销）

---

## 使用流程

### 注册新账户

1. 点击"注册账户"按钮
2. 填写表单：
   - **策略ID**：留空为默认账户，填写则为策略专用账户
   - **交易所**：选择 OKX 或 Binance
   - **API Key / Secret Key**：交易所 API 凭证
   - **Passphrase**：仅 OKX 需要
   - **模拟盘**：开启则使用测试环境
3. 点击"添加"

前端会通过 WebSocket 发送 `register_account` 消息到后端。

### 注销账户

1. 在账户列表中找到目标账户
2. 点击"注销"按钮
3. 确认操作

前端会通过 WebSocket 发送 `unregister_account` 消息到后端。

---

## WebSocket 消息格式

### 注册账户

```json
{
  "type": "register_account",
  "strategy_id": "grid_strategy_1",
  "exchange": "okx",
  "api_key": "xxx",
  "secret_key": "yyy",
  "passphrase": "zzz",
  "is_testnet": true
}
```

### 注销账户

```json
{
  "type": "unregister_account",
  "strategy_id": "grid_strategy_1"
}
```

---

## 后端对接

后端需要在 WebSocket 消息处理中添加：

```cpp
// 处理注册账户消息
if (msg_type == "register_account") {
    std::string strategy_id = msg.value("strategy_id", "");
    std::string exchange = msg.value("exchange", "okx");
    std::string api_key = msg.value("api_key", "");
    std::string secret_key = msg.value("secret_key", "");
    std::string passphrase = msg.value("passphrase", "");
    bool is_testnet = msg.value("is_testnet", true);

    ExchangeType ex_type = string_to_exchange_type(exchange);
    bool success = g_account_registry.register_account(
        strategy_id, ex_type, api_key, secret_key, passphrase, is_testnet
    );

    // 返回结果给前端
    nlohmann::json response;
    response["type"] = "register_account_response";
    response["success"] = success;
    response["strategy_id"] = strategy_id;
    // 发送响应...
}

// 处理注销账户消息
if (msg_type == "unregister_account") {
    std::string strategy_id = msg.value("strategy_id", "");
    // 需要知道交易所类型，可以从注册表查询或前端传递
    bool success = g_account_registry.unregister_account(strategy_id, ExchangeType::OKX);

    // 返回结果...
}
```

---

## 数据流

```
用户操作 → 前端表单 → WebSocket 消息 → 后端 AccountRegistry → 交易所 API
                                              ↓
                                        WebSocket 推送
                                              ↓
                                        前端更新列表
```

---

## 注意事项

1. **安全性**：
   - API 密钥通过 WebSocket 传输，确保使用 WSS（加密）
   - 后端应验证 API 密钥格式和权限

2. **错误处理**：
   - 前端需要监听后端返回的 `register_account_response` 消息
   - 显示注册成功/失败提示

3. **状态同步**：
   - 账户列表数据通过 WebSocket 快照推送
   - 前端 `account.js` store 监听 `snapshot` 事件更新数据

4. **默认账户**：
   - `strategyId` 为空时，后端应将其注册为默认账户
   - 默认账户用于未注册策略的订单

---

## 测试清单

- [ ] 注册 OKX 账户（实盘）
- [ ] 注册 OKX 账户（模拟盘）
- [ ] 注册 Binance 账户
- [ ] 注册默认账户（strategyId 为空）
- [ ] 注册策略专用账户
- [ ] 注销账户
- [ ] 切换交易所时 Passphrase 字段显示/隐藏
- [ ] 表单验证（必填项）
- [ ] WebSocket 消息发送成功
- [ ] 后端响应处理

---

**更新时间**: 2026-01
**维护者**: Sequence Team
