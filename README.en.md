[简体中文](./README.md) | **English**

# Crypto Trading Framework

> A production-grade, full-stack reference implementation for **solo quant traders**: a **C++17 event-driven engine** (the hot path) + a **pybind11 Python strategy layer** + a **real Vue 3 live control panel**. Built for Binance / OKX perpetual futures, with pre-trade risk checks, a kill-switch, per-process account/strategy isolation, and multi-channel alerts.

<p align="center">
  <img alt="License" src="https://img.shields.io/badge/License-MIT-22c55e.svg">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=cplusplus">
  <img alt="Vue3" src="https://img.shields.io/badge/Vue-3-42b883.svg?logo=vuedotjs">
  <img alt="Python" src="https://img.shields.io/badge/Python-3.11-3776AB.svg?logo=python&logoColor=white">
  <img alt="Stars" src="https://img.shields.io/github/stars/paidaxing1234/crypto-trading-framework?style=social">
</p>

> ⚠️ **This is not a money printer.** There is **no alpha, no keys, no model** in this repo — I've stripped out every edge on purpose. This is the **plumbing**; the edge is yours to bring. Its value is the engineering skeleton, not a magic signal.

---

## At a glance (the architecture in one diagram)

```
   ┌──────────────┐        REST + WebSocket        ┌─────────────────────────────┐
   │  Binance/OKX │  ◄──────────────────────────►  │   C++17 event-driven engine │
   │  perpetuals  │   market data / orders / fills │  core · adapters · network  │
   └──────────────┘                                │  trading(pre-trade risk/    │
                                                    │          orders/alerts)     │
                                                    └──────────────┬──────────────┘
                                                      pybind11 binding │ (strategy_base)
                                                    ┌──────────────▼──────────────┐
                                                    │      Python strategy layer   │
                                                    │  your strategy = StrategyBase│
                                                    └──────────────┬──────────────┘
                                                                   │ Redis (data/state hub)
                                                    ┌──────────────▼──────────────┐
   ┌──────────────┐   WebSocket :8002 (live push)  │    C++ UI Server (gateway)   │
   │ Vue3 control │  ◄──────────────────────────►  │  positions/orders/equity/    │
   │    panel     │   login · order · monitor · risk│  strategy management        │
   └──────────────┘                                └─────────────────────────────┘
```

> Flow: exchange ↔ C++ engine (hot path lives here) ↔ pybind11 ↔ Python strategy ↔ Redis ↔ Vue 3 panel.
> The engine ingests data, runs risk, sends orders; the strategy only writes logic; the panel only handles the human. Three layers, clean separation.

---

## What it is / why I built it

**In one line:** a *production-grade full-stack reference implementation* for the solo quant — not a demo, but the actual skeleton someone ran with real money, with the money-making part removed.

Most open-source bots are either single-process Python (freqtrade / hummingbot), or mature but **ship no control-panel UI** (nautilus_trader). What I wanted was:

- **Hot path in C++.** Market-data parsing, risk validation, and order submission run on a C++17 event engine. Python is never in the critical path.
- **Strategies in Python.** Research velocity matters. A strategy subclasses one base class and overrides a few callbacks; the engine's capabilities are exposed straight through pybind11.
- **An actual usable control panel.** A real-time Vue 3 dashboard: positions, orders, equity curve, order entry, strategy management, RBAC. This is the missing piece in nearly every open-source bot.

If you also carry the whole stack alone and want a real skeleton you can **read, modify, and extend** — instead of yet another toy backtest — this repo is for you.

---

## Core features

- **C++17 hot-path engine.** An event-driven core with Binance / OKX REST + WebSocket adapters unified behind one interface for subscriptions, order entry, and account pushes. The frontend gets data via WebSocket push — no HTTP polling layer in between.
- **pybind11 strategy layer.** The engine's capabilities (market queries, order entry, account, scheduled tasks) are exposed as a Python base class, `StrategyBase`. Your strategy is a subclass; override callbacks like `on_kline` / `on_trade` / `on_order_report`. The dev experience feels close to pure Python.
- **A real Vue 3 control panel.** Vue 3 `<script setup>` + Pinia + Vue Router + Element Plus + ECharts. Live positions, orders, equity curve, start/stop strategies, user/role management. Login **and** live data both run **over WebSocket** — not a REST admin panel bolted on as an afterthought.
- **Pre-trade risk + kill-switch.** Every order is validated **before** it leaves: per-order notional cap, per-symbol position cap, total-exposure cap, max open orders, drawdown stop (daily-peak or daily-initial mode), daily loss limit, and per-second / per-minute rate limits. A breach blocks the order. Config lives in `cpp/risk_config.json`.
- **Per-process account / strategy isolation.** Accounts and strategies are decoupled and bound; each strategy instance runs in its own process. One blown strategy won't drag down the others; one bad account won't take down the book.
- **Multi-channel alerts.** Email (SMTP) + Lark (Feishu), covering order anomalies, risk triggers, VPN/proxy network monitoring, and exchange-delisting detection. (Note: alerts are dispatched via `subprocess` — see *Known limitations*.)

---

## Why not just use freqtrade / hummingbot / nautilus?

An honest comparison — no shade. They're all good; they just aim at different targets:

| Dimension | **This framework** | freqtrade | hummingbot | nautilus_trader |
|---|---|---|---|---|
| Hot-path language | **C++17 event engine** | Python | Python (Cython in core) | Python + Rust core |
| Strategy language | Python (pybind11 → C++) | Python | Python / YAML config | Python |
| Built-in live control-panel UI | **✅ Full Vue 3 panel** | Partial (FreqUI, mostly read-only) | CLI-leaning / limited web | ❌ No built-in UI |
| Pre-trade risk + kill-switch | **✅ Engine-level, blocks before send** | Mostly strategy-level | Limited | ✅ Fairly complete |
| Multi-account/strategy process isolation | **✅ Per process** | Mostly single-process | Mostly single-process | In-process multi-strategy |
| Maturity / test coverage | ⚠️ **Solo project, no test suite** | High, large community | High, large community | High, rigorous |
| Positioning | A readable, hackable **full-stack skeleton** | Turn-key retail bot | Market-making / arb bot | Institutional research/exec platform |

**Pick in one line:** want turn-key with a big community → freqtrade / hummingbot; want institutional rigor → nautilus_trader; want a **full-stack reference skeleton with a real control-panel UI, a C++ hot path, and changes you can actually read end-to-end** — and you don't mind it's a solo project → stick around.

---

## Minimal strategy in 30 lines

A strategy = subclass `StrategyBase`, override the callbacks you care about. Full API reference: `cpp/strategies/implementations/example_strategy.py`.

```python
from strategy_base import StrategyBase, KlineBar  # pybind11 module — build it first

class MyStrategy(StrategyBase):
    def __init__(self, strategy_id: str, symbol: str):
        super().__init__(strategy_id, max_kline_bars=7200)
        self.symbol = symbol

    def on_init(self):
        # Register the account (read keys from env/config — never hard-code)
        self.register_account(api_key="...", secret_key="...",
                              passphrase="...", is_testnet=True)
        self.subscribe_kline(self.symbol, "1m")   # subscribe to 1-minute klines
        self.log_info(f"started, subscribed {self.symbol}")

    def on_kline(self, symbol: str, interval: str, bar: KlineBar):
        closes = self.get_closes(symbol, interval)
        if len(closes) < 20:
            return
        ma20 = sum(closes[-20:]) / 20          # your edge goes here (example: an MA)
        if bar.close > ma20 and self.get_position(symbol) is None:
            self.send_swap_market_order(symbol, "buy", 1)   # pre-trade risk runs first

    def on_order_report(self, report: dict):
        self.log_info(f"order {report.get('status')}: {report.get('symbol')}")

if __name__ == "__main__":
    MyStrategy("demo", "BTC-USDT-SWAP").run()
```

> This is an API demo, **not a profitable strategy**. Replace the `ma20` line with your own logic — the edge is yours to bring.

---

## Tech stack

| Layer | Tech |
|---|---|
| Trading engine | C++17, CMake, OpenSSL, libcurl, websocketpp (+ Asio), ZeroMQ, nlohmann/json |
| Data hub | Redis (hiredis), ClickHouse (equity/stats) |
| Strategy layer | Python 3.11, pybind11 |
| Frontend | Vue 3, Vite 5, Pinia, Vue Router, Element Plus, ECharts, Axios |
| Helper scripts | Python (kline preload, equity recording, stats API, statement export) |
| Deployment | Ubuntu, Nginx, systemd, Docker (optional) |

---

## Quick start

### 0. System dependencies (Ubuntu)

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake \
  libssl-dev libcurl4-openssl-dev \
  libhiredis-dev libzmq3-dev \
  nlohmann-json3-dev pybind11-dev \
  libwebsocketpp-dev libasio-dev \
  redis-server python3-dev python3-pip
```

> `libwebsocketpp-dev` + `libasio-dev` are required by the WebSocket server (websocketpp is header-only and depends on standalone Asio). Missing them surfaces as a "header not found" error during cmake/compile.

### 1. Build the C++ engine (single path)

```bash
cd cpp && mkdir build && cd build && cmake .. && make -j
```

Produces: `trading_server_full` (frontend WS gateway), `data_recorder`, `kline_gap_filler`, `kline_fast_filler`, and the pybind11 strategy module `strategy_base`.

### 2. Python strategy / script environment

```bash
pip install -r requirements.txt
```

The ops scripts under `cpp/scripts/` (kline preload, equity recorder, stats API, etc.) depend on these packages.

### 3. Frontend control panel

```bash
cd 实盘框架前端页面   # "real-account frontend"
npm install
npm run dev      # dev server, default http://localhost:3000
npm run build    # production build → dist/
```

### 4. Configure secrets (copy .example first, then fill real values)

**Never put real keys in source or commit them to Git.** Prefer environment variables:

```bash
cp .env.example .env       # then fill in your own keys
```

Environment variables the engine reads (see `cpp/server/config/server_config.cpp`):

```bash
# OKX
OKX_API_KEY=...
OKX_SECRET_KEY=...
OKX_PASSPHRASE=...
OKX_TESTNET=1                 # 1=testnet, 0=mainnet

# Binance
BINANCE_API_KEY=...
BINANCE_SECRET_KEY=...
BINANCE_TESTNET=1             # 1=testnet, 0=mainnet

# Extra for production deploy (see improve/04_deployment_guide.md)
REDIS_PASSWORD=...
JWT_SECRET=...
ADMIN_PASSWORD=...
```

For the JSON configs (accounts, users, alerts, strategy bindings) — field meanings and the location of every sanitized template — see **[CONFIG.md](./CONFIG.md)**.

### 5. (Optional) Spin up dependencies with Docker Compose

```bash
docker compose up -d        # start Redis and other infrastructure
```

---

## Port topology (spelled out once, so you never have to guess)

| Port | Process | Protocol | Who connects | Exposure policy |
|---|---|---|---|---|
| **8002** | `trading_server_full` (C++ UI Server) | **WebSocket** | **The frontend** — login, order entry, live positions/orders/equity all go through this | In prod, `bind 127.0.0.1`; reverse-proxied by Nginx as `wss://your-domain/ws` |
| **8003** | `stats_api.py` (Python stats API) | HTTP | The frontend "Stats" page | **Local only.** In dev, Vite proxies `/stats-api`; in prod, Nginx adds a `location /stats-api/` block |
| **3000** | Vite dev server | HTTP | Your browser (dev only) | Dev only; in prod serve the `npm run build` output via Nginx static hosting |
| 6379 | Redis | — | Engine / scripts | **Never public.** `bind 127.0.0.1` + `requirepass` |

**The key relationship:** the frontend's **primary live channel is the WebSocket on 8002** (even login goes through it). 8003 is only a **read-only stats side-channel**, and it listens locally. Don't conflate them — 8002 is the main artery; 8003 is a health report. In dev, Vite proxies `/ws → :8002` and `/stats-api → :8003` to the same origin; in prod, Nginx does the same plus TLS termination.

---

## Security notes (read before you trade real money)

- **Secrets go in environment variables / `.env`.** The repo ships only `.env.example` and `*.example.json` templates; real files are blocked by `.gitignore`. See **[CONFIG.md](./CONFIG.md)**.
- **Exchange API keys:** grant *futures trading only*, **bind an IP allowlist** to your server, and **disable withdrawals**. Even if a key leaks, no one can move your funds out.
- **Start small:** run with `*_TESTNET=1` first, then validate on mainnet with money you can lose, and only scale up once behavior is confirmed.
- **Keep infrastructure off the public internet:** Redis / ClickHouse `bind 127.0.0.1` + strong password + firewall; 8002 also binds locally and is only exposed via Nginx + TLS.
- **Suspected leak → revoke immediately:** rotate and reset on the exchange / mailbox / Lark side without hesitation.
- The full production hardening checklist is in **[improve/04_deployment_guide.md](./improve/04_deployment_guide.md)**; C++ performance tuning is in **[improve/02_performance_cpp.md](./improve/02_performance_cpp.md)**.

---

## Known limitations / roadmap (the honest section)

I'm not trying to farm stars under false pretenses, so the weak spots are out in the open:

- **No test suite, no published benchmark.** I don't claim "fastest HFT" — the engine is event-driven with its hot path in C++, but there's no reproducible latency benchmark. Don't treat it as a low-latency textbook; treat it as a full-stack skeleton.
- **Only two exchanges:** Binance + OKX (perpetuals). Adding another means writing an adapter yourself under `adapters/`.
- **Solo project.** Maturity, docs coverage, and edge-case handling can't match a large-community project like freqtrade or nautilus. "Readable and hackable" is the selling point; "production out of the box" is not.
- **Alerts run via `subprocess`,** shelling out to external scripts for email/Lark rather than a persistent message queue — works fine, but not elegant under heavy load.
- **Roadmap (no committed timeline):** add unit/integration tests; publish a reproducible latency benchmark; abstract a cleaner adapter interface to support more exchanges; migrate alerts from subprocess to a persistent worker.

Issues and PRs are welcome — just understand this is a one-person project maintained in spare time.

---

## ⚠️ Disclaimer (please read in full)

> **Real-money risk.** This framework trades crypto derivatives with real funds. Crypto perpetual futures are **leveraged, can be liquidated, and can wipe out your principal — or more — in a very short time.**
>
> **Not investment advice.** Nothing in this repository constitutes investment, financial, or trading advice. It does not recommend any coin or asset and does not manage money on anyone's behalf. Any parameter, symbol, or example strategy in the code is a **technical demonstration**, not a trading signal.
>
> **No promise of returns.** This framework **does not guarantee profit.** Any past performance (if any) is not indicative of future results. The repo contains **no profitable alpha, no ML models, and no real keys** — the edge is yours to bring.
>
> **Provided "AS IS" under the MIT License, without warranty of any kind**, express or implied (including, without limitation, merchantability, fitness for a particular purpose, and uninterrupted or error-free operation). If you trade live with it, **all profits, losses, and risks are entirely your own**, and the author is not liable for any direct or indirect loss.
>
> Before you use real money: read the code, run the testnet, and start with an amount you can afford to lose.

---

## License

[MIT](./LICENSE) © paidaxing1234

> This is the public, shared version of the framework's core. **All proprietary strategies (alpha), ML models, real keys, and personal trading data have been removed.** It's the plumbing; the edge is yours to bring.
