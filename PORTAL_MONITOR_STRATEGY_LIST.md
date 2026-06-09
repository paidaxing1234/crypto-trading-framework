# 接入指导：策略管理列表补「盈亏 / 收益率 / 成交笔数 / 运行时长」

> 配套 `PORTAL_INTEGRATION_PLAN.md` 与 `PORTAL_MONITOR_DETAIL_AND_LOGS.md`（复用其中的 `trading_ws_call`）。
> 目标：portal 策略列表对齐主站——每行显示**盈亏、收益率、成交笔数**，**运行时长从"首条日志日期"算起**（对重启/暂停免疫，不用进程 `start_time`）。

---

## 0. 数据来源

| 字段 | 来源 | 说明 |
|---|---|---|
| 盈亏 `pnl` | stats `accounts_overview` | = 净值 − 本金 |
| 收益率 `return_rate` | stats `accounts_overview` | **小数**(0.04=4%)，前端 ×100 显示 |
| 成交笔数 `trade_count` | stats `accounts_overview` | 累计 |
| 运行时长 | `get_strategy_log_files` 解析 | 取最早 `_YYYYMMDD.log`（**排除 dryrun/backtest**），无则回退 `start_time` |

策略 = 账户 **1:1**，按 `account_id` join overview。归属过滤后只含该用户自己的策略/账户。

---

## 1. 后端：一个聚合端点 `GET /monitor/api/strategies`

一次返回"列表 + 三项统计 + 首日志时间"，前端只管渲染。（若你已有策略列表端点，把下面的 join/first_log 逻辑并进去即可。）

```python
# routers/monitor_strategies.py
import re, asyncio, httpx
from datetime import datetime, timezone
from fastapi import APIRouter, Depends
from core.permissions import require_module
from services.trading_ws import trading_ws_call          # 见 detail&logs 指导 §1
# owns(user, sid) / owned_account_ids(user): 管理员=全部, 否则查归属表

router = APIRouter(prefix="/monitor/api")
TRADING_STATS = "https://trading.your-domain.com/stats-api"
_http = httpx.AsyncClient(timeout=12.0)

_DATE_RE = re.compile(r'(\d{8})\.log$')
_first_log_cache: dict = {}      # strategy_id -> ms | None（首日志日期不变，可长期缓存）

async def first_log_ts(strategy_id: str):
    if strategy_id in _first_log_cache:
        return _first_log_cache[strategy_id]
    try:
        files = await trading_ws_call("get_strategy_log_files", {"strategyId": strategy_id}) or []
    except Exception:
        return None
    best = None
    for f in files:
        fn = f.get("filename", "")
        if re.search(r'dryrun|backtest', fn, re.I):        # 排除试运行/回测
            continue
        m = _DATE_RE.search(fn)
        if not m:
            continue
        d = m.group(1)
        ts = int(datetime(int(d[:4]), int(d[4:6]), int(d[6:8]),
                          tzinfo=timezone.utc).timestamp() * 1000)
        if best is None or ts < best:
            best = ts
    _first_log_cache[strategy_id] = best
    return best

async def _overview_map(user):
    r = await _http.get(f"{TRADING_STATS}/api/accounts_overview")
    accts = r.json().get("accounts", [])
    owned = owned_account_ids(user)                        # None=管理员
    if owned is not None:
        accts = [a for a in accts if a["account_id"] in owned]
    return {a["account_id"]: a for a in accts}

@router.get("/strategies", dependencies=[Depends(require_module("monitor"))])
async def strategies(user=Depends(current_user)):
    strats = await trading_ws_call("list_strategies", {}) or []
    strats = [s for s in strats if owns(user, s.get("strategy_id"))]      # 归属过滤
    ov = await _overview_map(user)
    logs = await asyncio.gather(*[first_log_ts(s.get("strategy_id", "")) for s in strats])
    out = []
    for s, flt in zip(strats, logs):
        a = ov.get(s.get("account_id"), {})
        out.append({
            "strategy_id": s.get("strategy_id"), "account_id": s.get("account_id"),
            "exchange": s.get("exchange"), "status": s.get("status"),
            "start_time": s.get("start_time"),
            "pnl": a.get("pnl"), "return_rate": a.get("return_rate"),
            "trade_count": a.get("trade_count"),
            "first_log_ts": flt,                            # ms 或 null
        })
    return {"strategies": out}
```

要点：
- **`first_log_ts` 有缓存**（首日志日期是不变量）；进程重启自然刷新；删后重建同名策略会命中旧缓存（极少见，需要时清 `_first_log_cache`）。
- `list_strategies` 的 payload 在 `resp.data.data`（`trading_ws_call` 已剥到这层，返回数组）。
- `accounts_overview` 走 **httpx 服务端取**（不是浏览器），便于 join + 归属过滤。

---

## 2. 前端：渲染 + 运行时长实时跳动（vanilla JS）

```html
<table id="strat-table">
  <thead><tr>
    <th>策略</th><th>账户</th><th>状态</th>
    <th>盈亏(USDT)</th><th>收益率</th><th>成交笔数</th><th>运行时长</th><th>操作</th>
  </tr></thead>
  <tbody></tbody>
</table>

<script>
const fmtN = (v,d=2)=> (v==null||isNaN(v))?'-':Number(v).toFixed(d);
const fmtP = (v)=> (v==null||isNaN(v))?'-':(v*100).toFixed(2)+'%';
const cls  = (v)=> v==null?'':(v>=0?'pos':'neg');
function fmtDur(ms){
  if(ms==null||ms<0||isNaN(ms)) return '-';
  const s=Math.floor(ms/1000), d=Math.floor(s/86400), h=Math.floor(s%86400/3600), m=Math.floor(s%3600/60);
  if(d>0) return d+'天'+h+'小时';
  if(h>0) return h+'小时'+m+'分';
  if(m>0) return m+'分钟';
  return s+'秒';
}
function parseTs(v){ if(v==null||v===0)return null; if(typeof v==='number')return v<1e12?v*1000:v; const t=Date.parse(v); return isNaN(t)?null:t; }

let _rows = [];
async function loadStrategies(){
  const r = await api('/monitor/api/strategies');
  _rows = r.strategies || [];
  document.querySelector('#strat-table tbody').innerHTML = _rows.map(s=>`
    <tr data-sid="${s.strategy_id}">
      <td>${s.strategy_id}</td>
      <td>${s.account_id||'-'}</td>
      <td>${s.status}</td>
      <td class="${cls(s.pnl)}">${fmtN(s.pnl)}</td>
      <td class="${cls(s.return_rate)}">${fmtP(s.return_rate)}</td>
      <td>${s.trade_count ?? '-'}</td>
      <td class="runtime"></td>
      <td><a href="/monitor/strategy/${s.strategy_id}?account_id=${s.account_id}&name=${s.strategy_id}">详情</a></td>
    </tr>`).join('');
  tickRuntime();
}
function tickRuntime(){
  const now = Date.now();
  document.querySelectorAll('#strat-table tbody tr').forEach(tr=>{
    const s = _rows.find(x=>x.strategy_id===tr.dataset.sid); if(!s) return;
    const start = (s.first_log_ts!=null) ? s.first_log_ts : parseTs(s.start_time);  // 优先首日志
    tr.querySelector('.runtime').textContent =
      (s.status!=='running' || !start) ? '-' : fmtDur(now - start);
  });
}
setInterval(tickRuntime, 1000);        // 运行时长实时跳动（日级精度，用 30s 也够）
setInterval(loadStrategies, 10000);    // 列表 10s 刷新（同主站）
loadStrategies();
</script>
```
CSS：`#strat-table td.pos{color:#67c23a}` `#strat-table td.neg{color:#f56c6c}`

---

## 3. 关键点 / 易错点

- **运行时长用 `first_log_ts`，不是 `start_time`** —— 这正是"不太对"的修法：`start_time` 每次重启/debug 都归零，`first_log_ts` 取日志最早日期，对重启免疫。
- **排除 `dryrun`/`backtest`** 文件，否则试运行日志会把运行时长算早。
- `return_rate` 是**小数**，记得 ×100；`pnl` 正负着色。
- `first_log_ts` / `start_time` 都是 **ms 时间戳**；`first_log_ts` 为 null（无日志）时回退 `start_time`，再 null 显示 `-`。
- 仅 `status==='running'` 显示运行时长，其余 `-`（与主站一致）。
- 这些统计来自 `equity_recorder`（每 5 分钟写一次），所以 pnl/收益率/成交是**准实时**（最多滞后 5 分钟），列表 10s 刷新即可。

---

## 4. 验收（含期望值，可直接对照自测）

当前三个实盘策略，照本指导实现后应显示（运行时长随日增长）：

| 策略 | 账户 | 运行时长(首日志) | 盈亏/收益率/成交 来源 |
|---|---|---|---|
| `mastercombo` | acct2 | 从 2026-05-27 起（≈7+天） | accounts_overview[acct2] |
| `apollo_fund` | acct3 | 从 2026-04-27 起（≈37+天） | accounts_overview[acct3] |
| `five_mom_factor_binance_testnet` | acct1 | 从 2026-04-26 起（≈38+天） | accounts_overview[acct1] |

- [ ] 三列（盈亏/收益率/成交笔数）有值且正负着色正确。
- [ ] 运行时长与上表一致，且每秒/每刷新在涨；重启 trading 服务后**不归零**。
- [ ] 归属：策略所有人只见自己的策略；管理员见全部。
