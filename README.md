**简体中文** | [English](./README.en.md)

# Crypto Trading Framework

> 一套给**一个人**也能跑起来的加密量化全栈参考实现：**C++17 事件驱动引擎**（热路径）+ **pybind11 Python 策略层** + **真·Vue3 实时操作台**。面向 Binance / OKX 永续合约，自带盘前风控、kill-switch、多账户进程级隔离与多渠道告警。

<p align="center">
  <img alt="License" src="https://img.shields.io/badge/License-MIT-22c55e.svg">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=cplusplus">
  <img alt="Vue3" src="https://img.shields.io/badge/Vue-3-42b883.svg?logo=vuedotjs">
  <img alt="Python" src="https://img.shields.io/badge/Python-3.11-3776AB.svg?logo=python&logoColor=white">
  <img alt="Stars" src="https://img.shields.io/github/stars/paidaxing1234/crypto-trading-framework?style=social">
</p>

> ⚠️ **这不是一台印钞机。** 仓库里**没有 alpha、没有密钥、没有模型**——每一点 edge 我都给你剥光了。这是**水管**，edge 你自己带。它的价值在于工程骨架，而不是稳赚的信号。

---

## 效果预览（架构一眼看懂）

```
   ┌──────────────┐        REST + WebSocket        ┌─────────────────────────────┐
   │  Binance/OKX │  ◄──────────────────────────►  │     C++17 事件驱动引擎       │
   │   永续合约    │   行情订阅 / 下单 / 账户推送    │  core · adapters · network  │
   └──────────────┘                                │  trading(盘前风控/订单/告警) │
                                                    └──────────────┬──────────────┘
                                                       pybind11 绑定 │ (strategy_base)
                                                    ┌──────────────▼──────────────┐
                                                    │      Python 策略层           │
                                                    │  你的策略 = StrategyBase 子类 │
                                                    └──────────────┬──────────────┘
                                                                   │ Redis (行情/状态中枢)
                                                    ┌──────────────▼──────────────┐
   ┌──────────────┐    WebSocket :8002 (实时推送)   │   C++ UI Server (前端网关)   │
   │  Vue3 操作台  │  ◄──────────────────────────►  │   持仓/订单/权益/策略管理     │
   └──────────────┘    登录 · 下单 · 监控 · 风控     └─────────────────────────────┘
```

> 链路：交易所 ↔ C++ 引擎（热路径在这里）↔ pybind11 ↔ Python 策略 ↔ Redis ↔ Vue3 面板。
> 引擎吃行情、跑风控、发单；策略只写逻辑；面板只做人机交互。三层各司其职。

---

## 它是什么 / 为什么要造它

**一句话**：给独立量化做的「生产级全栈参考实现」——不是 demo，是真的有人用真钱跑过的那套骨架，去掉了赚钱的部分。

市面上的开源 bot，要么是 Python 单进程（freqtrade / hummingbot），要么虽然成熟但**没有自带操作台 UI**（nautilus_trader）。我想要的是：

- **热路径在 C++**：行情解析、风控校验、订单提交走 C++17 事件引擎，Python 不挡在关键路径上。
- **策略层用 Python**：研发效率不能丢，策略继承一个基类、重写几个回调就能跑，C++ 的能力通过 pybind11 直接暴露。
- **真有一块能用的操作台**：Vue3 实时面板，能看持仓/订单/权益曲线、能下单、能管策略、能权限控制——这是绝大多数开源 bot 缺的那块拼图。

如果你也是一个人扛全栈、想要一个**能改、能读、能扩**的真实骨架，而不是又一个 toy backtest，这个仓库就是给你的。

---

## 核心特性

- **C++17 热路径引擎**：事件驱动核心，Binance / OKX 的 REST + WebSocket 适配层统一封装行情订阅、下单、账户推送。前端通过 WebSocket 直推，省掉一层 HTTP 轮询。
- **pybind11 策略层**：把 C++ 引擎能力（行情查询、下单、账户、定时任务）暴露成一个 Python 基类 `StrategyBase`。你的策略就是它的子类，重写 `on_kline` / `on_trade` / `on_order_report` 等回调即可，研发体验接近纯 Python。
- **真·Vue3 操作台**：Vue 3 `<script setup>` + Pinia + Vue Router + Element Plus + ECharts。实时持仓、订单、权益曲线、策略启停、用户/权限管理。登录与实时数据**统一走 WebSocket**，不是贴个 REST 后台糊弄。
- **盘前风控 + kill-switch**：下单**前**逐项校验——单笔金额上限、单品种持仓上限、总敞口上限、最大挂单数、按回撤模式（当日峰值/当日初值）止损、单日亏损上限、每秒/每分钟限频。触发即拦单，配置见 `cpp/risk_config.json`。
- **多账户 / 多策略进程级隔离**：账户与策略解耦绑定，每个策略实例独立进程运行。一个策略崩了不会拖垮别的，单账户出问题不影响整盘。
- **多渠道告警**：邮件（SMTP）+ 飞书（Lark）双通道，覆盖订单异常、风控触发、VPN/代理网络监控、交易所下线币种监控。（实现上告警走 `subprocess` 调用，见「已知局限」。）

---

## 为什么不直接用 freqtrade / hummingbot / nautilus？

诚实对比，没有踩谁。它们都很好，只是定位不同：

| 维度 | **本框架** | freqtrade | hummingbot | nautilus_trader |
|---|---|---|---|---|
| 热路径语言 | **C++17 事件引擎** | Python | Python（核心含 Cython） | Python + Rust 核心 |
| 策略开发语言 | Python（pybind11 绑定 C++） | Python | Python / YAML 配置 | Python |
| 自带实时操作台 UI | **✅ Vue3 全功能面板** | 部分（FreqUI，偏只读监控） | 偏 CLI / 有限 Web | ❌ 无自带 UI |
| 盘前风控 + kill-switch | **✅ 引擎级，下单前拦截** | 策略层为主 | 有限 | ✅ 较完整 |
| 多账户多策略进程隔离 | **✅ 进程级** | 单进程为主 | 单进程为主 | 进程内多策略 |
| 成熟度 / 测试覆盖 | ⚠️ **单人项目，无测试套件** | 高，社区大 | 高，社区大 | 高，工程严谨 |
| 定位 | 可读可改的**全栈骨架** | 开箱即用的零售 bot | 做市 / 套利 bot | 机构级研究/执行平台 |

**一句话选型**：想要开箱即用、社区大 → 选 freqtrade / hummingbot；想要机构级严谨 → 选 nautilus_trader；想要一个**带操作台 UI、C++ 热路径、能整体读懂改动的全栈参考骨架**，并且不介意它是单人项目 → 留下来看看本仓库。

---

## 30 行最小策略示例

策略 = 继承 `StrategyBase`，重写你关心的回调。完整 API 参考见 `cpp/strategies/implementations/example_strategy.py`。

```python
from strategy_base import StrategyBase, KlineBar  # pybind11 模块，需先编译

class MyStrategy(StrategyBase):
    def __init__(self, strategy_id: str, symbol: str):
        super().__init__(strategy_id, max_kline_bars=7200)
        self.symbol = symbol

    def on_init(self):
        # 注册账户（密钥从你的配置/环境读，别硬编码）
        self.register_account(api_key="...", secret_key="...",
                              passphrase="...", is_testnet=True)
        self.subscribe_kline(self.symbol, "1m")   # 订阅 1 分钟 K 线
        self.log_info(f"已启动，订阅 {self.symbol}")

    def on_kline(self, symbol: str, interval: str, bar: KlineBar):
        closes = self.get_closes(symbol, interval)
        if len(closes) < 20:
            return
        ma20 = sum(closes[-20:]) / 20          # 你的 edge 写在这里（示例：均线）
        if bar.close > ma20 and self.get_position(symbol) is None:
            self.send_swap_market_order(symbol, "buy", 1)   # 盘前风控会先拦一遍

    def on_order_report(self, report: dict):
        self.log_info(f"订单 {report.get('status')}: {report.get('symbol')}")

if __name__ == "__main__":
    MyStrategy("demo", "BTC-USDT-SWAP").run()
```

> 这只是接口演示，**不是可盈利策略**。`ma20` 那行换成你自己的逻辑——edge 你自己带。

---

## 技术栈

| 层 | 技术 |
|---|---|
| 交易引擎 | C++17、CMake、OpenSSL、libcurl、websocketpp（+ Asio）、ZeroMQ、nlohmann/json |
| 数据中枢 | Redis（hiredis）、ClickHouse（权益/统计） |
| 策略层 | Python 3.11、pybind11 |
| 前端 | Vue 3、Vite 5、Pinia、Vue Router、Element Plus、ECharts、Axios |
| 辅助脚本 | Python（K 线预加载、权益记录、stats API、对账导出） |
| 部署 | Ubuntu、Nginx、systemd、Docker（可选） |

---

## 快速开始

### 0. 系统依赖（Ubuntu）

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake \
  libssl-dev libcurl4-openssl-dev \
  libhiredis-dev libzmq3-dev \
  nlohmann-json3-dev pybind11-dev \
  libwebsocketpp-dev libasio-dev \
  redis-server python3-dev python3-pip
```

> `libwebsocketpp-dev` + `libasio-dev` 是 WebSocket 服务端必需（websocketpp 是 header-only，依赖 standalone Asio），漏装会在 cmake/编译阶段报头文件找不到。

### 1. 构建 C++ 引擎（统一路径）

```bash
cd cpp && mkdir build && cd build && cmake .. && make -j
```

产出可执行：`trading_server_full`（前端 WS 网关）、`data_recorder`、`kline_gap_filler`、`kline_fast_filler`，以及 pybind11 策略模块 `strategy_base`。

### 2. Python 策略 / 脚本环境

```bash
pip install -r requirements.txt
```

`cpp/scripts/` 下的运维脚本（K 线预加载、权益记录、stats API 等）依赖这些包。

### 3. 前端操作台

```bash
cd 实盘框架前端页面
npm install
npm run dev      # 开发，默认 http://localhost:3000
npm run build    # 生产构建，产物在 dist/
```

### 4. 配置密钥（先复制 .example，再填真值）

**绝不把真实密钥写进代码或提交到 Git。** 推荐走环境变量：

```bash
cp .env.example .env       # 然后填入你自己的密钥
```

引擎读取的环境变量（见 `cpp/server/config/server_config.cpp`）：

```bash
# OKX
OKX_API_KEY=...
OKX_SECRET_KEY=...
OKX_PASSPHRASE=...
OKX_TESTNET=1                 # 1=测试网, 0=主网

# Binance
BINANCE_API_KEY=...
BINANCE_SECRET_KEY=...
BINANCE_TESTNET=1             # 1=测试网, 0=主网

# 生产部署额外项（见 improve/04_deployment_guide.md）
REDIS_PASSWORD=...
JWT_SECRET=...
ADMIN_PASSWORD=...
```

各类 JSON 配置（账户、用户、告警、策略绑定）的字段含义与脱敏模板位置，详见 **[CONFIG.md](./CONFIG.md)**。

### 5.（可选）Docker Compose 一键拉起依赖

```bash
docker compose up -d        # 启动 Redis 等基础设施
```

---

## 端口拓扑（一次说清，别再猜）

| 端口 | 进程 | 协议 | 谁连它 | 暴露策略 |
|---|---|---|---|---|
| **8002** | `trading_server_full`（C++ UI Server） | **WebSocket** | **前端**（登录、下单、实时持仓/订单/权益全走这条） | 生产环境 `bind 127.0.0.1`，由 Nginx 反代 `wss://域名/ws` 对外 |
| **8003** | `stats_api.py`（Python 统计 API） | HTTP | 前端「统计」页面 | **仅本地**，前端开发期由 Vite 代理 `/stats-api`，生产由 Nginx 加一段 `location /stats-api/` |
| **3000** | Vite dev server | HTTP | 你的浏览器（开发期） | 仅开发，生产用 `npm run build` 产物 + Nginx 静态托管 |
| 6379 | Redis | — | 引擎 / 脚本 | **绝不公网**，`bind 127.0.0.1` + `requirepass` |

**关键关系**：前端的**核心实时通道是 8002 的 WebSocket**（连登录都走它），8003 只是一个**只读统计旁路**且仅监听本地。两者不要混淆——8002 是主动脉，8003 是体检报告。开发期 Vite 把 `/ws → :8002`、`/stats-api → :8003` 都代理到同源，生产期由 Nginx 做同样的事 + TLS 终结。

---

## 安全提示（用真钱跑之前务必读）

- **密钥走环境变量 / `.env`**，仓库只提供 `.env.example` 与 `*.example.json` 模板，真实文件已被 `.gitignore` 拦截。详见 **[CONFIG.md](./CONFIG.md)**。
- **交易所 API Key**：只授予「合约交易」权限，**绑定服务器 IP 白名单**，**关闭提币权限**。即使泄露，对方也提不走钱。
- **小资金先验**：`*_TESTNET=1` 先在测试网，再用小资金主网验证，确认行为无误后才放量。
- **基础设施不公网**：Redis / ClickHouse `bind 127.0.0.1` + 强密码 + 防火墙；8002 也绑本地、只经 Nginx + TLS 对外。
- **疑似泄露立即吊销**：去交易所 / 邮箱 / 飞书后台吊销重置，别犹豫。
- 生产部署的完整加固清单见 **[improve/04_deployment_guide.md](./improve/04_deployment_guide.md)**，性能调优见 **[improve/02_performance_cpp.md](./improve/02_performance_cpp.md)**。

---

## 已知局限 / 路线图（诚实区）

我不想骗你 star，所以把短板写在明面上：

- **没有测试套件、没有公开 benchmark。** 我不吹「最快 HFT」——这套引擎是事件驱动、热路径在 C++，但没有可复现的延迟基准。别拿它当低延迟教科书，拿它当全栈骨架。
- **只接了两个交易所**：Binance + OKX（永续合约）。接别的交易所要自己照着 `adapters/` 写适配层。
- **单人项目**：成熟度、文档完整度、边界 case 处理都比不过 freqtrade / nautilus 那种大社区项目。能读懂、能改是它的卖点，「开箱即生产」不是。
- **告警走 `subprocess`** 调用外部脚本发邮件/飞书，不是常驻消息队列——量大时不够优雅，够用但不极致。
- **路线图（无承诺时间表）**：补单元/集成测试；公开一份可复现的延迟 benchmark；抽象出更通用的 adapter 接口以便接更多交易所；把告警从 subprocess 迁到常驻 worker。

欢迎 issue / PR，但请理解这是业余时间维护的单人项目。

---

## ⚠️ 免责声明（请认真读完）

> **真实资金风险。** 本框架用于真实资金的加密货币衍生品交易，加密永续合约**带杠杆、可爆仓、可在极短时间内亏光本金甚至更多**。
>
> **非投资建议。** 本仓库不构成任何投资、财务或交易建议，不推荐任何币种、不荐股、不代客理财。代码中出现的任何参数、标的、示例策略均为**技术演示**，非交易信号。
>
> **不承诺任何收益。** 本框架**不保证盈利**，过往任何表现（如有）不代表未来结果。仓库中**不包含**可盈利的 alpha 策略、ML 模型或真实密钥——edge 需要你自己带。
>
> **按 MIT 许可「按现状」提供，不附带任何明示或默示担保**（包括但不限于适销性、特定用途适用性、不间断或无错误运行）。任何人据此进行实盘交易，**所有盈亏与风险由你自己承担**，作者不对任何直接或间接损失负责。
>
> 在用真钱之前：先读代码、先跑测试网、先用你亏得起的小资金。

---

## License

[MIT](./LICENSE) © paidaxing1234

> 本仓库为框架核心的公开分享版本，**已剔除全部专有策略（alpha）、ML 模型、真实密钥与个人交易数据**。它是水管，edge 你自己带。
