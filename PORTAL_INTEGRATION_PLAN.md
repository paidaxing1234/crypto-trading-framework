# 方案：将实盘交易看板接入 system.your-domain.com/monitor

> 交付对象：portal 服务器上的 Claude（实现方）。
> 本文是**自包含实现规格**：即使拿不到 trading 仓库源码，也能照本文 + 数据契约实现；若能拿到 trading 前端源码（推荐），大部分页面可直接搬。

---

## 0. 已确认的三个决策

| 决策 | 选择 |
|---|---|
| 功能范围 | **全功能，与主站一致**（含启停/创建/删除策略、注册/注销账户、下单、止盈止损、改策略代码等"写"操作）；**唯一不做**：用户管理（/users）。Orders/Positions/手动交易**一并做齐**。 |
| 前端形态 | `/monitor` 做成**独立 Vue3 + Element Plus SPA 子应用**，由 portal 的 FastAPI 挂在 `/monitor` 路径当静态资源伺服 → 几乎原样搬 trading 看板（与主站一致）。portal 本体保持 Jinja2，不受影响。 |
| 集成方式 | **portal FastAPI 后端代理 + 专用服务账号 `portal_svc`**：FastAPI 用该账号登录 trading 后端并转发；portal 浏览器只和 portal 同源通信。 |
| 部署拓扑 | trading 与 portal **不同机** → portal 后端代理到 trading 的**公网 https 端点**（`wss://trading.your-domain.com/ws`、`https://trading.your-domain.com/stats-api/`），不碰内网端口。 |
| 鉴权 | 复用 portal 现有 **JWT HTTP-only Cookie `sqt_token`**；代理/中介在握手时校验该 cookie + `require_module("monitor")`。 |
| 写操作授权 | 仅 **策略所有人 + 管理员** 可执行"写"action；其余角色只读。中介层做闸门 + 审计。 |
| 实时推送 | snapshot **节流到 1s**（中介层聚合下发，降带宽）。 |

### Portal 后端栈（实现方环境，务必遵守其约定）
- **FastAPI 0.115 + Uvicorn（单 worker）**，Python 3.10.19；systemd `sqt-portal.service`，端口 9100。
- 认证：python-jose JWT + passlib bcrypt，**HTTP-only Cookie `sqt_token`**。
- HTTP 客户端：**httpx (async)**；WS：**websockets 13.1**；文件：aiofiles。
- 前端：Jinja2 模板 + vanilla JS + 全局 `api()` 助手（`static/js/common.js`）；另有**基本闲置的** Vue SPA `frontend/`（正好可承载 /monitor 子应用）。
- 约束：**单 worker**（monitor 写进程内 `core.state` 全局态，禁多进程）；同步阻塞调用须 `run_in_executor`；权限用 `Depends(require_module("xxx"))`；DB `async with get_*_db() as db:`；错误 `raise HTTPException(code, detail="中文")`。
- → 因此本方案的**中介与代理用 FastAPI 原生 async 实现**（见 §6），不要引入 Node。

---

## 1. 总体架构

```
┌─────────────────────────────────────────────────────────────────┐
│  浏览器 (portal 用户, 已登录 WBL)                                  │
│  Vue3 + Element Plus  ——  路由 base = /monitor                    │
│    │ 同源 WebSocket  wss://system.your-domain.com/monitor/ws       │
│    │ 同源 HTTP       https://system.your-domain.com/monitor/stats-api/* │
└────┼──────────────────────────────────────────────────────────────┘
     │  (只和 portal 同源, 不直接暴露 trading)
┌────▼──────────────────────────────────────────────────────────────┐
│  portal 后端 (system.your-domain.com)                              │
│  ┌──────────────────────┐   ┌──────────────────────────────────┐  │
│  │ WS 中介 (mediator)    │   │ HTTP 代理                         │  │
│  │ /monitor/ws           │   │ /monitor/stats-api/*  →  trading │  │
│  │  · 校验 portal 登录态  │   │   /stats-api/*                    │  │
│  │  · 用服务账号登录上游  │   └──────────────────────────────────┘  │
│  │  · 按 portal 角色过滤"写"                                       │  │
│  └─────────┬────────────┘                                          │
└────────────┼───────────────────────────────────────────────────────┘
             │  专用服务账号 (portal_svc)
┌────────────▼───────────────────────────────────────────────────────┐
│  trading.your-domain.com                                            │
│   · C++ 实盘服务 WebSocket  /ws → 127.0.0.1:8002                     │
│       (账户/策略/持仓/订单/日志/控制, 需 login 鉴权)                  │
│   · stats-api HTTP  /stats-api → 127.0.0.1:8003                      │
│       (净值曲线/夏普/收益率/成交笔数, 无鉴权, 已绑 127.0.0.1)         │
└─────────────────────────────────────────────────────────────────────┘
```

**两个数据后端：**
1. **C++ WebSocket**（实时、有状态、需登录）——所有账户/策略/持仓/订单/日志/控制类操作。
2. **stats-api HTTP**（只读、无状态、无鉴权）——净值曲线、夏普、最大回撤、收益率、成交笔数。

portal 后端要新增**两个代理**：HTTP 代理（简单转发）+ WS 中介（带鉴权注入）。

---

## 2. 鉴权设计（核心）

### 2.1 trading 的登录协议（已核实）
- 登录：浏览器/客户端发原始消息 `{ "type":"login", "username":"...", "password":"..." }`
- 返回：`{ "type":"login_response", "success":true, "token":"...", "user":{ "username","role","allowed_strategies?" } }`
- **鉴权按连接(client_id)记忆**：一条 WS 连接登录成功后，该连接后续所有命令即视为已认证（无需每条带 token）。断线重连＝新连接，需重新登录。
- 全功能需要 admin/超管级角色（角色体系含 SUPER_ADMIN / STRATEGY_MANAGER 等；`add_user` action 存在，说明支持建账号）。

### 2.2 服务账号
- 在 trading 后端**新建一个专用账号** `portal_svc`，赋**全权限角色**（能跑所有"写"action）。
- portal 后端持有该账号密码（放 portal 的 secret 配置，**绝不下发到浏览器**）。

### 2.3 WS 中介鉴权流程（推荐：每个浏览器会话 1:1 上游连接）
```
浏览器连接 /monitor/ws  (同源, 自动带 sqt_token cookie)
  └─ FastAPI 校验 sqt_token (JWT) + require_module("monitor")
       ├─ 未登录/无权限 → 关闭连接
       └─ 已登录 → 后端开一条到 trading wss://trading.your-domain.com/ws 的上游连接
                    └─ 上游 open 后立刻发 {type:'login', username:'portal_svc', password:***}
                       等到 login_response.success=true
  之后双向转发:
    浏览器 → 上游:  转发, 但"写"action 先按 portal 角色判定是否放行(见 2.4)
    上游 → 浏览器:  原样转发(response / snapshot / log 推送)
  特殊处理:
    · 浏览器发来的 {type:'login'}(前端遗留的登录步骤) 由中介"吞掉",
      直接回一个合成 {type:'login_response', success:true, token:'portal-proxy',
      user:{username:<portal用户名>, role:<portal角色>}} —— 这样复用的前端登录流程"照常工作",
      但实际上游是服务账号, portal 用户从不接触服务账号凭证。
```
> 为什么 1:1：每个浏览器各自一条上游连接，**requestId 天然隔离**、snapshot 推送天然各管各，实现最简单最稳。用户量大时再考虑"单条上游 + requestId 命名空间复用"做优化。

### 2.4 可见即可操作：按归属过滤 + 写闸门（已定）
**规则**：portal 里只有「策略所有人」和「管理员」能进 /monitor；**策略所有人只看得到自己的策略，管理员看得到全部；看得到即可操作。**
所以中介层要做**两件事**，都基于"该用户的可控集合 `owned`"（管理员 = 全部）：

**① 读过滤（关键，否则会泄露他人持仓财务）**——对下列返回按 `owned` 裁剪：
- `list_strategies` → 只留 `strategy_id ∈ owned` 的策略。
- `list_accounts` / `get_account_positions` → 只留 owned 策略对应的 `account_id`（策略=账户 1:1）。
- stats 代理（accounts_overview / account_stats / equity_curve）→ 同样按 owned account 过滤 / 拒绝（见 §6.1）。

**② 写闸门 + 审计**——写 action 必须 `strategy_id ∈ owned`（管理员放行全部），命中即记审计：
```
start_strategy, stop_strategy, create_strategy, delete_strategy,
register_account, unregister_account, save_strategy_source,
set_log_config, deactivate_kill_switch, place_order, cancel_order
（add_user / update_user / delete_user 一律禁）
```
- **`owned` 来源**：portal DB 维护 `user ↔ strategy_id`（含其 `account_id`）归属表；管理员=全部。`create_strategy` 成功后把新 strategy_id 记到创建者名下。
- **前端 `v-permission`** 仅作 UI 显隐（第一道）；中介过滤/闸门是权威（第二道）。

---

## 3. 数据契约（自包含）

### 3.1 C++ WebSocket  `wss://trading.your-domain.com/ws`
**信封**：请求 `{ action, data:{ requestId, ...params }, timestamp }`；响应 `{ type:"response", timestamp, data:{ success, message, ...payload, requestId } }`，用 `requestId` 关联（登录类用 `type` 关联）。

**认证类（无需登录即可发）**
| 消息 | 请求 | 响应 |
|---|---|---|
| login | `{type:'login',username,password}` | `{type:'login_response',success,token,user}` |
| logout | `{type:'logout',token}` | `{type:'logout_response',success}` |
| get_user_info | `{type:'get_user_info'}` | `{type:'user_info',success,user}` |

**账户**
| action | 请求 data | 响应 data |
|---|---|---|
| list_accounts | `{}` | `{ okx:[...], binance:[ {strategy_id,account_id,exchange,api_key(掩码),is_testnet,status,register_time,equity,unrealizedPnl,monitor_update_time} ] }` |
| register_account | `{account_id,exchange,api_key,secret_key,passphrase,is_testnet}` | `{success,message}` (超时15s) |
| unregister_account | `{strategy_id,account_id,exchange}` | `{success,message}` |
| get_account_positions | `{accountId}` | `[ {symbol,positionAmt,entryPrice,markPrice,unRealizedProfit,leverage,positionSide,notional,...} ]` |

**策略**
| action | 请求 data | 响应 data |
|---|---|---|
| list_strategies | `{}` | `[ {strategy_id,account_id,exchange,pid,status(running/stopped/pending/error),start_command,work_dir,start_time(ms),last_heartbeat} ]` |
| start_strategy | `{strategy_id,config}` | `{success,message}` |
| stop_strategy | `{strategy_id}` | `{success,message}` |
| create_strategy | `{config_file,strategy_id,account_id,strategy_name,symbol,python_file,qq_email,lark_email,description,auto_start}` | `{success,message}` (超时15s) |
| delete_strategy | `{strategy_id}` | `{success,message}` |
| list_strategy_configs | `{}` | `[ {filename,strategy_name,exchange,strategy_id,account_id,...} ]` |
| list_strategy_files | `{}` | `[ {filename,size} ]` |
| get_strategy_source | `{filename}` | `{content}` (超时15s) |
| save_strategy_source | `{filename,content}` | `{success,message}` (超时15s) |

**日志**
| action | 请求 data | 响应 data |
|---|---|---|
| get_strategy_log_files | `{strategyId}` | `[ {filename,size} ]` |
| get_strategy_logs | `{filename,tailLines}` | `{lines:[...]}` (超时15s) |
| get_system_log_files | `{}` | `[ {filename,size} ]` |
| get_system_logs | `{filename,tailLines}` | `{lines:[...]}` (超时15s) |
| download_log_file | `{filename,logType('strategy'/'system')}` | `{content}` (超时30s) |
| get_logs / get_log_dates / set_log_config | (系统日志控制台) | 见前端 Logs 页 |
| get_recent_orders | `{limit}` | 从策略日志解析的最近订单 |

**订单/风控（全功能需要）**
| action | 说明 |
|---|---|
| place_order / cancel_order | 手动下单/撤单（前端 orderStore 推送式，无 requestId） |
| get_risk_status / deactivate_kill_switch | 风控状态 / 解除一键停 |

**实时推送（上游主动下发，中介广播给浏览器）**
| 事件 | 频率 | 数据 |
|---|---|---|
| snapshot | ~100ms | `{orders,positions,strategies,...}` |
| order_filled | 成交时 | `{order_id,symbol,side,filled_quantity,filled_price}` |
| log | 实时 | `{level,source,message,timestamp,data}` |

### 3.2 stats-api HTTP（经 portal 代理为 `/monitor/stats-api/*`，CORS 已开）
| 端点 | 响应 |
|---|---|
| GET /api/accounts_overview | `{accounts:[ {account_id,exchange,equity,available,wallet,upnl,initial_capital,pnl,return_rate,trade_count,updated_at} ], server_time}` |
| GET /api/account_stats?account_id=X | `{account_id,latest:{...},initial_capital,trade_count,metrics:{points,sharpe,max_drawdown,annualized_return,volatility,total_return,total_pnl,span_days},server_time}` |
| GET /api/equity_curve?account_id=X&range=7d\|30d\|90d\|1y\|all | `{account_id,range,initial_capital,points:[{ts,equity,upnl}],metrics:{...},server_time}` |
| GET /api/health | `{ok,server_time}` |

> 注意：stats-api 在 trading 上**只绑 127.0.0.1 无鉴权**。portal 代理它时**务必加 portal 自己的登录校验**（否则等于把账户财务无鉴权暴露）。

---

## 4. 页面与布局规格（逐页）

> 复用主站同名页面；下方给出布局骨架 + 数据源。路由全部加 `/monitor` 前缀。

### 导航 / Layout（去掉"用户管理"）
```
┌──────────┬──────────────────────────────────────────────────────┐
│ [LOGO]   │  首页 / <当前页标题>        [WS●] [🔔] [🌓] [用户▾]      │  ← 顶栏
│          ├──────────────────────────────────────────────────────┤
│ 仪表板   │                                                        │
│ 策略管理 │                  <页面内容>                             │
│ 策略日志 │                                                        │
│ 账户管理 │                                                        │
│ 系统日志 │                                                        │
│ [«折叠]  │                                                        │
└──────────┴──────────────────────────────────────────────────────┘
```
- 侧边栏菜单（5 项，**删掉用户管理**）：仪表板`/monitor/dashboard`、策略管理`/monitor/strategy`、策略日志`/monitor/strategy-logs`、账户管理`/monitor/account`、系统日志`/monitor/logs`。
- 顶栏：面包屑 + **WS 连接状态灯**（绿/红，取 mediator 连接态）+ 通知角标 + 主题切换 + **用户菜单（接 portal 的用户/登出，不接 trading 的改密码）**。
- 隐藏路由：策略详情`/monitor/strategy/:id`、账户详情`/monitor/account/:id`。

### 4.1 仪表板 Dashboard
```
┌─总资产────┐ ┌─未实现盈亏─┐ ┌─运行策略数─┐ ┌─活跃订单──┐
│ Σequity   │ │ Σupnl     │ │ running/总 │ │ active    │
└───────────┘ └───────────┘ └───────────┘ └───────────┘
┌─运行中的策略 (Top5)────────────┐ ┌─最近订单 (Top5)──────────────┐
│ 名称 账户 状态 …  [查看全部→]   │ │ 交易对 方向 状态 …[查看全部→] │
└────────────────────────────────┘ └──────────────────────────────┘
```
数据源：`list_accounts`(总资产/未实现汇总) + `list_strategies`(运行数) + snapshot/orders(活跃订单)。

### 4.2 策略管理 Strategy
```
┌运行中┐┌待启动┐┌已停止┐┌异常┐                       [+ 添加策略]
└──────┘└──────┘└──────┘└────┘
┌策略列表─────────────────────────────────[搜索][状态筛选]┐
│名称│类型│账户│状态│盈亏│收益率│成交笔数│运行时长│操作    │
│… … … …  <tag> +N.NN +N.NN%  N    Xd Yh   [停止][日志][详情][删除]│
└──────────────────────────────────────────────────────────┘
```
- 盈亏/收益率/成交笔数：来自 **stats-api `accounts_overview`** 按 `account_id` join（策略=账户 1:1）。
- 运行时长：取**该策略最早日志日期**起算（对重启免疫）——复用 `get_strategy_log_files` 解析文件名 `_YYYYMMDD.log` 取最小值，排除 dryrun/backtest；无日志回退 `start_time`。
- 操作：启停=`start_strategy`/`stop_strategy`；删除=`delete_strategy`；日志=弹窗(`get_strategy_log_files`+`get_strategy_logs`，可自动刷新)；详情=跳 `/monitor/strategy/:id?account_id=&name=`。
- **添加策略弹窗**字段：策略名称、选择配置(`list_strategy_configs`)、策略ID、选择账户(`list_accounts`)、交易对、Python文件(`list_strategy_files`)、QQ邮箱、飞书邮箱、配置预览、描述、自动启动开关 → `create_strategy`。

### 4.3 策略详情 StrategyDetail（净值曲线/指标）
```
[←] <策略名>  账户<id>                 [7天][30天][90天][1年][全部]
┌净值┐┌累计收益率┐┌累计盈亏┐┌可用┐┌钱包┐┌未实现┐
┌成交┐┌夏普┐┌最大回撤┐┌年化┐┌波动率┐┌本金┐
┌─净值曲线 (ECharts, 含本金参考线)──────────────────────┐
│        ╱╲    ╱╲╱                                        │
└────────────────────────────────────────────────────────┘
```
数据源：`account_stats`(最新快照+全历史指标) + `equity_curve?range=`(曲线点+区间指标)。组件 `AccountEquityCurve.vue` 直接复用。
> 说明：30天=90天可能相同（Binance 仅留 ~30 天每日快照）；7天窗口的夏普/年化偏高是短窗口统计假象。

### 4.4 策略日志 StrategyLogs（两个 Tab）
```
┌Tab: 日志文件 | 策略代码┐
│[搜索文件]      │  <文件名>   [行数▾][刷新][导出][自动刷新]      │
│ file1.log      │  ┌─────日志内容(按级别着色)──────────────┐    │
│ file2.log  ←选 │  │ [INFO] ...   [ERROR]...(红) [WARN](黄) │    │
│ …              │  └───────────────────────────────────────┘    │
└────────────────┴───────────────────────────────────────────────┘
```
- 日志文件 Tab：`get_strategy_log_files` 列表 + `get_strategy_logs(filename,tailLines)` 内容 + `download_log_file` 导出 + 3s 自动刷新。
- 策略代码 Tab：`list_strategy_files`/`get_strategy_source` 查看；**编辑/保存**(`save_strategy_source`)属"写"，按 portal 角色放行。

### 4.5 账户管理 Account
```
┌账户概览  [选择账户▾]──────────────────────────────────────┐
│ 交易所  账户ID  总资产  未实现盈亏                          │
│ ┌当前持仓 [刷新]──────────────────────────────────────┐   │
│ │品种│方向│数量│开仓均价│标记价│名义价值│未实现│杠杆│   │   │
│ └──────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────┘
┌账户列表 (N)──────────────────────────────────[搜索]┐  [注册账户]
│▸│账户名/交易所│账户ID│API Key│钱包余额│净值│可用│未实现│收益率│状态│操作│
└─────────────────────────────────────────────────────────────────────┘
```
- 钱包余额/净值/可用/收益率：来自 stats-api `accounts_overview`（表头加 tooltip 解释：净值=钱包+未实现，可用=扣保证金后可开仓）。
- 持仓：`get_account_positions(accountId)`，12s 自动刷新。
- 注销=`unregister_account`；**注册账户弹窗**字段：账户ID、交易所(okx/binance)、API Key、Secret Key、Passphrase(仅okx)、模拟盘开关 → `register_account`。
- 展开行/详情页复用主站 `AccountDetail`（含持仓/订单/账户信息 Tab、平仓、撤单、下单——这些"写"按角色放行）。

### 4.6 系统日志 Logs
```
[控制台 | 文件]  ← 两种模式
控制台: LogSender + LogConsole(实时 log 推送)
文件:   同策略日志(get_system_log_files / get_system_logs / download_log_file)
```

---

## 5. 复用清单（同栈，直接搬 + 改动点）

### 5.1 可直接搬（从 trading 前端 `src/`）
- `views/`：Dashboard、Strategy、StrategyDetail、StrategyLogs、Account、AccountDetail、AccountPositions、Orders、Positions、Logs、NotFound、Layout、Login（Login 视情况换 portal 登录）。
- `components/`：全部（Charts/*、Account/*、Strategy/*、Order/*、Log/*、Permission/*）。
- `stores/`：account、strategy、order、stats、app（user store 改造见下）。
- `services/WebSocketClient.js`、`utils/*`、`api/*`、`directives/permission.js`。

> ⚠️ portal 那边需要先**拿到 trading 前端源码的拷贝**（让用户打个 tar 传过去，或给只读访问）。

### 5.2 必改的点
| 文件/处 | 改动 |
|---|---|
| `vite.config.js` | `base: '/monitor/'`；dev proxy 把 `/monitor/ws`、`/monitor/stats-api` 指向 portal 后端。 |
| `router/index.js` | `createWebHistory('/monitor')`；**删** `/users` 路由；详情页路由保留。 |
| `services/WebSocketClient.js` | WS 地址→ `wss://<portal>/monitor/ws`（同源）；**去掉**向 trading 直发 login 的逻辑（mediator 接管），或保留——mediator 会"吞掉"并合成 login_response。 |
| `utils/statsApi.js` | baseURL→ `/monitor/stats-api`。 |
| `stores/user.js` | 改成走 **portal 自己的登录/会话**；`hasPermission/role` 映射到 portal 角色（驱动 `v-permission` 决定"写"按钮显隐）。 |
| Layout 菜单 | **删"用户管理"**项；用户菜单接 portal 登出；改密码项去掉或接 portal。 |
| 删除 | `views/UserManagement.vue` 及 `list_users/add_user/update_user/delete_user/change_password` 相关调用。 |
| 品牌 | LOGO/标题换成 portal 的（`/static/sqt-logo-*.svg`）。 |

### 5.3 权限映射（v-permission）
主站用 `v-permission="'strategy:create'"` 等控制"写"按钮显隐。portal 把自己的角色映射到这些权限串（如：portal 管理员→全部；只读用户→只给 `*:view`）。后端 mediator 的写闸门(2.4)是**第二道**保险。

---

## 6. portal 后端要实现的代理层（FastAPI 原生 async）

trading 与 portal **不同机** → 一律走 trading 的**公网 https/wss 端点**，不碰内网 8002/8003。

### 6.1 stats-api HTTP 代理（FastAPI + httpx）
```python
# routers/monitor.py
import httpx
from fastapi import APIRouter, Request, Depends
from fastapi.responses import Response
from core.permissions import require_module

router = APIRouter(prefix="/monitor")
TRADING_STATS = "https://trading.your-domain.com/stats-api"
_client = httpx.AsyncClient(timeout=12.0)

@router.api_route("/stats-api/{path:path}", methods=["GET"],
                  dependencies=[Depends(require_module("monitor"))])
async def stats_proxy(path: str, request: Request, user=Depends(current_user)):
    owned = owned_account_ids(user)                  # None=管理员(看全部)
    qp = dict(request.query_params)
    if owned is not None and qp.get("account_id") and qp["account_id"] not in owned:
        raise HTTPException(403, "无权访问该账户")     # account_stats / equity_curve 归属校验
    r = await _client.get(f"{TRADING_STATS}/{path}", params=qp)
    data = r.json()
    if path.endswith("accounts_overview") and owned is not None:   # 列表按归属裁剪
        data["accounts"] = [a for a in data.get("accounts", []) if a["account_id"] in owned]
    return JSONResponse(data, status_code=r.status_code)
```
`require_module("monitor")` 已校验 sqt_token + 模块权限；同源无需 CORS。**务必做归属过滤**——否则策略所有人能借 stats API 看到他人财务。

### 6.2 WS 中介（FastAPI websocket + websockets 上游 + 1s 节流）
```python
# routers/monitor_ws.py
import asyncio, json, time, websockets
from fastapi import APIRouter, WebSocket, WebSocketDisconnect
from core.auth import verify_token_from_cookie          # 解 sqt_token -> user
from core.config import settings                          # PORTAL_SVC_USER / _PASS

router = APIRouter()
TRADING_WS = "wss://trading.your-domain.com/ws"
WRITE_ACTIONS = {"start_strategy","stop_strategy","create_strategy","delete_strategy",
                 "register_account","unregister_account","save_strategy_source",
                 "set_log_config","deactivate_kill_switch","place_order","cancel_order"}

def can_write(user, data):                                # 策略所有人 + 管理员
    return user.is_admin or user_owns_strategy(user, data.get("strategy_id"))

def filter_by_owned(user, msg):
    """按归属裁剪读返回(管理员不裁)。返回 None 表示不改。"""
    if user.is_admin: return None
    sids, aids = owned_strategy_ids(user), owned_account_ids(user)
    d = msg.get("data")
    if isinstance(d, list) and d and "strategy_id" in d[0]:               # list_strategies
        msg["data"] = [s for s in d if s.get("strategy_id") in sids]; return msg
    if isinstance(d, dict) and ("okx" in d or "binance" in d):           # list_accounts
        for ex in ("okx","binance"):
            d[ex] = [a for a in d.get(ex,[]) if a.get("account_id") in aids]
        return msg
    return None

@router.websocket("/monitor/ws")
async def monitor_ws(ws: WebSocket):
    user = verify_token_from_cookie(ws.cookies.get("sqt_token"))
    if not user or not user.has_module("monitor"):
        await ws.close(code=4401); return
    await ws.accept()
    async with websockets.connect(TRADING_WS, max_size=None) as up:
        await up.send(json.dumps({"type":"login",                       # 用服务账号登录上游
            "username": settings.PORTAL_SVC_USER, "password": settings.PORTAL_SVC_PASS}))
        last_snap = [0.0]
        async def pump_down():                                          # 上游 -> 浏览器
            async for raw in up:
                msg = json.loads(raw)
                if msg.get("type") == "login_response":                 # 上游登录结果, 不透传
                    continue
                if msg.get("type") == "snapshot":                       # 节流到 1s
                    now = time.monotonic()
                    if now - last_snap[0] < 1.0: continue
                    last_snap[0] = now
                    # (可选) snapshot 里的 positions/strategies 也按 owned 裁剪
                changed = filter_by_owned(user, msg)                    # 读过滤(list_strategies/accounts)
                await ws.send_text(json.dumps(changed) if changed else raw)
        async def pump_up():                                            # 浏览器 -> 上游
            while True:
                msg = json.loads(await ws.receive_text())
                if msg.get("type") == "login":                          # 吞掉前端登录, 合成成功
                    await ws.send_text(json.dumps({"type":"login_response","success":True,
                        "token":"portal-proxy","user":{"username":user.name,"role":user.role}}))
                    continue
                act = msg.get("action","")
                if act in WRITE_ACTIONS and not can_write(user, msg.get("data",{})):
                    await ws.send_text(json.dumps({"type":"response","data":{"success":False,
                        "message":"无权限","requestId": msg.get("data",{}).get("requestId")}}))
                    continue
                if act in WRITE_ACTIONS: audit_log(user, act, msg.get("data"))
                await up.send(json.dumps(msg))
        try:
            await asyncio.gather(pump_down(), pump_up())
        except (WebSocketDisconnect, websockets.ConnectionClosed):
            pass
```
要点：① 单 worker 下纯 async 不阻塞；② 1:1 上游连接，requestId 天然隔离；③ snapshot 1s 节流；④ 吞登录、合成响应；⑤ 写闸门（策略所有人+管理员）+ 审计；⑥ 任一端断开，`async with`/`gather` 自动收尾（前端已有重连）。

### 6.3 服务账号
- 在 trading 后端建 `portal_svc`（全权限角色），密码放 portal `core/config`（可用其 Fernet 加密，与 claudecode sk 同机制）。
- **trading 侧我可直接建好账号并给凭证。**

### 6.4 静态伺服 /monitor 子应用
```python
from fastapi.staticfiles import StaticFiles
app.mount("/monitor", StaticFiles(directory="monitor_dist", html=True), name="monitor")
```
- SPA 构建：`vite base=/monitor/`、`router base=/monitor`；history 模式 fallback 用 `StaticFiles(html=True)` 或 catch-all 返回 index.html。
- 用户身份：进入 /monitor 前已在 portal 登录（带 sqt_token）；SPA 不走 trading login（中介合成）；加一个 `GET /monitor/me`（FastAPI 读 sqt_token 返回 `{username, role, permissions}`）喂给前端 user store，驱动 `v-permission`。

---

## 7. 实施步骤（给 portal Claude 的 checklist）

1. **拿源码**：获取 trading 前端 `src/` 拷贝（向用户索取 tar）。
2. **建子应用**：portal 里挂一个 `/monitor` 子前端，`vite base=/monitor/`，复制 `src/`。
3. **删用户管理**：移除 `views/UserManagement.vue`、`/users` 路由与菜单项、`list_users` 等调用。
4. **改连接**：`WebSocketClient` → `/monitor/ws`；`statsApi` → `/monitor/stats-api`；`router` base `/monitor`。
5. **接 portal 鉴权**：删除 SPA 自带 Login/`stores/user.js` 的 trading 登录逻辑；改为请求 `GET /monitor/me` 拿当前用户/角色/权限，喂 user store，驱动 `v-permission`。
6. **FastAPI 后端**：
   - 配置服务账号 `portal_svc` 凭证（`core/config`，Fernet 加密）。
   - 实现 WS 中介 `routers/monitor_ws.py`（第 6.2）+ stats 代理 `routers/monitor.py`（第 6.1）+ `GET /monitor/me`。
   - 静态挂载 `/monitor`（第 6.4）。"写" action 角色闸门（策略所有人+管理员）+ 审计。
7. **逐页验收**：对照第 4 节，重点验：实时 snapshot(1s) 推送、日志自动刷新、净值曲线区间切换、启停/创建/删除/注册/下单的写链路 + 无权用户被拦。
8. **品牌**：换 LOGO(`/static/sqt-logo-*.svg`)/标题/主题色。

---

## 8. 决策状态

**已确认（已并入方案）：**
- ① 全功能对等，仅不做用户管理；Orders/Positions/手动交易做齐。
- ② portal 栈 = FastAPI + Jinja2 + vanilla JS（+ 闲置 Vue SPA），中介/代理用 FastAPI async。
- ③ trading 与 portal **不同机** → 走公网 `wss://trading.your-domain.com/ws` + `https://.../stats-api/`。
- ④ 同意建服务账号 `portal_svc`。
- ⑤ 写操作授权 = **策略所有人 + 管理员**。
- ⑥ snapshot **节流 1s**。

- ⑦ /monitor = **Vue3 SPA 子应用**（确认）。
- ⑧ 权限模型 = **可见即可操作**：策略所有人只见/操作自己的策略，管理员见/操作全部 → 中介**读过滤 + 写闸门**（§2.4 / §6）。
- ⑨ **写操作落审计表**（谁/何时/动了哪个策略/结果）。
- ⑩ 服务账号 **`portal_svc`（SUPER_ADMIN）已在 trading 侧建好并持久化**（`cpp/user_configs/portal_svc.json`）；**凭证经安全渠道单独交付**（不写进本文档）。

**portal 实现方还需在 portal 侧建两张表：**
1. **归属表** `strategy_owner(portal_user, strategy_id, account_id)`：驱动 §2.4 的 `owned_*` 过滤；管理员=全部；`create_strategy` 成功后写入创建者。
2. **审计表** `monitor_audit(ts, portal_user, action, strategy_id/account_id, payload, result)`：中介每次放行写操作时落一条。

**归属表初始种子数据（已定，全部归 portal 用户 `hyf`；管理员另见全部）：**

| portal_user | strategy_id | account_id | exchange |
|---|---|---|---|
| hyf | `mastercombo` | acct2 | binance |
| hyf | `apollo_fund` | acct3 | binance |
| hyf | `five_mom_factor_binance_testnet` | acct1 | binance |

> 即 `hyf` 登录 /monitor 只看到这 3 个策略/账户并可操作；管理员看到全部。今后 `create_strategy` 成功后把新 strategy_id 自动记到创建者名下。
> （以上 strategy_id↔account_id 由 portal_svc 实拉 `list_strategies` 取得，准确。）

---

至此方案的全部决策点已闭环；portal 实现方可直接据本文动工。
