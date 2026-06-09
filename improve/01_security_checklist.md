# 上线安全清单 - 逐项修复指南

> 本文档供 Claude 在目标服务器上逐项执行修复。每个条目包含：问题描述、涉及文件、具体修改方法。
> 执行前请先通读全文，确认环境匹配后再逐条修改。

---

## P0-1: Redis 安全加固

**问题**: Redis 绑定 0.0.0.0:6379，无密码，protected-mode 关闭，任何人可连接。

**配置文件**: `/home/<USER>/redis-local/etc/redis.conf`

**修改步骤**:
1. 修改 `redis.conf`：
   - 将 `bind 0.0.0.0 -::1` 改为 `bind 127.0.0.1 -::1`
   - 添加 `requirepass <生成一个32位随机密码>`（记录此密码，后续多处需要引用）
   - 将 `protected-mode no` 改为 `protected-mode yes`
2. 重启 Redis：`redis-cli shutdown && redis-server /home/<USER>/redis-local/etc/redis.conf`
3. 全局搜索项目中所有 Redis 连接代码，补上密码参数：
   - Python: `redis.Redis(...)` 调用加 `password=os.getenv("REDIS_PASSWORD")`
   - C++: `redisCommand(context_, "AUTH %s", password)` 连接后认证
   - 涉及文件（grep "Redis\|redis" 找全）：
     - `cpp/scripts/fast_kline_filler.py` (get_redis 函数)
     - `cpp/scripts/preload_klines_to_redis.py`
     - `cpp/server/managers/redis_data_provider.cpp`
     - `cpp/server/managers/redis_recorder.cpp`
     - `cpp/server/klinedata/data_recorder.cpp`
     - `cpp/strategies/core/` 下所有 Redis 相关文件

---

## P0-2: API Key 从配置文件移到环境变量

**问题**: 交易所 API Key / Secret 明文写在 JSON 配置文件中。

**涉及文件**:
- `cpp/strategies/strategy_configs/binance_ycx_ret_skew_binance_btc_main.json`（实盘）
- `cpp/strategies/strategy_configs/binance_ycx2_five_mom_factor_binance_testnet.json`（测试网）
- 其他 `strategy_configs/*.json` 中含 `api_key` / `secret_key` 的文件

**修改步骤**:
1. 在服务器上创建 `/home/<USER>/.trading_env`，写入：
   ```bash
   export BINANCE_MAIN_API_KEY="<实盘key>"
   export BINANCE_MAIN_SECRET_KEY="<实盘secret>"
   export BINANCE_TESTNET_API_KEY="<测试网key>"
   export BINANCE_TESTNET_SECRET_KEY="<测试网secret>"
   ```
   权限设为 `chmod 600 /home/<USER>/.trading_env`
2. 在 `start_all.sh` 开头加 `source /home/<USER>/.trading_env`
3. 修改 JSON 配置文件，将 `api_key` / `secret_key` 的值改为空字符串 `""`
4. 修改策略加载配置的代码，优先从环境变量读取：
   - 找到读取 JSON config 中 `api_key` / `secret_key` 的 C++ 代码（在 adapter 或 strategy loader 中）
   - 加逻辑：若 JSON 中为空则从 `std::getenv("BINANCE_MAIN_API_KEY")` 读取
5. 将 `strategy_configs/*.json` 加入 `.gitignore`，或用 `.json.template` 模板替代
6. 清除 git 历史中的密钥：`git filter-branch` 或 BFG Repo-Cleaner

---

## P0-3: 所有服务绑定 127.0.0.1

**问题**: WebSocket(8002)、ZMQ(5556) 等服务绑定 0.0.0.0，对外暴露。

**涉及文件及修改**:

1. **WebSocket 服务端**:
   - 文件: `cpp/server/trading_server_main.cpp`
   - 找到 `g_frontend_server->start("0.0.0.0", 8002)`
   - 改为 `g_frontend_server->start("127.0.0.1", 8002)`

2. **默认配置**:
   - 文件: `cpp/core/config_center.h`
   - 找到 `std::string bind_address = "0.0.0.0"`
   - 改为 `std::string bind_address = "127.0.0.1"`

3. **前端 Vite dev server**:
   - 文件: `实盘框架前端页面/vite.config.js`
   - 找到 `host: true`
   - 改为 `host: false`
   - 生产环境应用 `npm run build` 生成静态文件，用 nginx 托管

---

## P0-4: 删除默认管理员账号和 Mock 登录

**问题**: 硬编码的 `admin`/`********` 超级管理员 + 前端 mock 登录绕过。

**涉及文件及修改**:

1. **C++ 后端默认账号**:
   - 文件: `cpp/network/auth_manager.h`
   - 找到:
     ```cpp
     if (!has_super_admin) {
         add_user("admin", "********", UserRole::SUPER_ADMIN);
     }
     ```
   - 改为从环境变量读取：`add_user("admin", std::getenv("ADMIN_PASSWORD"), UserRole::SUPER_ADMIN);`
   - 若环境变量为空则打印错误并退出，不创建弱密码账号

2. **前端 Mock 登录**:
   - 文件: `实盘框架前端页面/src/stores/user.js`
   - 找到约第 223-238 行的 mock 登录逻辑（`username === 'admin' && credentials.password === '********'`）
   - 删除整个 mock 分支，只保留 WebSocket 认证路径

3. **JWT Secret**:
   - 文件: `cpp/network/auth_manager.h`
   - 找到 `"CHANGE_ME_SET_A_STRONG_JWT_SECRET"` 默认值
   - 改为从环境变量 `JWT_SECRET` 读取，若为空则生成随机 256 位密钥并持久化到本地文件

---

## P0-5: 防火墙配置

**操作步骤**（在目标服务器执行）:
```bash
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow 22/tcp          # SSH
sudo ufw allow 80/tcp          # nginx HTTP（重定向到 HTTPS）
sudo ufw allow 443/tcp         # nginx HTTPS
sudo ufw enable
```

只对外暴露 SSH + nginx，其余所有端口仅通过 127.0.0.1 本地访问。

---

## P1-1: 敏感配置文件权限

**操作**:
```bash
chmod 600 cpp/trading/alerts/email_config.json
chmod 600 cpp/trading/alerts/lark_config.json
chmod 600 cpp/totalconfig/network_monitor_config.json
chmod 600 cpp/strategies/strategy_configs/*.json
```

**长期**: 将这些文件中的密钥也迁移到环境变量，JSON 中只保留非敏感配置。

---

## P1-2: Nginx 反向代理 + TLS

**部署架构**:
```
用户浏览器 --[HTTPS/WSS]--> nginx:443 --[HTTP/WS]--> localhost:8002 (WebSocket)
                                       --[HTTP]-----> localhost:3000 (前端静态文件)
```

**步骤**:
1. 申请 SSL 证书（Let's Encrypt: `certbot --nginx`）
2. 前端用 `npm run build` 构建，nginx 托管 `dist/` 静态文件
3. WebSocket 反代配置 `proxy_pass http://127.0.0.1:8002` + `Upgrade` 头
4. 前端 `.env.production` 中 `VITE_WS_URL=wss://你的域名/ws`

---

## P1-3: 前端交易所凭证处理

**问题**: 前端 `AddAccountDialog.vue` 直接收集 apiKey/secretKey 通过 WebSocket 明文发送。

**修改思路**:
- 有了 nginx + TLS 后，WebSocket 通道本身是加密的（wss://），传输安全问题已解决
- 但仍建议：后端收到凭证后加密存储（AES），不以明文保存

---

## 执行顺序建议

1. P0-5 防火墙（最快生效，兜底保护）
2. P0-1 Redis 加固
3. P0-3 服务绑定 127.0.0.1
4. P0-4 删除默认账号
5. P0-2 API Key 环境变量化
6. P1-2 Nginx + TLS
7. P1-1 文件权限
8. P1-3 凭证加密存储
