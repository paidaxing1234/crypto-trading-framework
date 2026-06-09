#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""账户/策略统计只读 API —— 给前端的净值曲线、夏普、收益率、最大回撤等面板供数。

设计取舍: 独立进程, 只读 Redis(equity_recorder 写的那些 key), 不碰 live trading_server,
所以可以随时重启而不影响实盘。 用 stdlib http.server, 零额外依赖(只用 numpy/redis)。

数据来源(均由 equity_recorder.py 写):
  equity_history:binance:{aid}   ZSET  score=ts_ms  value=JSON{ts,equity,available,wallet,upnl}
  equity_latest:binance:{aid}    HASH  最新快照 + initial_capital/return_rate/pnl/trade_count
  trade_count:binance:{aid}      累计成交笔数

接口(全部 GET, 返回 JSON, 已开 CORS):
  /api/health
  /api/accounts_overview                      所有账户最新统计(账户表/策略表 join 用)
  /api/account_stats?account_id=X             单账户最新 + 全历史指标
  /api/equity_curve?account_id=X&range=30d    净值曲线(点) + 指标; range=7d|30d|90d|1y|all

运行: nohup python3 stats_api.py >> /var/log/stats_api.log 2>&1 &   (默认端口 8003)
"""
from __future__ import annotations
import os, sys, json, time, glob, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import numpy as np
import redis

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ACCT_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "strategies", "acount_configs"))
INIT_CAP_FILE = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "strategies", "configs", "initial_capital.json"))
PORT = int(os.environ.get("STATS_API_PORT", 8003))
DAY_MS = 86400000
MAX_POINTS = 600          # 曲线最多返回点数, 超过则等距下采样

R = redis.Redis(host=os.environ.get("REDIS_HOST", "127.0.0.1"),
                port=int(os.environ.get("REDIS_PORT", 6379)),
                password=os.environ.get("REDIS_PASSWORD") or None,
                decode_responses=True)

RANGE_MS = {"1d": DAY_MS, "7d": 7*DAY_MS, "30d": 30*DAY_MS,
            "90d": 90*DAY_MS, "1y": 365*DAY_MS, "all": None}


# ---------- 数据读取 ----------
def _now_ms() -> int:
    return int(time.time() * 1000)


def load_initial_caps() -> dict:
    try:
        return json.load(open(INIT_CAP_FILE, encoding="utf-8"))
    except Exception:
        return {}


def list_accounts() -> list[dict]:
    """从 acount_configs 读账户列表(只取 binance)。"""
    out = []
    for fp in sorted(glob.glob(os.path.join(ACCT_DIR, "*.json"))):
        try:
            d = json.load(open(fp, encoding="utf-8"))
        except Exception:
            continue
        if d.get("exchange", "").lower() == "binance" and d.get("account_id"):
            out.append({"account_id": d["account_id"], "exchange": "binance"})
    return out


def get_latest(aid: str) -> dict:
    h = R.hgetall(f"equity_latest:binance:{aid}")
    out = {}
    for k, v in h.items():
        try:
            out[k] = float(v)
        except (ValueError, TypeError):
            out[k] = v
    return out


def get_history(aid: str, start_ms: int | None, end_ms: int | None) -> list[dict]:
    key = f"equity_history:binance:{aid}"
    lo = "-inf" if start_ms is None else start_ms
    hi = "+inf" if end_ms is None else end_ms
    rows = R.zrangebyscore(key, lo, hi)
    out = []
    for raw in rows:
        try:
            out.append(json.loads(raw))
        except Exception:
            continue
    return out


# ---------- 指标计算 ----------
def _daily_resample(ts: np.ndarray, eq: np.ndarray):
    """取每个自然日最后一个净值点, 返回 (daily_ts, daily_eq)。 ts 升序。"""
    day = ts // DAY_MS
    if len(day) == 0:
        return ts, eq
    last_idx = np.concatenate([np.where(np.diff(day) != 0)[0], [len(day) - 1]])
    return ts[last_idx], eq[last_idx]


def compute_metrics(history: list[dict], initial_capital: float | None) -> dict:
    """夏普/最大回撤/年化/波动率/总收益。 数据不足时优雅降级返回 None。"""
    n = len(history)
    base = {
        "points": n, "sharpe": None, "max_drawdown": None,
        "annualized_return": None, "volatility": None,
        "total_return": None, "total_pnl": None,
        "start_equity": None, "end_equity": None,
        "peak_equity": None, "span_days": None,
    }
    if n == 0:
        return base
    ts = np.array([h["ts"] for h in history], dtype=np.int64)
    eq = np.array([h["equity"] for h in history], dtype=float)
    base["start_equity"] = float(eq[0])
    base["end_equity"] = float(eq[-1])
    base["peak_equity"] = float(eq.max())
    span = (ts[-1] - ts[0]) / DAY_MS
    base["span_days"] = round(float(span), 3)

    # 最大回撤(用全分辨率序列)
    running_max = np.maximum.accumulate(eq)
    dd = eq / running_max - 1.0
    base["max_drawdown"] = float(dd.min())

    # 总收益: 优先相对配置本金, 否则相对首点
    ref = float(initial_capital) if initial_capital else float(eq[0])
    if ref > 0:
        base["total_return"] = float(eq[-1] / ref - 1.0)
        base["total_pnl"] = float(eq[-1] - ref)

    # 夏普/波动率: 用日重采样收益, 需 >=3 个日点才有意义
    d_ts, d_eq = _daily_resample(ts, eq)
    if len(d_eq) >= 3:
        d_ret = np.diff(d_eq) / d_eq[:-1]
        sd = d_ret.std(ddof=1)
        if sd > 0:
            base["sharpe"] = float(d_ret.mean() / sd * np.sqrt(365))
        base["volatility"] = float(sd * np.sqrt(365))
    # 年化: 几何, 需 span>~1天
    if span >= 1 and eq[0] > 0:
        base["annualized_return"] = float((eq[-1] / eq[0]) ** (365.0 / span) - 1.0)
    return base


def _downsample(history: list[dict], max_points: int) -> list[dict]:
    n = len(history)
    if n <= max_points:
        return history
    idx = np.linspace(0, n - 1, max_points).astype(int)
    idx = np.unique(idx)
    return [history[i] for i in idx]


# ---------- 业务组装 ----------
def overview() -> dict:
    caps = load_initial_caps()
    accts = []
    for a in list_accounts():
        aid = a["account_id"]
        latest = get_latest(aid)
        cap = caps.get(aid)
        equity = latest.get("equity")
        item = {
            "account_id": aid,
            "exchange": "binance",
            "equity": equity,
            "available": latest.get("available"),
            "wallet": latest.get("wallet"),
            "upnl": latest.get("upnl"),
            "initial_capital": float(cap) if cap else latest.get("initial_capital"),
            "pnl": latest.get("pnl"),
            "return_rate": latest.get("return_rate"),
            "trade_count": int(latest["trade_count"]) if "trade_count" in latest else None,
            "updated_at": int(latest["ts"]) if "ts" in latest else None,
        }
        accts.append(item)
    return {"accounts": accts, "server_time": _now_ms()}


def account_stats(aid: str) -> dict:
    caps = load_initial_caps()
    latest = get_latest(aid)
    history = get_history(aid, None, None)
    metrics = compute_metrics(history, caps.get(aid))
    return {
        "account_id": aid,
        "latest": latest,
        "initial_capital": float(caps[aid]) if aid in caps else latest.get("initial_capital"),
        "trade_count": int(latest["trade_count"]) if "trade_count" in latest else None,
        "metrics": metrics,
        "server_time": _now_ms(),
    }


def slippage_history(aid: str, rng: str) -> dict:
    """每次调仓的滑点汇总点(由策略 _write_slippage_point 写入)。"""
    span = RANGE_MS.get(rng)
    start = None if span is None else _now_ms() - span
    key = f"slippage_history:binance:{aid}"
    lo = "-inf" if start is None else start
    points = []
    for raw in R.zrangebyscore(key, lo, "+inf"):
        try:
            points.append(json.loads(raw))
        except Exception:
            continue
    cost = sum(p.get("cost_usdt") or 0 for p in points)
    fee = sum(p.get("fee_usdt") or 0 for p in points)
    notional = sum(p.get("notional_usdt") or 0 for p in points)
    return {
        "account_id": aid, "range": rng, "points": points,
        "totals": {
            "rebalances": len(points),
            "cost_usdt": round(cost, 4), "fee_usdt": round(fee, 4),
            "notional_usdt": round(notional, 2),
            "wavg_bps": round(cost / notional * 10000, 4) if notional > 0 else None,
        },
        "server_time": _now_ms(),
    }


def equity_curve(aid: str, rng: str) -> dict:
    caps = load_initial_caps()
    span = RANGE_MS.get(rng, 30 * DAY_MS)
    start = None if span is None else _now_ms() - span
    history = get_history(aid, start, None)
    metrics = compute_metrics(history, caps.get(aid))      # 指标按所选区间算
    pts = [{"ts": h["ts"], "equity": round(h["equity"], 4),
            "upnl": round(h.get("upnl", 0), 4)} for h in _downsample(history, MAX_POINTS)]
    return {
        "account_id": aid, "range": rng,
        "initial_capital": float(caps[aid]) if aid in caps else None,
        "points": pts, "metrics": metrics, "server_time": _now_ms(),
    }


# ---------- HTTP ----------
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False, default=str).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self._send({}, 204)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        path = u.path.rstrip("/")
        try:
            if path == "/api/health":
                self._send({"ok": True, "server_time": _now_ms()})
            elif path == "/api/accounts_overview":
                self._send(overview())
            elif path == "/api/account_stats":
                aid = q.get("account_id", [""])[0]
                if not aid:
                    return self._send({"error": "missing account_id"}, 400)
                self._send(account_stats(aid))
            elif path == "/api/equity_curve":
                aid = q.get("account_id", [""])[0]
                rng = q.get("range", ["30d"])[0]
                if not aid:
                    return self._send({"error": "missing account_id"}, 400)
                self._send(equity_curve(aid, rng))
            elif path == "/api/slippage_history":
                aid = q.get("account_id", [""])[0]
                rng = q.get("range", ["all"])[0]
                if not aid:
                    return self._send({"error": "missing account_id"}, 400)
                self._send(slippage_history(aid, rng))
            else:
                self._send({"error": "not found", "path": path}, 404)
        except Exception as e:
            self._send({"error": str(e)}, 500)

    def log_message(self, fmt, *args):
        # 静默默认访问日志(避免污染), 保留错误由异常处理
        pass


def main():
    # 只绑 127.0.0.1: 账户财务数据无鉴权, 不直接对外暴露; 仅同机的 vite/nginx 反代可达。
    host = os.environ.get("STATS_API_HOST", "127.0.0.1")
    srv = ThreadingHTTPServer((host, PORT), Handler)
    print(f"[stats_api] 监听 {host}:{PORT}  (Redis {R.connection_pool.connection_kwargs.get('host')})")
    sys.stdout.flush()
    srv.serve_forever()


if __name__ == "__main__":
    main()
