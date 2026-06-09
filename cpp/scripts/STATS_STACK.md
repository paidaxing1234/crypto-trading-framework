# 账户/策略统计栈 (净值曲线 / 夏普 / 收益率 / 成交笔数)

为前端"策略详情 + 账户余额"面板供数。 **完全独立于 C++ 交易主进程**，可随时重启，零实盘风险。

## 组成

| 组件 | 文件 | 作用 |
|---|---|---|
| 净值记录器 | `equity_recorder.py` | 每 5 分钟读 `/fapi/v2/account` + `/fapi/v1/income`，写 Redis 时序 |
| 统计 API | `stats_api.py` | 只读 Redis，算夏普/回撤/年化，HTTP 暴露给前端 (127.0.0.1:8003) |
| 起始本金 | `../strategies/configs/initial_capital.json` | `{account_id: 本金}`，算收益率用 |

## Redis key

```
equity_history:binance:{aid}   ZSET  score=ts_ms  value=JSON{ts,equity,available,wallet,upnl}
equity_latest:binance:{aid}    HASH  最新快照 + initial_capital/return_rate/pnl/trade_count
trade_count:binance:{aid}      累计成交笔数
equity_recorder:income_cursor:binance:{aid}   成交笔数增量游标
```

## cron (已装)

```
*/5 * * * * /usr/bin/python3 .../equity_recorder.py >> /var/log/equity_recorder.log 2>&1
*/5 * * * * pgrep -f stats_api.py >/dev/null || (cd .../scripts && nohup python3 stats_api.py >> /var/log/stats_api.log 2>&1 &)
```

## HTTP 接口

```
GET /api/health
GET /api/accounts_overview                  所有账户最新统计
GET /api/account_stats?account_id=X         单账户最新 + 全历史指标
GET /api/equity_curve?account_id=X&range=30d  曲线点 + 区间指标  (range: 7d|30d|90d|1y|all)
```

## 前端接入

- 前端走**同源路径 `/stats-api`**(避免 CORS / https 混合内容)。
- 开发：`vite.config.js` 已加代理 `/stats-api` → `http://127.0.0.1:8003`。
- **生产：nginx 需加一段**(和 `/ws` 同级)：

```nginx
location /stats-api/ {
    proxy_pass http://127.0.0.1:8003/;   # 末尾的 / 会把 /stats-api 前缀剥掉
}
```

## 安全

- `stats_api.py` 只绑 `127.0.0.1`(无鉴权，暴露账户财务)，仅同机反代可达，**勿对公网开 8003 端口**。
- 生产如需更严，可在上面的 nginx `location` 里加 `auth_basic` 或 token 校验。
