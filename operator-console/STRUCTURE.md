# Vue3 操作台结构 (operator-console/)

## 总览

`operator-console` 是一套**实盘量化交易管理台**的纯前端工程，技术形态为 **Vue3 (`<script setup>` 组合式 API) + Pinia + Vue Router + Element Plus + ECharts**。

它的最大特点是 **WebSocket 实时优先**：绝大多数业务数据（订单、持仓、策略、日志、行情快照）由 C++ UI 服务器（默认 `ws://localhost:8002`）以 **100ms 快照 + 增量事件** 的方式实时推送，前端用单例 `wsClient` 接管收发，再分发到各 Pinia store。`src/api/*` 里的方法本质上是「把 WebSocket 请求包成 Promise」的 RPC 封装，而非传统 HTTP 接口。两个例外走 HTTP：认证（`authApi.js` → `:8080`）和只读统计净值曲线（`statsApi.js` → `:8003`，独立 Python `stats_api.py`）。

权限模型是双角色 RBAC：`SUPER_ADMIN`（超级管理员，全权限）与 `STRATEGY_MANAGER`（策略管理员，只能查看 + 启停被分配的策略）。Dashboard 按角色渲染不同面板，路由守卫 + `v-permission` 指令 + `<Permission>` 组件三层把关。

### 技术栈

| 类别 | 选型 |
|------|------|
| 框架 | Vue 3.4 (`<script setup>` Composition API) |
| 构建 | Vite 5（`@` 别名指向 `src`，端口 3000，代理 `/ws`→8002、`/stats-api`→8003）|
| 状态 | Pinia 2（setup store 写法）|
| 路由 | Vue Router 4（history 模式 + 全局 `beforeEach` 守卫）|
| UI 库 | Element Plus 2.5 + `@element-plus/icons-vue`（中文 locale，暗黑模式 CSS 变量）|
| 图表 | ECharts 5 + vue-echarts |
| 实时通信 | 原生 WebSocket（单例 `wsClient`，自动重连 + Mock 降级）|
| HTTP | axios（仅统计 API）/ 原生 fetch（仅认证 API）|
| 工具 | dayjs、lodash-es；SCSS；ESLint + Prettier |

### 目录树

```
operator-console/
├── package.json                # 依赖与脚本 (dev / build / lint)
├── vite.config.js              # Vite 配置：@ 别名、端口、/ws 与 /stats-api 代理
├── start-wsl.sh                # WSL 一键启动脚本（检环境/装依赖/查后端/起 dev）
├── .env.example                # 环境变量样例（VITE_WS_URL / AUTH / STATS / DEMO_MODE）
├── README.md                   # 项目说明（Kungfu 架构、快速开始、常见问题）
├── ACCOUNT_INTEGRATION.md      # 多账户注册/注销的前后端 WebSocket 协议说明
├── index.html (入口)           # SPA 挂载点 #app
├── src/
│   ├── main.js                 # 应用入口：装 Pinia/Router/ElementPlus/权限指令/WS 插件
│   ├── App.vue                 # 根组件：<router-view> + 全套明暗主题 CSS 变量
│   ├── api/                    # WebSocket RPC 封装（account/order/strategy）
│   ├── services/              # 底层服务：WebSocketClient 单例 + 认证 fetch
│   ├── stores/               # Pinia 状态：user/app/account/order/strategy/log/stats
│   ├── views/                # 页面级路由组件
│   │   └── Dashboard/        # 按角色拆分的仪表板子面板
│   ├── components/           # 业务组件，按领域分目录
│   │   ├── Account/         # 账户详情/添加/净值曲线弹窗
│   │   ├── Charts/          # 8 个 ECharts 图表组件
│   │   ├── Log/            # 日志控制台 + 日志发送器
│   │   ├── Order/         # 订单详情/下单/最近订单表
│   │   ├── Permission/   # 权限插槽组件 <Permission>
│   │   └── Strategy/    # 创建策略/运行中策略表
│   ├── directives/         # v-permission / v-role 自定义指令
│   ├── router/            # 路由表 + 守卫
│   ├── utils/            # 格式化、storage、statsApi（axios）
│   ├── mock/           # 无后端时的模拟数据
│   └── styles/        # 全局 SCSS
├── database/
│   └── clickhouse_schema.sql   # ClickHouse 建表 DDL（参考用，前端不直连）
├── web_server/
│   └── README.md               # 后端 FastAPI/SSE Web 服务层设计文档（说明性）
└── deploy/
    ├── deploy.sh               # Linux 生产部署脚本
    ├── nginx.conf              # Nginx 反代 + HTTPS 配置
    └── setup-ssl.sh            # Let's Encrypt 证书申请脚本
```

---

## 根目录文件

这一层是工程的「外壳」：构建配置、启动脚本、环境变量、说明文档。

| 文件 | 职责 |
|------|------|
| `package.json` | 工程元数据与依赖清单；scripts 提供 `dev`(vite) / `build` / `preview` / `lint`。名称 `trading-platform-frontend`，纯前端 SPA，`type: module`。 |
| `vite.config.js` | Vite 配置。`@` 别名→`src`；dev server 端口 3000、`host:true` 允许外部访问；代理 `/ws`→`ws://127.0.0.1:8002`（WebSocket）、`/stats-api`→`http://127.0.0.1:8003`（统计 API，避免 CORS）。生产输出到 `dist`。 |
| `start-wsl.sh` | WSL 环境一键启动脚本：检查 Node/npm，自动 `npm install`（可选国内镜像），缺 `.env.development` 时自动生成，探测 C++ 后端 8002 端口是否就绪，最后 `npm run dev`。 |
| `.env.example` | 环境变量样例。说明 Vite 只暴露 `VITE_` 前缀变量、严禁写后端密钥。关键变量：`VITE_WS_URL`(交易 WS)、`VITE_AUTH_API_URL`(认证)、`VITE_STATS_API_URL`(统计)、`VITE_DEMO_MODE`(只读演示模式开关，生产必须 false)。 |
| `README.md` | 项目说明，强调「Kungfu 架构 / WebSocket 直连 C++ / 100ms 快照 / 零 HTTP」，含快速开始与「WS 连接失败属正常」等常见问题。 |
| `ACCOUNT_INTEGRATION.md` | 多账户管理集成文档：`register_account` / `unregister_account` 的 WebSocket 消息格式、前端表单字段（strategyId/exchange/isTestnet）、后端 C++ 对接示例、数据流与测试清单。 |

---

## src/ 入口

整个 SPA 的两个根文件，负责「装配应用」与「定义全局视觉系统」。

| 文件 | 职责 |
|------|------|
| `main.js` | 应用启动入口。创建 Pinia 并初始化 `userStore.init()`(从 localStorage 恢复登录态) 与 `appStore.init()`(恢复主题)；全局注册所有 Element Plus 图标、`v-permission`/`v-role` 指令、`<Permission>` 组件、WebSocket 插件；挂载后延迟 1 秒自动 `wsClient.connect()`，并在路由 `afterEach` 中对已登录用户补连 WS。 |
| `App.vue` | 根组件。模板仅 `<el-config-provider>` 包 `<router-view>`(中文 locale)；`<style>` 内定义**完整的设计系统**——明/暗两套 CSS 变量（颜色/阴影/圆角/字体）、滚动条、Element Plus 组件深度覆写、价格闪烁/呼吸/淡入等动画、毛玻璃卡片。 |

---

## src/api/

WebSocket RPC 封装层。注意：**这里不是 HTTP API**，而是把 `wsClient.send(action, {requestId, ...})` 包成 Promise，靠 `requestId` 在 `response` 事件里匹配回调（带超时）。部分方法是占位实现（数据实际由快照推送）。

| 文件 | 职责 |
|------|------|
| `index.js` | 统一导出 `accountApi` / `strategyApi` / `orderApi`，并注明本系统数据主要走 WebSocket。 |
| `account.js` | 账户 RPC。实现 `requestId` 匹配的 `sendRequest`；`getAccounts`→`list_accounts`、`addAccount`→`register_account`(15s 超时)、`deleteAccount`→`unregister_account`；详情/余额/持仓等为占位 stub。 |
| `strategy.js` | 策略 RPC（功能最全）。覆盖策略列表/启停/删除/创建、策略日志文件与内容、系统日志、最近订单、账户持仓、策略配置/源码读写、日志文件下载等十余个 action。`resolve` 时解包 `data.data ?? data`。 |
| `order.js` | 订单占位 API。全部方法仅打印日志并返回 `{success:true}`——真实订单数据由 WebSocket 快照写入 `orderStore`，下单/撤单实际通过 `orderStore` 直接发 `place_order`/`cancel_order` 命令。 |

---

## src/services/

底层通信服务。`WebSocketClient.js` 是整个前端的实时数据心脏，务必重点理解。

| 文件 | 职责 |
|------|------|
| `WebSocketClient.js` | **核心**。单例 `wsClient`（同时导出为 Vue 插件，注入 `$ws`/`provide('wsClient')`）。负责：连接 C++ UI 服务器（`VITE_WS_URL`，默认 8002）；按 `type` 分发消息——`snapshot`(100ms 全量快照)、`event`(增量事件如 `order_filled`)、`log`(后端日志)、`response`(命令响应/登录响应)；自定义 `on/off/emit` 事件总线供各 store 订阅；指数退避**自动重连**(最多 10 次)；连接超时 5s 后启用 **Mock 模式**（每秒推空快照，让前端脱离后端也能跑）；`send`/`sendLog` 发命令与前端日志；移动平均统计延迟。 |
| `authApi.js` | 认证 HTTP 服务（**唯一走 fetch 的模块**），对接 C++ `SecureFrontendHandler`（`VITE_AUTH_API_URL`，默认 `:8080/api/auth`）。封装 login/logout/getUserInfo/changePassword/addUser/listUsers/register&unregisterAccount/listAccounts，统一用 `type` 字段区分请求、校验 `code===200`。注：实际登录在 `stores/user.js` 里走的是 WebSocket，本文件偏 HTTP 备用通道。 |

---

## src/stores/

Pinia 状态层，**全部用 setup store 写法**。多数 store 在模块加载时就订阅 `wsClient` 的 `connected`/`snapshot`/`event` 事件，做到数据自动流入。

| 文件 | 职责 |
|------|------|
| `user.js` | **权限体系核心**。定义 `UserRole`(SUPER_ADMIN / STRATEGY_MANAGER)、`Permissions`(strategy/account/order/position/user 五大类细粒度权限)、`RolePermissions`(角色→权限映射)。登录走 WebSocket（无后端且 `VITE_DEMO_MODE=true` 时降级为受限只读演示，已删除 admin/admin 后门）；提供 `hasPermission/hasAnyPermission/hasAllPermissions`、`isSuperAdmin/isStrategyManager` 计算属性；超管专属的用户增删改查（`list/add/update/delete_user`）；token/userInfo/allowedStrategies 持久化到 localStorage。 |
| `app.js` | 全局应用状态：侧边栏折叠、主题(light/dark，切换时操作 `html.dark` class 并持久化)、语言、WebSocket 连接状态(`wsConnected`/`wsReconnecting`/`wsLatency`，监听 WS 事件)、系统通知列表(保留最近 100 条)。 |
| `account.js` | 账户列表状态。WS 连接后自动 `fetchAccounts`；把后端 `{okx:[], binance:[]}` 展平为单一列表并用 `account_id/strategy_id` 归一化 id/name；提供 `totalEquity`/`totalPnL`/`activeAccounts` 计算属性及增删改/同步操作。 |
| `order.js` | 订单与持仓状态。订阅 `snapshot` 批量刷新 orders/positions、订阅 `order_filled` 即时改单状态；计算 `activeOrders`/`filledOrders`/`totalPositionValue` 等；`placeOrder`/`cancelOrder` 直接发 WS 命令。 |
| `strategy.js` | 策略状态（逻辑较重）。WS 连接后延迟拉取并每 10s 轮询策略列表；与统计服务 `statsApi` 按 `account_id` join 出 pnl/收益率/成交数；`fetchFirstLogDate` 从日志文件名解析「首次运行日期」作为运行时长起算点（排除 dryrun/backtest）；含策略日志文件/内容读取。 |
| `log.js` | 日志状态。订阅 `log` 事件与 `snapshot.logs` 批量入库；最多保留 1000 条防内存溢出；按级别统计、筛选(级别/来源/关键词/时间)、导出 txt；`sendLogToBackend` 前端日志回传；可从后端拉历史日志文件与日期。 |
| `stats.js` | 统计 store。数据来自独立 `statsApi`(:8003)，与 WebSocket 解耦；维护 `overview`(account_id→净值/盈亏/收益率/成交数) 与 `reachable`(服务不可达时优雅降级)。 |

---

## src/views/

页面级路由组件，与 `router/index.js` 一一对应。

| 文件 | 职责 |
|------|------|
| `Login.vue` | 登录页（公开路由）。表单提交后调 `userStore.login`，走 WebSocket 认证。 |
| `Layout.vue` | 主框架布局。左侧 `el-menu` 侧边栏（菜单项用 `v-permission` 控制可见性）+ 顶栏面包屑 + WebSocket 连接状态指示 + 主题切换/用户下拉，主体区嵌 `<router-view>`。所有需登录页面的父容器。 |
| `Dashboard.vue` | 仪表板入口。**按角色分发**：超管渲染 `SuperAdminDashboard`、策略管理员渲染 `StrategyManagerDashboard`，否则回退到内置通用面板（总资产/未实现盈亏/运行策略/活跃订单卡片 + 策略与订单列表）。 |
| `Strategy.vue` | 策略管理页。列表 + 创建按钮(`v-permission="strategy:create"`)，使用 `RunningStrategiesTable` 与 `CreateStrategyDialog`。 |
| `StrategyDetail.vue` | 策略详情页（隐藏路由 `/strategy/:id`）。展示单策略净值曲线（按 7d/30d 等时间段），策略与账户 1:1。 |
| `StrategyLogs.vue` | 策略日志页。左侧日志文件列表 + 右侧日志内容/策略源码查看。 |
| `Account.vue` | 账户管理页。多策略多交易所(OKX/Binance)账户列表，添加按钮(`v-permission="account:create"`)，配 `AddAccountDialog`/`AccountDetail`。 |
| `AccountDetail.vue` | 账户详情页（隐藏路由 `/account/:id`）。展示账户交易所/实盘模拟盘标签及明细。 |
| `AccountPositions.vue` | 账户持仓管理页。展示某账户全仓/逐仓持仓（未在主路由表中直接挂载，作详情子页）。 |
| `Orders.vue` | 订单管理页。订单列表 + 手动下单按钮(`v-permission="order:create"`)，用 `PlaceOrderDialog`/`OrderDetailDialog`。 |
| `Positions.vue` | 持仓管理页。持仓统计卡片 + 持仓列表 + 分布图表。 |
| `Logs.vue` | 系统日志页。控制台视图/表格视图双模式切换，承载 `LogConsole`，展示来自 C++ 框架的全部日志。 |
| `UserManagement.vue` | 用户管理页（`adminOnly`，仅超管）。用户统计 + 增删改用户与策略分配。 |
| `NotFound.vue` | 404 页（glitch 风格文字 + 返回首页）。 |

### src/views/Dashboard/（按角色拆分的子面板）

Dashboard 的实现细节，由 `Dashboard.vue` 根据当前用户角色动态挂载其一。

| 文件 | 职责 |
|------|------|
| `SuperAdminDashboard.vue` | 超管全局概览。顶部 OKX/Binance **实时行情**（价格涨跌闪烁），下接全局统计卡片（总资产/未实现盈亏等）与图表区。 |
| `StrategyManagerDashboard.vue` | 策略管理员面板。顶部「策略管理员模式」提示；账号选择器**只列出与被分配策略关联的账户**(`relatedAccounts`)；展示选中账号的净值/未实现盈亏等统计。 |
| `ViewerDashboard.vue` | 观摩者面板（只读）。账号选择器 + 选中账号统计卡片，强调「只能查看，无法交易」。注：当前路由角色仅含超管/策略管理员，此面板为预留/兼容的纯查看视图。 |

---

## src/components/Account/

账户相关业务组件（详情展示 + 添加 + 净值曲线弹窗）。

| 文件 | 职责 |
|------|------|
| `AccountDetail.vue` | 账户详情展示组件。用 `el-descriptions` 展示策略ID/账户ID/交易所标签/脱敏 API Key/实盘或模拟盘环境等。 |
| `AddAccountDialog.vue` | 添加交易账户对话框。表单含账户ID/交易所(OKX/Binance)/API Key/Secret/Passphrase/模拟盘开关；**动态表单**：选 OKX 显示 Passphrase，选 Binance 自动隐藏；提交触发 `register_account`。 |
| `EquityCurveDialog.vue` | 净值曲线弹窗。顶部时间段选择(1d/7d/30d/90d/all)，内嵌 `EquityChart` 渲染指定账户净值。 |

## src/components/Charts/

8 个 ECharts 图表组件。分两类：**纯展示型**（数据由父组件 props 传入，`AccountEquityCurve`/`SlippageChart`）与**自取数型**（内部连 store/接口）；部分仍含 mock 兜底数据。

| 文件 | 职责 |
|------|------|
| `AccountEquityCurve.vue` | 纯展示净值曲线。父传 `points:[{ts,equity,upnl}]` + 本金线，画平滑净值折线。 |
| `EquityChart.vue` | 账户净值图。按 `accountId`+`timeRange` 自行从 `accountStore` 取数渲染（`EquityCurveDialog` 使用）。 |
| `MultiAccountEquityChart.vue` | 多账户净值对比图。复选框最多选 5 个账户 + 时间段切换，叠加多条净值曲线。 |
| `MultiStrategyPerformanceChart.vue` | 多策略业绩对比图。复选框最多选 5 个策略 + 时间段切换，对比多策略表现。 |
| `PnlDistributionChart.vue` | 盈亏分布图。基于传入 `positions` 绘制各持仓盈亏分布（含 mock 兜底）。 |
| `PositionDistributionChart.vue` | 持仓分布图。基于 `positions` 绘制持仓占比/分布（含 mock 兜底）。 |
| `SlippageChart.vue` | 滑点分析图。柱状=每次调仓冲击成本(USDT)，折线(右轴)=加权滑点 bp；纯展示，数据父传。 |
| `StrategyPerformanceChart.vue` | 策略业绩排行柱状图（当前为内置 mock 数据演示）。 |

## src/components/Log/

日志系统组件（双向通信：接收 C++ 日志 + 回传前端日志）。

| 文件 | 职责 |
|------|------|
| `LogConsole.vue` | 日志控制台。终端风格实时滚动显示，顶部工具栏含连接状态/日志数/延迟、自动滚动开关、全屏、过滤；支持彩色级别标识。`Logs.vue` 的控制台视图载体。 |
| `LogSender.vue` | 日志发送器卡片。可手动选级别+填消息+附加 JSON 数据，调 `logStore.sendLogToBackend` 把日志回传 C++ 后端，含快速发送预设。 |
| `README.md` | 日志系统使用说明文档：双视图、前后端 WebSocket 日志协议（`log`/`frontend_log`/`set_log_config`）、级别/来源约定、最佳实践、故障排查。 |

## src/components/Order/

订单相关业务组件。

| 文件 | 职责 |
|------|------|
| `OrderDetailDialog.vue` | 订单详情对话框。`el-descriptions` 展示订单ID/交易所单号/交易对/方向标签/价格数量/成交明细等。 |
| `PlaceOrderDialog.vue` | 手动下单对话框。表单选账户 + 交易对/方向/类型/价格/数量，校验后下单。 |
| `RecentOrdersTable.vue` | 最近订单表格。列含交易对/方向(买卖标签)/价格/数量/状态等，用 `format.js` 做中文化展示。 |

## src/components/Permission/

权限控制（组件式，与指令互补）。

| 文件 | 职责 |
|------|------|
| `index.vue` | `<Permission>` 插槽组件（全局注册）。props 接收 `permission`(String/Array) 与 `requireAll`，命中权限才渲染默认插槽内容；适合包裹一段需要条件显示的 UI（比指令更灵活，不直接删 DOM）。 |

## src/components/Strategy/

策略相关业务组件。

| 文件 | 职责 |
|------|------|
| `CreateStrategyDialog.vue` | 创建策略对话框。表单含策略名称 + 选择配置模板(`list_strategy_configs`)等，提交触发 `create_strategy`。 |
| `RunningStrategiesTable.vue` | 运行中策略表格。列含策略名/盈亏/收益率(涨绿跌红)/状态/操作（启停），供 Dashboard 与策略页复用。 |

---

## src/directives/

Vue 自定义指令，实现「无权限即从 DOM 移除元素」的硬控制（与 `<Permission>` 组件软隐藏互补）。

| 文件 | 职责 |
|------|------|
| `permission.js` | 导出 `permission` 与 `role` 两个指令。`v-permission="'x:y'"` 单权限、数组为任一满足、`.all` 修饰符要求全部满足；`v-role` 按角色控制。无权限时 `el.parentNode.removeChild(el)` 直接摘除元素。`main.js` 注册为 `v-permission`/`v-role`。 |

## src/router/

| 文件 | 职责 |
|------|------|
| `index.js` | 路由表 + 全局守卫。history 模式；`/login` 公开，其余挂在 `Layout` 下需 `requiresAuth`，路由 meta 携带 `title`/`icon`/`permission`/`adminOnly`/`hidden`。`beforeEach` 守卫：设页面标题→放行公开页→未登录跳登录(带 redirect)→`adminOnly` 校验超管→`permission` 校验细粒度权限，不满足跳 `/dashboard`。 |

## src/utils/

通用工具函数。

| 文件 | 职责 |
|------|------|
| `format.js` | 格式化工具集（dayjs）。时间/数字/百分比/金额/大数(K/M/B)/时长格式化；`parseTimestamp` 兼容秒/毫秒/ISO；订单状态、策略状态、买卖方向、订单类型的**中文映射**；收益率与盈亏计算。 |
| `statsApi.js` | 统计 API 客户端（**axios**）。连独立 Python `stats_api.py`(只读 Redis 净值/成交时序，默认同源 `/stats-api`→:8003)，与交易主进程解耦；提供 accountsOverview/accountStats/equityCurve/slippageHistory/health。 |
| `storage.js` | localStorage 封装。带 `trading_platform_` 前缀的 `Storage` 类(load/save/remove，含异常兜底)，预导出 strategy/account/order/position/user 五个实例。 |

## src/mock/

| 文件 | 职责 |
|------|------|
| `index.js` | 模拟数据服务（后端未就绪时本地开发用）。内置策略/账户/订单/持仓初始数据，从 `storage` 加载或回退默认值；`saveMockData` 持久化、`mockApiResponse` 模拟带延迟的 `{code,message,data}` 响应。 |

## src/styles/

| 文件 | 职责 |
|------|------|
| `main.scss` | 全局样式入口（`main.js` 引入）。承载全局 SCSS 基础样式，与 `App.vue` 内的 CSS 变量设计系统配合。 |

---

## 非前端目录（参考/部署用）

这些目录不参与前端构建，是后端契约说明与运维资产，帮助理解前后端如何对接。

### database/

| 文件 | 职责 |
|------|------|
| `clickhouse_schema.sql` | ClickHouse 建表 DDL（**前端不直连**，仅作数据契约参考）。含 users/strategies/accounts/account_snapshots/orders/trades/positions/strategy_performance/system_logs 九张表（多为时序 MergeTree、按月分区、日志 90 天 TTL），以及账户汇总/策略每日/订单统计三个物化视图与常用查询示例。 |

### web_server/

| 文件 | 职责 |
|------|------|
| `README.md` | 后端 Python Web 服务层设计文档（说明性，非可运行代码）。描绘 FastAPI + SSE 低延迟事件流方案、目录规划(api/services/database/models/utils)、SSE 管理器与端点示例、WebSocket/SSE/轮询三种方案的延迟对比。 |

### deploy/

| 文件 | 职责 |
|------|------|
| `deploy.sh` | Linux 生产部署脚本。`set -e` 严格模式，定义项目名/部署目录/Nginx 配置路径/备份目录，自动化构建产物落地与 Nginx 接入。 |
| `nginx.conf` | Nginx 反向代理配置。`upstream` 指向后端 `127.0.0.1:8000`(keepalive)；80 端口强制跳转 443；443 启用 SSL/HTTP2，挂载证书并代理前端与后端。 |
| `setup-ssl.sh` | SSL 证书申请脚本（Let's Encrypt），自动化签发与续期配置。 |