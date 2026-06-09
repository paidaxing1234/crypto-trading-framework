# 配置指南（Configuration Guide）

> 🔐 **安全第一**：本仓库**不包含任何真实密钥**。所有真实配置均被 `.gitignore` 排除，仓库仅提供脱敏的 `*.example.json` 模板。
> 使用时请复制模板为真实文件（去掉 `.example`），填入你自己的密钥，**且永远不要把填好的真实配置提交到 Git**。

## 使用方法

每个配置目录下都有一个 `xxx.example.json` 模板。复制并改名为框架实际读取的文件名，然后填入真实值：

```bash
cp cpp/strategies/acount_configs/binance_account.example.json \
   cpp/strategies/acount_configs/binance_main.json
```

## 配置清单

| 配置类型 | 模板位置 | 说明 | 含密钥 |
|---------|---------|------|:---:|
| 交易所账户 | `cpp/strategies/acount_configs/binance_account.example.json` | 账户 ID + 交易所 API Key/Secret | ✅ |
| 前端用户 | `cpp/user_configs/user.example.json` | 用户名 + 角色 + 密码哈希 + salt | ✅ |
| 交易告警-邮件 | `cpp/trading/alerts/email_config.example.json` | SMTP 邮件告警 | ✅ |
| 交易告警-飞书 | `cpp/trading/alerts/lark_config.example.json` | 飞书 webhook + app secret | ✅ |
| 网络监控 | `cpp/totalconfig/network_monitor_config.example.json` | VPN/代理监控 + 邮件告警 | ✅ |
| 网络告警邮件 | `cpp/totalconfig/email_alert_network.example.json` | 网络监控专用邮件配置 | ✅ |
| 策略配置 | `cpp/strategies/configs/strategy.example.json` | 策略参数 + 交易所密钥 + 风控 | ✅ |
| 账户↔策略绑定 | `cpp/strategies/strategy_configs/account_strategy_binding.example.json` | 实例化运行参数 | ✅ |
| 初始资金 | `cpp/strategies/configs/initial_capital.example.json` | 各账户初始权益 | ❌ |
| 全局风控 | `cpp/risk_config.json` | 限额/回撤/频率限制（无密钥） | ❌ |

## 关键字段说明

### 交易所账户（acount_configs）
- `api_key` / `secret_key`：交易所 API 密钥。**建议仅授予“合约交易”权限，绑定服务器 IP 白名单，关闭提币权限。**
- `is_testnet`：`true` 走测试网，`false` 走实盘。**实盘务必小资金验证后再放量。**

### 前端用户（user_configs）
- `password_hash` = SHA256(`salt` + 明文密码)；`salt` 为随机 hex。**切勿存明文密码。**
- `role`：`SUPER_ADMIN` / `STRATEGY_MANAGER` 等。

### 告警（email / lark）
- 邮件 `smtp_password` 为邮箱**授权码**（非登录密码）。
- 飞书 `secret` / `app_secret` 为机器人签名密钥与应用密钥。

### Redis
- `redis.password` 生产环境务必设置，并禁止公网暴露 6379 端口。

## ⚠️ 安全红线（务必遵守）

1. 真实 `*.json`（非 `.example`）一律不提交，已在 `.gitignore` 中拦截。
2. API Key 绑定 IP 白名单 + 关闭提币权限。
3. 一旦密钥疑似泄露，**立即去交易所/邮箱/飞书后台吊销并重置**。
4. 服务器 Redis / ClickHouse 不要暴露公网，使用防火墙 + 强密码。
