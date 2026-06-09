# 前端"盈亏/收益率/成交笔数/净值曲线"为空 —— stats_api 排障

> 现象：前端策略管理页的 **盈亏(USDT) / 收益率 / 成交笔数** 三列空白；
> 或账户页"钱包余额/可用/收益率"空；或策略详情页净值曲线/指标加载不出。
> 首次出现：2026-06-04（Redis 重建后 stats_api 僵尸进程导致）。

---

## 0. 数据链路（先理解，才好定位）

```
前端策略列表 = list_strategies(C++ WS)  ⨝(按 account_id)  stats_api /api/accounts_overview
前端净值曲线 = stats_api /api/equity_curve / account_stats
                       │
            stats_api(:8003, 只绑 127.0.0.1)  读  Redis: equity_latest / equity_history / slippage_history / trade_count
                       │
            前端经 同源路径 /stats-api 反代到 :8003 (vite dev / 生产 nginx)
```
任一环断 → 三列/曲线空。**最常见是 stats_api 那一环**。

---

## 1. 三步定位（从下往上）

```bash
# ① Redis 里有没有数据(equity_recorder cron 每5分钟写; 应有 pnl/return_rate/trade_count)
redis-cli HGETALL equity_latest:binance:acct3

# ② stats_api 是否真在监听端口(关键! 进程"在"不等于端口"绑")
ss -ltn | grep 8003 || echo "8003 没人监听!"
curl -s -o /dev/null -w "health HTTP %{http_code}\n" -m4 http://127.0.0.1:8003/api/health   # 应 200
curl -s -m5 http://127.0.0.1:8003/api/accounts_overview | python3 -m json.tool | head

# ③ 前端反代是否通(生产 nginx 需有 /stats-api location → 127.0.0.1:8003)
```

判读：
- ① 空 → 是 Redis 数据问题（见 `REDIS_数据恢复操作指南.md`，或 equity_recorder cron 没跑）。
- ① 有、② HTTP 000 / 端口没绑 → **stats_api 没真服务**（最常见，见 §2）。
- ①② 都正常、前端仍空 → 前端反代/缓存问题（生产确认 nginx `/stats-api`；浏览器硬刷新 Ctrl+Shift+R）。

---

## 2. 最常见根因：stats_api 僵尸进程占名额

**坑**：旧 stats_api 进程"僵"在（进程还在、但没绑 8003）。原看门狗 cron 用
`pgrep -f stats_api.py` 判活，看到僵尸进程就以为"活着"不重启 → 真服务永远起不来 → 前端三列空。

### 手动修复
```bash
pkill -9 -f "python3 stats_api.py"; sleep 2
ss -ltn | grep 8003 || echo "端口已释放"
cd /path/to/Real-account-trading-framework/cpp/scripts
setsid python3 stats_api.py >> /var/log/stats_api.log 2>&1 < /dev/null &
sleep 3
curl -s -o /dev/null -w "health HTTP %{http_code}\n" http://127.0.0.1:8003/api/health   # 应 200
```

### 已堵住（看门狗 = 独立脚本探活端口）
当前 cron（无需再改）：
```
*/5 * * * * /path/to/Real-account-trading-framework/cpp/scripts/stats_api_watchdog.sh >/dev/null 2>&1
```
脚本逻辑：curl health → 不通才 `pkill -f '[s]tats_api\.py'` + setsid 拉新 + 复检。

> ⚠️ 两个都踩过的坑：
> 1. **看进程名判活**：僵尸进程占名额 → 改探活端口；
> 2. **pkill 写在 crontab 行内会"自杀"**：`pkill -f 'python3 stats_api.py'` 匹配到 cron 自身
>    sh -c 的命令行(里面含这串文字)，把父 shell 杀掉，后面的启动永远跑不到 →
>    必须放独立脚本 + `[s]tats` 正则技巧（2026-06-06 实发）。

---

## 3. 修复后
- 后端立刻吐数据，**前端硬刷新 Ctrl+Shift+R** 即恢复（浏览器可能缓存了空结果）。
- 核对三列应有值：
```bash
curl -s http://127.0.0.1:8003/api/accounts_overview | python3 -c "import sys,json;[print(a['account_id'],a.get('pnl'),a.get('return_rate'),a.get('trade_count')) for a in json.load(sys.stdin)['accounts']]"
```

## 备注
- stats_api 只绑 `127.0.0.1:8003`（无鉴权，财务数据不对公网开），靠同源反代给前端。
- equity_latest/history 由 `equity_recorder.py` cron（每5分钟）写；它若没跑，①就会空，去看 `crontab -l`。

---

## 4. 策略详情页：净值曲线 / 夏普指标 为空或只剩一小段

**现象**：详情页净值曲线断了、只有今天一小段，或夏普/回撤/年化空白。
**根因**：曲线数据源 `equity_history:binance:{aid}` 的**深历史被清了**（Redis 重建/清空时一起没了）。
`equity_recorder` cron 只从清空那刻起每5分钟写新点，**几十天的历史不会自动回来**——要用回填脚本重建。

### 修复：跑 equity_backfill 重建净值深历史
```bash
cd /path/to/Real-account-trading-framework/cpp/scripts
python3 equity_backfill.py        # income小时级重建 + 日快照校准, 自动回到各账户开仓起
# 核对(每账户应有几十~上千点, 最早回到开仓月份):
for aid in acct2 acct3 acct1; do
  echo "$aid: $(redis-cli ZCARD equity_history:binance:$aid) 点"
done
```

### ⚠️ 关键坑：Binance 限频 429
`equity_backfill` 的 income 重建很重（成交多的账户如 acct1 要翻很多页，income 端点 weight=30/次）。
若同时有 **`fast_kline_filler --loop`** 或别的进程在狂打 Binance，会撞 `2400次/分` 限频 → income 失败、
退回浅的日快照（曲线只剩 ~30 天）。日志会显示 `income HTTP 429 ... 退回每日快照`。

**对策**：
1. 确认 `trading_server_full` 在跑（它实时写K线）→ **停掉冗余的 `fast_kline_filler`**：
   `pkill -9 -f fast_kline_filler`（C++ 已接管实时K线，filler 多余且抢限频）。
2. 等 ~70s 限频窗口重置，再 `python3 equity_backfill.py`（幂等，会重试 income）。
3. 别让多个进程同时狂拉 Binance（vision/filler/backfill 错开跑）。

### 滑点图（详情页底部"滑点冲击成本"）
`slippage_history:binance:{aid}` **无法回填**——它记的是"每次调仓相对决策价的冲击成本"，决策价不存历史。
清空后只能**等策略下次调仓**自动写新点，滑点图随之恢复。这是正常的，不用处理。

> equity_backfill 已有 cron（每日 00:10 UTC，CST 08:10）持续维护，income 失败会自动退日快照兜底。
