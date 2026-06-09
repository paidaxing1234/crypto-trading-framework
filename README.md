# Real-Account Trading Framework（实盘量化交易框架）

一套面向加密货币（Binance / OKX）永续合约的 **高频实盘量化交易框架**：C++17 低延迟交易引擎 + Python 策略层 + Vue3 实时监控前端。支持 Tick/Kline 级行情、实时风控、多账户多策略隔离运行与订单执行。

> ⚠️ **风险提示**：本框架用于真实资金交易，仅作工程参考与学习。任何人据此进行实盘交易，盈亏与风险自负。仓库中**不包含**任何真实密钥、账户配置或专有 alpha 策略，所有配置均为脱敏 `*.example.json` 模板。

---

## ✨ 核心特性

- **低延迟 C++ 引擎**：基于事件驱动（event engine）的核心撮合/行情处理，WebSocket 直连前端（1–5ms）。
- **多交易所适配**：Binance / OKX 的 REST + WebSocket 适配层，统一下单/行情接口。
- **实时风控**：单笔/单品种/总敞口限额、日内回撤、下单频率限制（见 `cpp/risk_config.json`）。
- **多账户多策略**：账户与策略解耦绑定，进程级隔离运行（`strategy_process_manager`）。
- **Python 策略层**：通过 pybind11 将 C++ 引擎能力暴露给 Python 策略，兼顾性能与研发效率。
- **Redis 行情中枢**：Kline/Tick 预加载至 Redis，行情广播与持久化（`redis_data_provider` / `redis_recorder`）。
- **告警体系**：邮件（SMTP）+ 飞书（Lark）多渠道告警，含 VPN/代理网络监控与下线币种监控。
- **Vue3 监控前端**：实时持仓、订单、权益曲线、策略管理、权限控制（Pinia + ElementPlus + ECharts）。

---

## 🏗️ 架构总览

```
                    ┌─────────────────────────────┐
   Binance / OKX    │   C++ Trading Engine (核心)  │
   REST + WS  ◄────►│  core / adapters / network   │
                    │  trading(风控/订单) / server │
                    └──────────────┬──────────────┘
                                   │ pybind11
                          ┌────────▼────────┐
                          │  Python 策略层   │  strategies/
                          └────────┬────────┘
                                   │ Redis (行情/状态)
                    ┌──────────────▼──────────────┐
   Vue3 前端  ◄──── WebSocket(1–5ms) ──── C++ UI Server
```

## 📂 目录结构

```
cpp/
├── core/              # 事件引擎、日志、配置中心、核心数据结构
├── adapters/
│   ├── binance/       # 币安 REST + WebSocket 适配
│   └── okx/           # OKX REST + WebSocket 适配
├── network/           # 网络层
├── server/            # UI Server（WebSocket）
│   ├── handlers/      # 下单处理、查询、订阅、前端指令
│   ├── managers/      # 账户/Redis/策略进程/下线监控
│   └── klinedata/     # K线数据
├── trading/           # 订单、风控、账户注册、配置加载
│   └── alerts/        # 邮件 / 飞书告警
├── strategies/
│   ├── core/          # 策略基类 + pybind 绑定
│   ├── utils/         # 策略工具
│   └── implementations/
│       ├── grid/                # 示例：网格策略
│       └── example_strategy.py  # 示例：策略模板
├── scripts/           # 运维脚本（K线预加载、权益记录、stats API 等）
└── risk_config.json   # 全局风控配置

实盘框架前端页面/      # Vue3 实时监控前端
├── src/               # 组件 / 视图 / 状态 / 服务
├── deploy/            # nginx / SSL / 部署脚本
└── database/          # ClickHouse schema

advise/ improve/ yunwei/  # 工程笔记、性能/安全清单、运维指南
PORTAL_*.md               # 监控门户集成文档
```

## 🔧 技术栈

| 层 | 技术 |
|----|------|
| 交易引擎 | C++17、CMake、OpenSSL、libcurl、websocketpp、ZeroMQ、nlohmann/json |
| 数据中枢 | Redis（hiredis）、ClickHouse |
| 策略层 | Python 3.11、pybind11 |
| 前端 | Vue 3、Vite、Pinia、Vue Router、Element Plus、ECharts、Axios |
| 部署 | Ubuntu、Nginx、Docker（可选） |

## 🚀 快速开始

### 1. 构建 C++ 引擎

```bash
# Ubuntu 依赖（示例）
sudo apt install -y build-essential cmake libssl-dev libcurl4-openssl-dev \
                    libhiredis-dev libzmq3-dev nlohmann-json3-dev pybind11-dev

cd cpp
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

### 2. 配置（重要：先复制脱敏模板再填入你自己的密钥）

所有真实配置都被 `.gitignore` 排除，仓库只提供 `*.example.json` 模板。详见 [CONFIG.md](./CONFIG.md)。

```bash
cp cpp/strategies/acount_configs/binance_account.example.json \
   cpp/strategies/acount_configs/binance_main.json   # 填入你的 API Key/Secret
# 其余告警、用户、策略配置同理
```

### 3. 启动前端

```bash
cd 实盘框架前端页面
npm install
npm run dev      # 开发
npm run build    # 生产构建
```

## ⚙️ 配置说明

请阅读 **[CONFIG.md](./CONFIG.md)**，其中列出了每一类配置文件的字段含义与脱敏模板位置。
**切勿将填好真实密钥的配置文件提交到任何仓库。**

## 📜 License

[MIT](./LICENSE)

---

> 本仓库为框架核心的公开分享版本，**已剔除全部专有策略（alpha）、ML 模型、真实密钥与个人交易数据**。
