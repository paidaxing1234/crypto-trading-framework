# C++ 引擎结构 (cpp/)

> 本文档面向第一次接触本仓库的开发者，目标是「一眼看懂每个文件夹干啥、每个关键文件干啥」。所有职责描述均基于真实源码总结。

## 总览

这是一套**事件驱动 (event-driven)** 的**实盘量化交易引擎**，C++ 负责低延迟核心（行情接入、订单执行、风控、数据落地），Python 负责策略逻辑（通过 pybind11 绑定调用 C++ 能力）。整体分五层：

1. **core（核心）** — 事件引擎 `EventEngine`、事件/数据基类、统一配置中心 `ConfigCenter`、日志系统 `Logger`。事件引擎是全系统的中枢，所有组件 (`Component`) 注册监听器、按时间戳顺序派发事件。
2. **adapters（交易所适配）** — 对接 OKX 与 Binance 的 REST API + WebSocket，统一封装下单/撤单/查询、行情订阅、自动重连。是引擎与外部交易所的唯一边界。
3. **network（网络层）** — ZeroMQ 进程间通信 (IPC)、公共 WebSocket 客户端/服务端、前端请求处理、用户认证、VPN/代理监控。C++ server 与 Python 策略、Web 前端之间靠这层通信。
4. **server（服务层）** — 交易服务器主进程 `trading_server_main`，把 core/adapters/network 组装起来：订阅全市场 K线、处理订单与查询、Redis 录制与回放、账户监控、策略子进程管理、合约下架/网络告警等。
5. **strategies + trading（策略与交易域）** — `trading/` 是交易域模型（订单、风控、账户注册表、配置加载器）；`strategies/` 是 pybind11 策略基类 (`StrategyBase`) 及其三个模块 (行情/交易/账户)，Python 策略继承它运行。
6. **scripts（运维脚本）** — 启停、K线补缺/预加载、净值记录、统计 API、交割单导出等独立运维工具，多数为独立进程/cron，零实盘风险。

**数据流（简化）**：交易所 WebSocket → adapters 解析 → ZMQ PUB 行情通道 → Python 策略 (SUB) → 策略下单 → ZMQ PUSH 订单通道 → server `order_processor` → 风控检查 → adapters REST 下单 → 回报经 ZMQ PUB 回报通道送回策略。行情同时被 Redis 录制器落地，供历史查询。

```text
cpp/
├── CMakeLists.txt              # 构建脚本（静态库 trading_core + 4 个可执行 + strategy_base 模块）
├── risk_config.json            # 全局风控限额配置（实际生效文件）
├── core/                       # 事件引擎、事件/数据基类、配置中心、日志
├── adapters/                   # 交易所适配
│   ├── binance/                # Binance REST + WebSocket
│   └── okx/                    # OKX REST + WebSocket
├── network/                    # ZMQ、WebSocket 客户端/服务端、认证、代理监控
├── server/                     # 交易服务器主进程及其子模块
│   ├── callbacks/              # WebSocket 行情回调装配
│   ├── config/                 # 服务器全局配置与状态
│   ├── handlers/               # 订单/查询/订阅/前端命令 处理器
│   ├── klinedata/              # K线录制、补缺、历史拉取、聚合
│   └── managers/               # 账户/Redis/策略进程/下架监控 等管理器
├── strategies/                 # 策略层（pybind11 基类 + Python 策略 + 工具）
│   ├── core/                   # C++ 策略基类与三大模块、pybind11 绑定
│   ├── implementations/        # 具体 Python 策略（示例、网格）
│   ├── utils/                  # Python 策略工具（K线管理、日志、最小下单单位）
│   ├── configs/                # 策略参数/本金配置（示例）
│   ├── strategy_configs/       # 账户↔策略绑定运行配置（示例）
│   └── acount_configs/         # 账户凭证配置（示例，注意目录名拼写为 acount）
├── trading/                    # 交易域：订单、风控、账户注册表、配置加载
│   └── alerts/                 # 邮件 / 飞书 告警脚本
├── totalconfig/                # 全局总配置（网络监控、邮件）示例
├── user_configs/               # 前端登录用户配置（示例）
└── scripts/                    # 运维脚本（启停、补数、统计、导出）
```

---

## 根目录文件

这一层放构建脚本和实际生效的风控配置。

| 文件 | 职责 |
|------|------|
| `CMakeLists.txt` | CMake 构建。C++17，查找 OpenSSL/CURL/pybind11/hiredis/zmq/websocketpp/nlohmann_json；把 core+network+adapters+server 编成静态库 `trading_core`；产出 4 个可执行：`trading_server_full`(主服务器)、`data_recorder`、`kline_gap_filler`、`kline_fast_filler`；通过 `pybind11_add_module` 产出 Python 模块 `strategy_base`。 |
| `risk_config.json` | **实际生效**的风控限额：单笔/持仓/总敞口上限、最大回撤、单日亏损、下单频率、回撤模式 (`daily_peak`/`daily_initial`)，以及告警渠道开关。`RiskManager` 启动时读取本文件。 |

---

## core/ — 核心引擎

事件驱动架构的中枢：事件引擎 + 事件/数据基类 + 配置中心 + 日志。整层 header-only 居多（`logger` 例外有 .cpp）。

| 文件 | 职责 |
|------|------|
| `event.h` | 事件基类 `Event`：携带 timestamp(毫秒)、source(来源引擎)、producer_id(产生者)；提供 `copy()`/`derive()`/`current_timestamp()`。所有事件的根类型。 |
| `event_engine.h` | **核心**。`EventEngine` 事件引擎：`put()` 推送事件、`drain()` 按「Senior 全局监听器 → 类型监听器 → Junior 全局监听器」顺序派发；维护全局时间戳；`register_listener`/`register_global_listener` 注册回调；`ignore_self` 防死循环；`inject`/`call` 动态注入函数接口。文件内还定义了 `Component` 抽象基类（生命周期 `start(engine)`/`stop()`）。 |
| `data.h` | 行情数据事件类，均继承 `Data`(继承 `Event`)：`TickerData`(行情快照，含买卖价/中间价/价差)、`TradeData`(逐笔成交)、`OrderBookData`(订单簿，含最优买卖/中间价)、`KlineData`(K线 OHLCV，含 `is_confirmed` 完结标志)。 |
| `config_center.h` | **统一配置中心** `ConfigCenter`(单例)。多来源加载（JSON 文件 > 环境变量 > 默认值）、热重载 (`reload`)、变更通知（观察者模式）、`shared_mutex` 线程安全。内含 `ServerConfig`/`OKXConfig`/`BinanceConfig`/`RedisConfig`/`RiskConfig` 五个结构体，各自 `to_json`/`from_json`。便捷入口 `Config()`。 |
| `logger.h` | 日志系统 `Logger`(单例) 声明：DEBUG/INFO/WARN/ERROR 多级别、按来源(source)分文件并按天切分、按大小轮转、异步写线程、`set_ws_callback` 把日志推送到前端 WebSocket；提供 `audit`/`order_lifecycle` 审计与订单生命周期日志，以及 `LOG_INFO` 等便捷宏。 |
| `logger.cpp` | `Logger` 实现：创建日志目录、启动异步写线程、按 source 维护独立文件句柄、时间戳/级别格式化、轮转逻辑。 |

---

## adapters/ — 交易所适配

引擎与交易所之间的唯一边界，按交易所分两个子目录。回调统一用**原始 JSON**（OKX/Binance 各保留原始结构，不强行归一化）。

### adapters/binance/

对接 Binance（现货 SPOT / U本位 FUTURES / 币本位 COIN_FUTURES）的 REST + WebSocket。

| 文件 | 职责 |
|------|------|
| `binance_rest_api.h` | `BinanceRestAPI` 接口：市场数据（ping/时间/exchangeInfo/depth/klines/ticker/funding rate/溢价指数K线）、用户数据流 listenKey、交易（下单/撤单/查单/挂单/批量下单）、账户（余额/持仓/改杠杆/改保证金模式/改持仓模式）。含 `MarketType`/`OrderSide`/`OrderType`/`PositionSide`/`TimeInForce` 枚举。 |
| `binance_rest_api.cpp` | REST 实现。HMAC-SHA256 签名、服务器时间同步(`time_offset_ms_`)、CURL 请求。**关键优化**：内置 `CurlHandlePool` 连接复用池，复用 TLS 连接把单请求 p50 从 ~45ms(每次握手) 降到 ~5ms，批量下单的并发线程各取独立句柄。 |
| `binance_websocket.h` | `BinanceWebSocket` 接口：三种连接类型 TRADING(低延迟下单)/MARKET(行情)/USER(用户数据流)。支持 `connect_with_streams` 组合流 URL（订阅大量 streams 更可靠）、批量订阅、自动重连、listenKey 自动刷新、各类行情/账户/订单回调。 |
| `binance_websocket.cpp` | WebSocket 实现。基于公共 `core::WebSocketClient`；HMAC 签名、消息解析(trade/kline/ticker/depth/markPrice/账户更新/订单成交更新)、重连后 `resubscribe_all` 重订阅。 |
| `BINANCE_README.md` | Binance 集成说明文档：列出已实现的 REST/WS 功能清单与用法。 |

### adapters/okx/

对接 OKX（public/business/private 三种 WebSocket 端点）的 REST + WebSocket。

| 文件 | 职责 |
|------|------|
| `okx_rest_api.h` | `OKXRestAPI` 接口及数据结构（如 `AttachAlgoOrder` 止盈止损参数）。**注意**：文件顶部声明全局 `set_curl_abort_flag()`/`get_curl_abort_flag()`，供信号处理函数在退出时中断正在进行的 CURL 请求。 |
| `okx_rest_api.cpp` | OKX REST 实现。完整下单参数（含止盈止损）、HMAC 签名；通过全局 `g_curl_abort_flag` + CURL 进度回调实现 Ctrl+C 优雅中断。 |
| `okx_websocket.h` | `OKXWebSocket` 接口。三端点 public(行情/深度/成交)/business(K线/trades-all)/private(订单/持仓/账户)；私有频道登录认证、心跳、自动重连；`KlineInterval` 枚举（1s~1M）。 |
| `okx_websocket.cpp` | OKX WebSocket 实现。基于公共 `core::WebSocketClient`；带独立 debug/重连日志文件（`/tmp/okx_websocket_debug.log`）；登录签名、订阅、消息解析与回调。 |
| `README.md` | OKX 适配器说明文档。 |

---

## network/ — 网络层

进程间/跨端通信基础设施：ZeroMQ、WebSocket 客户端与服务端、前端处理、认证、代理监控。

| 文件 | 职责 |
|------|------|
| `zmq_server.h` | **关键**。`ZmqServer` ZeroMQ 服务端，定义全套 IPC 通道地址 `IpcAddresses`（`ipc:///tmp/seq_*.ipc`，比 tcp 低延迟 3-5 倍）：行情 PUB(统一/OKX/Binance 三路)、订单 PULL、回报 PUB、查询 REP、订阅 PULL。含 `MessageType` 枚举与 `publish_*`/`recv_order`/`poll_queries` 等接口。是策略↔服务端的主通信骨架。 |
| `zmq_server.cpp` | `ZmqServer` 实现：建链各 ZMQ socket、发布行情/回报、轮询订单/查询/订阅。 |
| `ws_client.h` | 公共 WebSocket 客户端 `core::WebSocketClient` 与 `WebSocketConfig`(SSL 验证/代理/超时/ping 间隔)。OKX/Binance 适配器底层都复用它。 |
| `ws_client.cpp` | 公共 WebSocket 客户端实现（封装 websocketpp 底层细节）。 |
| `websocket_server.h` | `core::WebSocketServer`(基于 websocketpp)：面向 Web 前端的推送服务端，`set_message_callback` 处理前端命令、`send_event`/`send_log` 推送行情与日志、快照生成器。主服务器在 8002 端口启动它。 |
| `websocket_server.cpp` | 前端 WebSocket 服务端实现。 |
| `proxy_config.h` | 公共代理配置 `core::ProxyConfig`（host/port、`get_proxy_url`、从环境变量加载）。REST 与 WS 统一使用。 |
| `auth_manager.h` | 用户认证管理器 `auth`：登录/登出、JWT Token 生成与验证、SHA256+盐 密码哈希、角色权限（`SUPER_ADMIN`/`STRATEGY_MANAGER`）、用户 JSON 持久化（对应 `user_configs/`）。 |
| `frontend_handler.h` | `FrontendHandler`：独立线程通过 ZMQ REP(端口 5556) 处理前端账户管理请求，不阻塞主交易线程。 |
| `secure_frontend_handler.h` | `SecureFrontendHandler`：在 `FrontendHandler` 基础上增加登录/Token 验证/权限检查的带认证版本。 |
| `vpn_network_monitor.h` | `VpnNetworkMonitor`：定期通过代理探测交易所地址，检测 VPN/代理是否可用，连续失败超阈值经 `RiskManager` 发告警，恢复后发恢复通知。配置来自 `totalconfig/network_monitor_config.json`。 |

---

## server/ — 服务层

交易服务器主进程及其拆分出的子模块。`trading_server_main.cpp` 是整个系统的组装中心。

| 文件 | 职责 |
|------|------|
| `trading_server_main.cpp` | **主入口**（可执行 `trading_server_full`）。启动流程：初始化 Logger/认证/配置/Redis 录制器 → CPU 绑核 + 实时优先级(SCHED_FIFO) → 注册信号处理（含 `crash_signal_handler`：崩溃时杀光策略子进程并 fork 发邮件+飞书告警）→ 加载账户/策略配置 → 启动前端处理器(5556) 与 ZMQ → 连接 OKX(business+public) 与 Binance WebSocket、批量订阅全市场 1m K线（带健康检查重连）+ 主要币种 Ticker → 启动前端 WS(8002)、账户监控、网络监控、VPN 监控、合约下架监控 → 拉起订单/查询/订阅三个工作线程 → 主循环打印统计 + 检查策略心跳 → Ctrl+C 优雅清理。 |

### server/callbacks/

把交易所 WebSocket 的行情回调装配到 ZMQ 发布。

| 文件 | 职责 |
|------|------|
| `websocket_callbacks.h` / `.cpp` | `setup_websocket_callbacks`(OKX)、`setup_binance_websocket_callbacks`、`setup_binance_kline_callback`：把适配器收到的 K线/Ticker 等原始数据转成统一消息 `publish` 到 ZMQ 行情通道，并可回调通知闭合 K线（用于健康检查计数）。 |

### server/config/

| 文件 | 职责 |
|------|------|
| `server_config.h` | 交易服务器全局配置与运行态（端口、订阅集合、全局计数器如 `g_okx_kline_count`/`g_order_count`、`g_running` 等）。注释标注：新代码推荐用 `core/config_center.h` 的 `ConfigCenter`，本文件为向后兼容保留。 |
| `server_config.cpp` | 上述全局变量定义与 `load_config`/`print_risk_config` 等实现。 |

### server/handlers/

ZMQ 各通道请求的实际处理逻辑，从主入口拆出来。

| 文件 | 职责 |
|------|------|
| `order_processor.h` / `.cpp` | **订单处理**。`process_place_order`/`process_batch_orders`/`process_cancel_order`：从 ZMQ 订单通道取请求 → 经全局 `g_risk_manager` 风控检查 → 调用对应交易所 REST 下单/撤单 → 把回报 publish 回策略。此处定义全局 `g_risk_manager`，并 extern 全局账户监控器/策略进程管理器。 |
| `query_handler.h` / `.cpp` | 查询处理 `handle_query`：响应策略端 REQ-REP 查询（账户余额、持仓、未成交订单等）。 |
| `subscription_manager.h` / `.cpp` | 订阅处理 `handle_subscription`：响应策略端动态订阅/退订行情的请求。 |
| `frontend_command_handler.h` / `.cpp` | 前端 WebSocket 命令处理 `handle_frontend_command`（含认证）：响应 Web 前端发来的控制命令（账户/策略管理等）。 |

### server/klinedata/

K线数据的录制、补缺、历史拉取、周期聚合。其中三个 `.cpp` 各自是独立可执行程序。

| 文件 | 职责 |
|------|------|
| `kline_utils.h` / `.cpp` | K线工具：`Kline` 结构、`get_interval_milliseconds`(周期→毫秒)、`align_timestamp`(时间戳对齐到周期边界)、OKX/Binance K线解析、`SymbolInfo`/`is_usdt_contract` 等。被补缺/拉取程序共用。 |
| `gap_detector.h` / `.cpp` | `GapDetector`：连接 Redis，检测某交易对某周期 K线序列中的缺失段(`Gap`)，供补缺程序定位需要回补的时间区间。 |
| `historical_data_fetcher.h` / `.cpp` | 历史数据拉取器：基类 + `OKXHistoricalFetcher`/`BinanceHistoricalFetcher`，按时间范围从交易所 REST 拉历史 K线。供策略历史查询与补缺共用。 |
| `data_recorder.cpp` | **独立可执行** `data_recorder`：被动 ZMQ SUB 接收主服务器发布的 1m K线，存入 Redis（2 个月过期），并聚合成 5m/15m/30m/1h（不同周期不同过期时间）。本身不发订阅请求。 |
| `kline_gap_filler.cpp` | **独立可执行** `kline_gap_filler`：用 `GapDetector` 找缺口 + `HistoricalDataFetcher` 回补，并补齐聚合周期。配合 `scripts/run_gap_filler_loop.sh` 循环运行。 |
| `kline_fast_filler.cpp` | **独立可执行** `kline_fast_filler`：高性能补缺，阶段一用 Pipelined ZCOUNT 粗扫描快速定位缺口，再精补，比 gap_filler 更快。 |

### server/managers/

各类后台管理器：账户、Redis 读写、策略进程、下架监控。

| 文件 | 职责 |
|------|------|
| `account_manager.h` / `.cpp` | 策略↔账户映射：`register_strategy_account`/`unregister_strategy_account`、`get_okx_api_for_strategy`/`get_binance_api_for_strategy` 取出某策略对应的交易所 API 客户端。 |
| `account_monitor.h` | `AccountMonitor`：独立线程定期(默认 10s) 用 REST 查各账户余额/持仓，计算实时盈亏，更新 `RiskManager` 状态（含按策略的回撤检查），触发风控告警。含 `AccountCredentials` 结构。 |
| `redis_recorder.h` / `.cpp` | **Redis 录制器** `RedisRecorder`：把行情实时写入 Redis（trades→List、kline→Sorted Set、orderbook→Hash、funding_rate→Sorted Set）。集成在主服务器内随之启动，受 `RedisConfig.enabled` 开关控制。 |
| `redis_data_provider.h` / `.cpp` | **Redis 读取/回放** `RedisDataProvider`：为策略端提供历史 K线查询（按时间范围/最近 N 天/最近 N 根），支持 1m 自动聚合成 5m/15m/1h/4h/1d，覆盖 OKX/Binance。定义被 pybind11 复用的 `KlineBar` 结构。与 recorder 是「写/读」一对。 |
| `strategy_process_manager.h` | `StrategyProcessManager`：独立于账户注册表，追踪策略子进程运行态。注册/启动/停止策略进程、心跳检测(`check_heartbeats`)、前端中止策略(kill)、崩溃时 `kill_all_strategies_unsafe`(信号安全)。账户注册与策略运行是独立生命周期。 |
| `symbol_delist_monitor.h` | `SymbolDelistMonitor`：每 30s 轮询 Binance `exchangeInfo`，检测 PERPETUAL 合约 status 变化或 deliveryDate 变更（即将/已下架），经邮件+飞书通知管理员。内含简易 `http_get`(curl 命令)。 |

---

## strategies/ — 策略层

C++ 策略基类（pybind11 暴露给 Python）+ 具体 Python 策略 + 工具 + 各类配置示例。Python 策略继承 `StrategyBase`，通过 ZMQ 与 server 通信。

### strategies/core/

C++ 侧策略基类与三大模块、pybind11 绑定。

| 文件 | 职责 |
|------|------|
| `py_strategy_base.h` | 策略基类 `PyStrategyBase`：组合行情/交易/账户三模块 + 历史数据查询(经 `RedisDataProvider`) + 定时任务调度，提供 `run()` 主循环、`connect`/`poll_messages`、各类 `on_*` 虚回调（供 Python 重写）。是 Python 策略的 C++ 本体。 |
| `market_data_module.h` | 行情数据模块 `MarketDataModule`：K线/Trades/OrderBook 订阅与存储（环形缓冲，默认存 2 小时）。定义 `KlineBar` 等结构。 |
| `trading_module.h` | 交易模块 `TradingModule`：合约市价/限价下单、撤单、订单回报处理。定义 `OrderStatus` 等。 |
| `account_module.h` | 账户模块 `AccountModule`：账户注册/注销、余额/持仓查询、账户更新回报处理。定义余额/持仓结构。 |
| `py_strategy_bindings.cpp` | **关键**。pybind11 绑定：把 `StrategyBase` 及 `KlineBar`/`TradeData`/`OrderBookSnapshot`/`FundingRateData`/`BalanceInfo`/`PositionInfo`/`OrderInfo`/`ScheduledTask`/`HistoricalKline` 暴露给 Python（编译出 `strategy_base` 模块）。含 nlohmann::json↔dict 转换、`PyStrategyTrampoline`(允许 Python 继承并正确处理 GIL)、上百个下单/查询/历史数据/定时任务 API。Python 策略的全部能力都从这里来。 |

### strategies/implementations/

具体 Python 策略实现。

| 文件 | 职责 |
|------|------|
| `example_strategy.py` | 策略基类 API 完整参考/示例策略：列出所有数据结构与可调用方法，是写新策略的模板与文档。 |
| `grid/grid_strategy_cpp.py` | 网格策略：基于模块化 C++ `StrategyBase` 实现，支持从配置文件(`--config`)或命令行参数运行，演示订阅 K线 + 网格下单。 |

### strategies/utils/

Python 策略侧工具。

| 文件 | 职责 |
|------|------|
| `kline_manager.py` | `KlineManager`：基于 NumPy 的高性能内存 K线存储，自动滑动窗口、按 symbol 索引，提供 `get_closes` 等快速读取。 |
| `redis_data_fetcher.py` | Redis 数据拉取器：从服务器 Redis 拉行情并存本地 CSV，供离线分析。 |
| `strategy_logger.py` | `StrategyLogger`：把策略日志同时输出到终端和文件（按交易所/策略 ID 分目录）。 |
| `binance_min_qty_sync.py` | Binance minQty 热同步：从 exchangeInfo 抽取 LOT_SIZE.minQty 写入 `binancemin.txt`，并调用策略基类 `load_min_order_config()` 重载 C++ 内存映射，使下次调仓即用最新最小下单单位。 |
| `view_strategy_logs.sh` | 查看策略日志文件的便捷脚本。 |

### strategies/configs/、strategy_configs/、acount_configs/

各类配置示例（`.example.json`，真实文件需自行填密钥）。注意 `acount_configs` 目录名拼写如此（少一个 c）。

| 文件 | 职责 |
|------|------|
| `configs/strategy.example.json` | 单策略完整配置示例：策略 ID/账户/交易所/密钥、联系人、风控参数、策略 params（如网格数/价格区间）。 |
| `configs/initial_capital.example.json` | `{account_id: 起始本金}`，给统计栈算收益率用。 |
| `strategy_configs/account_strategy_binding.example.json` | **账户↔策略绑定运行配置**：实例化运行参数。含 python_file(启动哪个脚本)、enabled、dynamic_symbols、redis、factor_params、trading_params(interval/leverage/调仓周期等)。主服务器据此把策略注册到进程管理器（状态 pending，待手动启动）。 |
| `acount_configs/binance_account.example.json` | 账户凭证配置：account_id/exchange/api_key/secret_key/is_testnet。主服务器启动时遍历此目录注册账户。 |

---

## trading/ — 交易域

订单模型、风控、账户注册表、配置加载器，以及告警脚本。多为 header-only。

| 文件 | 职责 |
|------|------|
| `order.h` | 订单模型：`OrderType`(LIMIT/MARKET/POST_ONLY/FOK/IOC)、`OrderSide`(BUY/SELL)、`OrderState` 生命周期(CREATED→SUBMITTED→ACCEPTED→PARTIALLY_FILLED→FILLED/CANCELLED) 及订单结构。 |
| `risk_manager.h` | **关键**。`RiskManager` 风险管理器：订单前置检查 `check_order_with_value`(单笔金额/数量/挂单数/单品种持仓/总敞口/单日亏损/下单频率)、按策略的回撤检查 `update_account_equity`(支持 daily_peak/daily_initial 两种模式)、紧急止损 Kill Switch、策略邮箱/飞书注册与定向告警。内含 `AlertService`(异步 popen 调用 `alerts/` 下 Python 脚本发邮件+飞书)、`RiskLimits`(从 `risk_config.json` 加载)、`RiskCheckResult`。 |
| `account_registry.h` | `AccountRegistry` 账户注册表：多账户/多策略/多市场(`ExchangeType` OKX/Binance + Binance SPOT/FUTURES/COIN)。注册/注销/更新、默认账户、线程安全查询、`get_all_okx_accounts`/`get_all_binance_accounts`、健康检查、配置持久化。 |
| `config_loader.h` | 配置加载器：`load_json_file`、加载账户配置、环境变量覆盖等通用工具函数。 |
| `strategy_config_loader.h` | 策略配置加载器 `StrategyConfigManager`：从 `strategies/configs/` 读每个策略配置（账户/参数/联系人 `ContactInfo`），自动注册账户到 `AccountRegistry`，并给风控端提供查询接口。 |

### trading/alerts/

被 C++ (`AlertService`) 和 Python 共用的告警脚本。

| 文件 | 职责 |
|------|------|
| `email_alert.py` | 邮件告警 `EmailAlertService`：支持 SMTP / SendGrid / Mailgun；可 Python 直接调用，也可命令行调用（供 C++ popen）。 |
| `lark_alert.py` | 飞书告警 `LarkAlertService`：支持 Webhook 群通知 + Open API 单聊私信(富文本卡片)；同样支持命令行调用。 |
| `__init__.py` | `AlertManager` 统一告警管理器：按级别路由到 email/lark 渠道、记录告警历史、`create_alert_manager_from_env` 从环境变量构建。 |
| `email_config.example.json` | 邮件告警配置示例（SMTP 主机/账号/授权码/收件人）。 |
| `lark_config.example.json` | 飞书告警配置示例（webhook/secret/app_id/app_secret）。 |

---

## totalconfig/ — 全局总配置

跨模块的总配置示例（真实文件含密钥，不入库）。

| 文件 | 职责 |
|------|------|
| `network_monitor_config.example.json` | VPN/代理网络监控配置：代理地址、探测目标(OKX/Binance)、失败阈值/冷却、恢复通知，以及内嵌的邮件告警配置。对应 `vpn_network_monitor.h`。 |
| `email_alert_network.example.json` | 网络监控专用邮件告警配置（SMTP）。崩溃/下架等告警也复用此配置文件路径。 |

---

## user_configs/ — 前端登录用户

| 文件 | 职责 |
|------|------|
| `user.example.json` | 前端登录用户配置示例：username/role(`SUPER_ADMIN`/`STRATEGY_MANAGER`)/password_hash(SHA256)/salt/allowed_strategies。被 `auth_manager.h` 加载。 |

---

## scripts/ — 运维脚本

启停、K线补数/预加载、净值统计、交割单导出等。多为独立进程/cron，**与 C++ 主进程解耦，可随时重启、零实盘风险**。

### 启停与监控

| 文件 | 职责 |
|------|------|
| `start_all.sh` | 一键启动：在 tmux 会话中拉起 `trading_server_full` + `data_recorder` + `kline_gap_filler` + 监控脚本（默认直连交易所、清空代理环境变量）。 |
| `stop_all.sh` | 停止全部交易系统进程（tmux 会话 + 策略子进程）。 |
| `monitor_btc_klines.sh` | 实时监控 BTC K线写入（起止时间、连续性、重复检测）。 |
| `check_klines_stats.sh` | K线数据统计工具（summary/full/symbol/gaps 多模式）。 |
| `run_gap_filler_loop.sh` | 循环执行 `kline_gap_filler`，每轮间隔 3s。 |
| `fix_duplicate_klines.sh` | K线重复数据去重 + 智能加载的完整流程。 |

### K线补数与预加载

| 文件 | 职责 |
|------|------|
| `auto_kline_monitor_v2.py` | 自动监控并补全各周期 K线连续性：经 OKX REST 拉缺失 1m K线、聚合其他周期写回 Redis，定时运行（默认 30min）。 |
| `fast_kline_filler.py` | 快速 K线补全：redis-py pipeline 批量、按交易所分组限频、只扫最近 N 小时(默认 12h)、补 1m 后立即聚合多周期、429 自动退避。 |
| `preload_klines_to_redis.py` | 预加载历史 K线到 Redis（从 Binance/OKX 拉，支持 `--days`/`--interval`/`--exchange`）。 |
| `preload_1d_klines.py` | 预加载 120 天 1d K线（全市场 USDT 永续），供日频策略；写后截断到 120 条并设 4 个月过期。 |
| `preload_funding_rate.py` | Binance 永续资金费率日频预加载/增量补缺，扫描过去 120 天缺哪些 UTC 日并只拉缺失天。 |
| `reaggregate_klines.py` | 从 Redis 已有 1m K线重新聚合 5m/15m/30m（含 amount/buy_amount 字段）。 |
| `refill_from_vision.py` | 从 `data.binance.vision` 拉 K线 CSV(zip) 写入 Redis（多线程）。 |
| `boundary_probe.py` | 00:00 UTC 边界观测器：测量新 K线到 Redis 的时序，定位数据延迟根因（只读，不影响实盘）。 |
| `test_redis_8h_klines.py` | 诊断脚本：用 Lua 服务端脚本高频监测 Redis 中 8h K线变化(<1ms/轮)。 |

### 净值统计与导出

| 文件 | 职责 |
|------|------|
| `equity_recorder.py` | 按账户每 5min 读 `/fapi/v2/account` 写净值时序到 Redis(`equity_history`/`equity_latest`)，为净值曲线/夏普/回撤供数。独立 cron、只读不下单。 |
| `equity_backfill.py` | 回填 Binance 历史净值到 Redis：用 `/fapi/v1/income` 重建钱包净值曲线(按小时聚合) + `/sapi accountSnapshot` 补更早段，让前端历史曲线更密。 |
| `stats_api.py` | 账户/策略统计只读 HTTP API(127.0.0.1:8003)：读 Redis 算夏普/回撤/年化/收益率，给前端面板。stdlib http.server，零额外依赖。 |
| `stats_api_watchdog.sh` | `stats_api` 看门狗：探活 `/api/health`，不通才杀旧拉新（独立脚本避免误杀 cron 自身）。 |
| `export_trade_statements.py` | 导出账户交割单(逐笔成交明细)为 CSV：经 `/fapi/v1/income` 发现交易过的 symbol + `/fapi/v1/userTrades` 翻页拉全量成交，含限频退避。 |
| `STATS_STACK.md` | 统计栈说明文档：净值记录器 + 统计 API + 起始本金 三件套，强调完全独立于 C++ 主进程。 |

### 最小下单单位拉取

| 文件 | 职责 |
|------|------|
| `fetch_binance_min_qty.py` | 拉取 Binance 所有 USDT 永续合约的最小下单数量(minQty) 写配置文件。 |
| `fetch_okx_min_coin_qty.py` | 拉取 OKX 所有 USDT 永续的最小下单张数(minSz) 与每张面值(ctVal)，用于「下单金额↔张数」换算。 |
