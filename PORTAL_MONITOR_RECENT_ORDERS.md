# 接入指导：最近订单

> 配套 `PORTAL_MONITOR_DETAIL_AND_LOGS.md`（复用其中的 `trading_ws_call`）。

## 0. 数据来源（重要）

- **主站**的订单来自实时 `snapshot` WS 推送（`orderStore` 监听 `snapshot` 事件）。但实测 **snapshot 默认并不主动推**（连上后 8 次轮询收不到），不可靠；`orderApi.getOrders` 是空占位。
- **推荐 portal 用 `get_recent_orders`**（请求/响应 action，**从今日策略日志解析 `[ORDER:]` 行**），HTTP 可轮询、稳定，比 snapshot 更省心。
- **实测返回**（每条订单字段）：

```json
{ "timestamp":"2026-06-03 08:00:46.567", "strategy":"hyfcombo_mastercombo",
  "symbol":"USELESSUSDT", "side":"", "status":"ACCEPTED",
  "order_id":"py44484643924", "quantity":"", "detail":"exchange_id=1008879290 symbol=USELESSUSDT", "level":"INFO" }
```
- `side`/`quantity` 视日志行可能为空 → 显示 `-`；`status` 是主信息（ACCEPTED/FILLED/CANCELLED/REJECTED/…）。
- **只含今日**订单（"最近"语义）；今日无单则返回空。
- `strategy` 形如 `hyfcombo_mastercombo`（含账户前缀），归属过滤用它做子串匹配。

---

## 1. 后端端点（带归属过滤）

```python
# routers/monitor_orders.py
from fastapi import APIRouter, Depends
from core.permissions import require_module
from services.trading_ws import trading_ws_call          # 见 detail&logs 指导 §1

router = APIRouter(prefix="/monitor/api")

def owned_keys(user):
    """该用户可见的 account_id ∪ strategy_id 集合；管理员返回 None(不过滤)。"""
    if user.is_admin: return None
    return owned_account_ids(user) | owned_strategy_ids(user)   # 例: {acct2, mastercombo, ...}

@router.get("/recent_orders", dependencies=[Depends(require_module("monitor"))])
async def recent_orders(limit: int = 30, user=Depends(current_user)):
    orders = await trading_ws_call("get_recent_orders", {"limit": limit}) or []
    keys = owned_keys(user)
    if keys is not None:                                  # 按归属过滤(order.strategy 含账户/策略名)
        orders = [o for o in orders if any(k in (o.get("strategy") or "") for k in keys)]
    return {"orders": orders}
```

> 仪表板"最近订单 Top5" 用 `limit=5`；订单页可用 `limit=50`。

---

## 2. 前端：表格 + 轮询（vanilla JS）

```html
<table id="orders-table">
  <thead><tr><th>时间</th><th>策略</th><th>交易对</th><th>方向</th><th>状态</th><th>订单ID</th></tr></thead>
  <tbody></tbody>
</table>

<script>
const ORDER_STATUS = {                                   // 状态 -> [中文, 样式类]
  CREATED:['已创建','st-info'], SUBMITTED:['已提交','st-info'], ACCEPTED:['已接受','st-info'],
  PARTIALLY_FILLED:['部分成交','st-warn'], FILLED:['完全成交','st-ok'],
  CANCELLED:['已取消','st-mute'], REJECTED:['已拒绝','st-bad'], EXPIRED:['已过期','st-mute'],
};
const sideCls = s => s==='BUY'||s==='buy' ? 'pos' : (s==='SELL'||s==='sell' ? 'neg' : '');
const esc = s => String(s||'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));

async function loadOrders(){
  const r = await api('/monitor/api/recent_orders?limit=30');
  const rows = r.orders || [];
  const tb = document.querySelector('#orders-table tbody');
  tb.innerHTML = rows.length ? rows.map(o=>{
    const [txt,cls] = ORDER_STATUS[o.status] || [o.status||'-','st-info'];
    return `<tr>
      <td>${esc(o.timestamp)}</td>
      <td>${esc(o.strategy)}</td>
      <td>${esc(o.symbol)||'-'}</td>
      <td class="${sideCls(o.side)}">${esc(o.side)||'-'}</td>
      <td><span class="badge ${cls}">${txt}</span></td>
      <td title="${esc(o.detail)}">${esc(o.order_id)||'-'}</td>
    </tr>`;
  }).join('') : '<tr><td colspan="6" style="text-align:center;color:#909399">今日暂无订单</td></tr>';
}
setInterval(loadOrders, 8000);     // 8s 轮询（日志解析，不必更快）
loadOrders();
</script>
```
CSS：
```css
.badge{padding:2px 8px;border-radius:10px;font-size:12px}
.st-ok{background:rgba(103,194,58,.15);color:#67c23a}
.st-bad{background:rgba(245,108,108,.15);color:#f56c6c}
.st-warn{background:rgba(230,162,60,.15);color:#e6a23c}
.st-info{background:rgba(64,158,255,.15);color:#409eff}
.st-mute{background:rgba(144,147,153,.15);color:#909399}
#orders-table td.pos{color:#67c23a} #orders-table td.neg{color:#f56c6c}
```

---

## 3. 仪表板"最近订单 Top5"

同上，`api('/monitor/api/recent_orders?limit=5')`，去掉轮询或保留 8s，渲染到仪表板卡片即可。

---

## 4. 验收
- [ ] 表格显示今日订单：时间/策略/交易对/方向/状态/订单ID。
- [ ] 状态着色：FILLED 绿、REJECTED 红、PARTIALLY_FILLED 橙、ACCEPTED/SUBMITTED 蓝、CANCELLED/EXPIRED 灰。
- [ ] 归属：策略所有人只看到自己策略的订单（`order.strategy` 子串匹配 owned）；管理员看全部。
- [ ] 今日无单时显示"今日暂无订单"，不报错。
- [ ] 8s 轮询自动更新；新成交在最长 8s 内出现。

> 说明：当前 `[ORDER:]` 行里 `side`/`quantity` 常为空（取决于策略日志写法），属正常，显示 `-`。若以后要更全的字段（成交价/数量/已成交量），需让策略在下单日志里补 `side=/qty=` 等字段，`get_recent_orders` 的解析器（C++ `extract_val`）会自动带出。
