# 接入指导：/monitor 的「策略详情」+「策略日志」（portal 原生 vanilla JS）

> 配套主方案 `PORTAL_INTEGRATION_PLAN.md`。本文只讲两个当前缺失的页面，给**可直接落地的 FastAPI + 模板 + vanilla JS + ECharts** 代码。
> 这两个功能都是**请求/响应型，走 HTTP 即可**，不依赖实时 WS 推送。
> 若你走的是 Vue SPA 子应用：主站已有 `views/StrategyDetail.vue` / `views/StrategyLogs.vue`，直接挂载即可，可跳过本文。以下针对 **portal 原生 vanilla JS**。

---

## 0. 响应封装（实测，务必照此解析）

trading 的 WS action 响应统一封装为：
```jsonc
{ "type": "response", "timestamp": 1780464878847,
  "data": {
     "success": true, "message": "", "requestId": "r2",
     "type": "strategy_logs",            // = action 对应的类型
     "data": <真正的 payload>            // ← 列表/对象都在这一层
  }
}
```
即：**payload 在 `resp.data.data`；success 在 `resp.data.success`；requestId 在 `resp.data.requestId`**。

---

## 1. 后端公共件：调用 trading WS action

策略日志走 WS action（`get_strategy_log_files` / `get_strategy_logs`）。在 FastAPI 里封一个工具，用服务账号 `portal_svc` 登录上游、发一个 action、拿回 payload。短连接版（最简、够用；高频可改持久连接池）：

```python
# services/trading_ws.py
import json, asyncio, itertools, websockets
from core.config import settings          # PORTAL_SVC_USER / PORTAL_SVC_PASS

TRADING_WS = "wss://trading.your-domain.com/ws"
_rid = itertools.count(1)

def _login_ok(m):
    if m.get("type") == "login_response":
        return m.get("success", True)
    d = m.get("data", {})
    return d.get("type") == "login_response" or "token" in d or "token" in m

async def trading_ws_call(action: str, data: dict, timeout: float = 15.0):
    """以 portal_svc 调一个 request/response action，返回 payload(resp.data.data)。失败抛异常。"""
    rid = f"portal_{next(_rid)}"
    async with websockets.connect(TRADING_WS, max_size=None, open_timeout=timeout) as ws:
        await ws.send(json.dumps({"type": "login",
            "username": settings.PORTAL_SVC_USER, "password": settings.PORTAL_SVC_PASS}))
        sent = False
        loop = asyncio.get_event_loop(); end = loop.time() + timeout
        while loop.time() < end:
            raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, end - loop.time()))
            m = json.loads(raw)
            if not sent and _login_ok(m):                       # 登录确认后再发 action
                await ws.send(json.dumps({"action": action, "data": {**data, "requestId": rid}}))
                sent = True
                continue
            if m.get("type") == "response" and m.get("data", {}).get("requestId") == rid:
                body = m["data"]
                if not body.get("success", True):
                    raise RuntimeError(body.get("message", "trading action failed"))
                return body.get("data")                          # ← 真正的 payload
        raise TimeoutError(f"trading_ws_call timeout: {action}")
```

> 优化（可选）：单 worker 下维护**一条常驻上游连接** + `requestId -> asyncio.Future` 映射，省去每次握手/登录。接口签名不变。

---

## 2. 策略日志

### 2.1 后端两个端点（带归属校验）
```python
# routers/monitor_logs.py
from fastapi import APIRouter, Depends, HTTPException
from core.permissions import require_module
from services.trading_ws import trading_ws_call
# assert_owns(user, strategy_id): 管理员放行；否则查归属表，无权 -> 403

router = APIRouter(prefix="/monitor/api")

@router.get("/strategy_log_files", dependencies=[Depends(require_module("monitor"))])
async def strategy_log_files(strategy_id: str, user=Depends(current_user)):
    assert_owns(user, strategy_id)
    files = await trading_ws_call("get_strategy_log_files", {"strategyId": strategy_id})  # [{filename,size}]
    return {"files": files or []}

@router.get("/strategy_logs", dependencies=[Depends(require_module("monitor"))])
async def strategy_logs(strategy_id: str, filename: str, tail: int = 200, user=Depends(current_user)):
    assert_owns(user, strategy_id)                               # 用 strategy_id 校验归属
    if strategy_id not in filename:                              # 防越权读他人文件
        raise HTTPException(403, "文件与策略不匹配")
    payload = await trading_ws_call("get_strategy_logs", {"filename": filename, "tailLines": tail})
    return {"filename": filename, "lines": (payload or {}).get("lines", [])}
```

### 2.2 前端（HTML + vanilla JS）
```html
<!-- templates/monitor/_strategy_logs.html  (可嵌详情页, 或独立日志页) -->
<div class="log-panel" data-strategy-id="{{ strategy_id }}">
  <div class="log-toolbar">
    <select class="log-file"></select>
    <select class="log-tail">
      <option value="100">最近 100 行</option>
      <option value="200" selected>最近 200 行</option>
      <option value="500">最近 500 行</option>
      <option value="1000">最近 1000 行</option>
    </select>
    <button class="log-refresh">刷新</button>
    <label><input type="checkbox" class="log-auto"> 自动刷新</label>
  </div>
  <pre class="log-view"></pre>
</div>

<script>
(function(){
  const root = document.currentScript.previousElementSibling;
  const sid  = root.dataset.strategyId;
  const $file = root.querySelector('.log-file');
  const $tail = root.querySelector('.log-tail');
  const $view = root.querySelector('.log-view');
  let timer = null;

  function lineClass(l){
    if (/\[ERROR\]|ERROR/.test(l)) return 'log-error';
    if (/\[WARN\]|WARNING/.test(l)) return 'log-warn';
    if (/\[DEBUG\]/.test(l)) return 'log-debug';
    return '';
  }
  async function loadFiles(){
    const r = await api(`/monitor/api/strategy_log_files?strategy_id=${encodeURIComponent(sid)}`);
    // 优先按文件名里的日期倒序，排除 start_*/无日期的放后面
    const files = (r.files||[]).slice().sort((a,b)=> b.filename.localeCompare(a.filename));
    $file.innerHTML = files.map(f=>`<option value="${f.filename}">${f.filename} (${(f.size/1024).toFixed(1)}KB)</option>`).join('');
    if (files.length) loadLogs();
  }
  async function loadLogs(){
    const fn = $file.value; if(!fn) return;
    const r = await api(`/monitor/api/strategy_logs?strategy_id=${encodeURIComponent(sid)}&filename=${encodeURIComponent(fn)}&tail=${$tail.value}`);
    $view.innerHTML = (r.lines||[]).map(l=>`<code class="${lineClass(l)}">${escapeHtml(l)}</code>`).join('\n');
    $view.scrollTop = $view.scrollHeight;
  }
  function escapeHtml(s){return s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}

  $file.onchange = loadLogs;
  $tail.onchange = loadLogs;
  root.querySelector('.log-refresh').onclick = loadLogs;
  root.querySelector('.log-auto').onchange = e => {
    clearInterval(timer); timer = null;
    if (e.target.checked) timer = setInterval(loadLogs, 3000);
  };
  loadFiles();
})();
</script>
```
CSS（与主站一致）：`.log-view{font-family:monospace;white-space:pre-wrap;height:60vh;overflow:auto;background:#0d1117;color:#c9d1d9;padding:16px;border-radius:8px}` `.log-error{color:#f56c6c}` `.log-warn{color:#e6a23c}` `.log-debug{color:#8b949e;opacity:.7}`

---

## 3. 策略详情（指标 + 净值曲线）

### 3.1 数据来源：stats 代理（主方案 §6.1 已建，无需新后端）
- `GET /monitor/stats-api/api/account_stats?account_id=<acct>` → `{ latest:{equity,available,wallet,upnl,pnl,return_rate,trade_count}, initial_capital }`
- `GET /monitor/stats-api/api/equity_curve?account_id=<acct>&range=7d|30d|90d|1y|all` → `{ points:[{ts,equity,upnl}], metrics:{sharpe,max_drawdown,annualized_return,volatility,total_return,points,span_days}, initial_capital }`
- `account_id` 由策略行带过来（策略=账户 1:1）。stats 代理已做归属校验。

### 3.2 进入详情：在策略列表行加「详情」入口
两种做法，任选：
- **页面**（更贴 portal 风格）：`GET /monitor/strategy/{sid}` 渲染 `strategy_detail.html`，account_id 走 query（`?account_id=acct3`）或后端按归属表反查。
- **弹窗**：列表行点「详情」打开 modal，纯前端 fetch。

### 3.3 前端（HTML + vanilla JS + ECharts）
```html
<!-- templates/monitor/strategy_detail.html -->
<div class="detail" data-account="{{ account_id }}" data-name="{{ name }}">
  <div class="detail-head">
    <h2>{{ name }}</h2><span class="muted">账户 {{ account_id }}</span>
    <span class="ranges">
      <button data-r="7d">7天</button><button data-r="30d" class="on">30天</button>
      <button data-r="90d">90天</button><button data-r="1y">1年</button><button data-r="all">全部</button>
    </span>
  </div>
  <div class="metric-grid"></div>                 <!-- 指标卡 -->
  <div id="equityChart" style="height:380px"></div>
</div>

<script>
(function(){
  const root = document.currentScript.previousElementSibling;
  const acct = root.dataset.account;
  const chart = echarts.init(root.querySelector('#equityChart'));
  let range = '30d';

  const fmtN = (v,d=2)=> (v==null||isNaN(v))?'-':Number(v).toFixed(d);
  const fmtP = (v,d=2)=> (v==null||isNaN(v))?'-':(v*100).toFixed(d)+'%';
  const cls  = v=> v==null?'':(v>=0?'pos':'neg');

  function cards(latest, m, initCap){
    const C = [
      ['净值(USDT)', fmtN(latest.equity), ''],
      ['累计收益率', fmtP(latest.return_rate), cls(latest.return_rate)],
      ['累计盈亏', fmtN(latest.pnl), cls(latest.pnl)],
      ['可用余额', fmtN(latest.available), ''],
      ['钱包余额', fmtN(latest.wallet), ''],
      ['未实现盈亏', fmtN(latest.upnl), cls(latest.upnl)],
      ['成交笔数', latest.trade_count ?? '-', ''],
      ['夏普比率', m.sharpe==null?'-':Number(m.sharpe).toFixed(2), cls(m.sharpe)],
      ['最大回撤', fmtP(m.max_drawdown), 'neg'],
      ['年化收益', fmtP(m.annualized_return), cls(m.annualized_return)],
      ['年化波动率', fmtP(m.volatility), ''],
      ['本金(USDT)', fmtN(initCap), ''],
    ];
    root.querySelector('.metric-grid').innerHTML = C.map(([k,v,c])=>
      `<div class="metric-card"><div class="mk">${k}</div><div class="mv ${c}">${v}</div></div>`).join('');
  }

  function renderCurve(points, initCap){
    chart.setOption({
      tooltip:{trigger:'axis'},
      grid:{left:'3%',right:'4%',bottom:'3%',top:'8%',containLabel:true},
      xAxis:{type:'time'},
      yAxis:{type:'value',scale:true,axisLabel:{formatter:v=>v.toFixed(0)}},
      series:[{
        name:'净值', type:'line', smooth:true, showSymbol:false, sampling:'lttb',
        data: points.map(p=>[p.ts, p.equity]),
        itemStyle:{color:'#409eff'},
        areaStyle:{color:new echarts.graphic.LinearGradient(0,0,0,1,
          [{offset:0,color:'rgba(64,158,255,.28)'},{offset:1,color:'rgba(64,158,255,.03)'}])},
        markLine: initCap?{silent:true,symbol:'none',data:[{yAxis:initCap}],
          lineStyle:{type:'dashed',color:'#909399'},label:{formatter:'本金 '+initCap}}:undefined
      }]
    }, true);
  }

  async function loadStats(){
    const s = await api(`/monitor/stats-api/api/account_stats?account_id=${encodeURIComponent(acct)}`);
    root._latest = s.latest||{}; root._initCap = s.initial_capital;
    loadCurve();                                   // 曲线返回区间指标, 一起刷卡片
  }
  async function loadCurve(){
    const c = await api(`/monitor/stats-api/api/equity_curve?account_id=${encodeURIComponent(acct)}&range=${range}`);
    cards(root._latest||{}, c.metrics||{}, c.initial_capital ?? root._initCap);
    renderCurve(c.points||[], c.initial_capital ?? root._initCap);
  }

  root.querySelectorAll('.ranges button').forEach(b=> b.onclick = ()=>{
    root.querySelectorAll('.ranges button').forEach(x=>x.classList.remove('on'));
    b.classList.add('on'); range = b.dataset.r; loadCurve();
  });
  window.addEventListener('resize', ()=>chart.resize());
  loadStats();
})();
</script>
```
CSS：`.metric-grid{display:grid;grid-template-columns:repeat(6,1fr);gap:12px;margin:16px 0}` `.metric-card{border:1px solid #ebeef5;border-radius:8px;padding:14px}` `.mk{font-size:11px;color:#909399;text-transform:uppercase}` `.mv{font-size:22px;font-weight:800;font-family:monospace}` `.mv.pos{color:#67c23a}` `.mv.neg{color:#f56c6c}`

> ECharts：portal 的 `static/vendor` 里引入 `echarts.min.js`（与主站同为 5.x）。

### 3.4 详情入口（策略列表行）
在你已有的策略列表行操作列加：
```html
<a href="/monitor/strategy/{{ s.strategy_id }}?account_id={{ s.account_id }}&name={{ s.strategy_id }}">详情</a>
```
后端 `GET /monitor/strategy/{sid}` 渲染 `strategy_detail.html`，account_id 取 query（或按归属表反查，校验该用户拥有 sid）。

---

## 3.5 滑点冲击成本图(策略详情页新增模块)

数据源(走同一个 stats 代理,归属校验同 §3.1):
```
GET /monitor/stats-api/api/slippage_history?account_id=<acct>&range=7d|30d|90d|1y|all
→ { points:[ {ts, strategy_id, rebalance_no, trades, fee_usdt, cost_usdt, notional_usdt, wbps} ],
    totals:{ rebalances, cost_usdt, fee_usdt, notional_usdt, wavg_bps } }
```
- **每次调仓一个点**(由策略调仓收尾时写入 Redis `slippage_history:binance:{aid}`)。
- `cost_usdt` = 冲击成本(正=比决策价吃亏,负=优于决策价);`wbps` = 该次调仓加权滑点(基点)。
- 图表:ECharts **柱(cost_usdt,正红负绿) + 折线(wbps,右轴)**,x=调仓时间;卡片头显示 totals(N次调仓 | 成本合计 | 加权bp | 手续费)。
- vanilla JS 写法与 §3.3 净值曲线同构,option 关键差异:
```js
yAxis: [{type:'value',name:'USDT'},{type:'value',name:'bp',splitLine:{show:false}}],
series: [
  { name:'冲击成本(USDT)', type:'bar', data: pts.map(p=>p.cost_usdt),
    itemStyle:{ color: q => (pts[q.dataIndex]?.cost_usdt??0) >= 0 ? '#f56c6c' : '#67c23a' } },
  { name:'加权滑点(bp)', type:'line', yAxisIndex:1, smooth:true, data: pts.map(p=>p.wbps) }
]
```
- 空数据(策略尚未在新版下调仓过)显示"暂无滑点数据",不报错。

## 4. 字段映射速查

| 卡片 | 来源字段 | 来源接口 | 格式 |
|---|---|---|---|
| 净值 | latest.equity | account_stats | 数字2位 |
| 累计收益率 | latest.return_rate | account_stats | ×100% |
| 累计盈亏 | latest.pnl | account_stats | 数字2位 |
| 可用/钱包/未实现 | latest.available/wallet/upnl | account_stats | 数字2位 |
| 成交笔数 | latest.trade_count | account_stats | 整数 |
| 夏普 | metrics.sharpe | equity_curve(区间) | 2位 |
| 最大回撤 | metrics.max_drawdown | equity_curve(区间) | ×100% |
| 年化/波动率 | metrics.annualized_return / volatility | equity_curve(区间) | ×100% |
| 本金 | initial_capital | 两者皆有 | 数字2位 |
| 曲线点 | points[].ts / .equity | equity_curve | [ms, 净值] |

> 说明：`return_rate` 是**小数**（0.04=4%），前端 ×100 显示。`points[].ts` 是**毫秒**。30天与90天可能相同（Binance 仅留 ~30 天每日快照）；7天窗口夏普/年化偏高是短窗口统计假象，可在卡片加「窗口<14天仅供参考」提示。

---

## 5. 验收清单
- [ ] 策略列表行「详情」→ 进详情页，12 个指标卡有值、净值曲线出图、本金参考线在。
- [ ] 区间 7d/30d/90d/1y/all 切换 → 曲线与夏普/回撤/年化随区间变。
- [ ] 策略日志：文件下拉有列表、选中出内容、ERROR 红 / WARN 黄、行数切换、刷新、自动刷新(3s)。
- [ ] 归属校验：策略所有人只能看/拉自己策略的详情与日志；越权账户/文件被 403。
- [ ] 服务账号短连接调用稳定（高频再上常驻连接池）。

> 当前实盘三策略（供自测）：`mastercombo`→acct2、`apollo_fund`→acct3、`five_mom_factor_binance_testnet`→acct1。
