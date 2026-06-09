#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""回填 Binance 历史净值到 Redis 时序，让前端净值曲线有"密集"的历史（不再是每天 1 点的粗线）。

两路数据源（按密度优先）：
  1) /fapi/v1/income —— 每笔已实现盈亏/资金费/手续费/划转都有时间戳，累加重建**钱包净值**曲线，
     按**小时**聚合 => 历史段也有 ~24 点/天，和记录器(5分钟)视觉一致。这是主源。
  2) /sapi/v1/accountSnapshot (FUTURES, 每日) —— 仅用于补 income 覆盖不到的更早一段（每天 1 点的尾巴）。

写入 ZADD equity_history:binance:{aid}（source=income/snapshot），只写到"记录器第一个实时点之前"，
不与今日 5 分钟实时点重叠。每次跑都先清掉旧的历史回填段再重写（幂等）。

注意：历史段是**已实现钱包净值**(income 重建)，近段(记录器)是**含未实现的净值**，交界处可能有一个≈当时未实现盈亏
的小台阶——这是 Binance 不提供历史未实现盈亏所致，属可接受。

一次性跑可回填；也由 cron 每日跑一次。 python3 equity_backfill.py
"""
from __future__ import annotations
import os, sys, json, time, hmac, hashlib, urllib.parse, glob
import requests
import redis

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ACCT_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "strategies", "acount_configs"))
FAPI = "https://fapi.binance.com"
SAPI = "https://api.binance.com"
HTTP_TIMEOUT = 20
INCOME_WIN_MS = 7 * 86400 * 1000
HOUR_MS = 3600000
BUCKET_MS = 5 * 60 * 1000          # 历史 income 聚合粒度(对齐记录器的 5 分钟)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _signed_get(base, path, api_key, secret, params=None):
    params = dict(params or {})
    params["timestamp"] = _now_ms()
    params["recvWindow"] = 5000
    qs = urllib.parse.urlencode(params)
    sig = hmac.new(secret.encode(), qs.encode(), hashlib.sha256).hexdigest()
    return requests.get(f"{base}{path}?{qs}&signature={sig}",
                        headers={"X-MBX-APIKEY": api_key}, timeout=HTTP_TIMEOUT)


def _load_accounts():
    out = []
    for fp in sorted(glob.glob(os.path.join(ACCT_DIR, "*.json"))):
        try:
            d = json.load(open(fp, encoding="utf-8"))
        except Exception:
            continue
        if d.get("exchange", "").lower() == "binance" and d.get("api_key") and d.get("secret_key"):
            out.append(d)
    return out


def _created_ms(acct) -> int:
    c = acct.get("created_at")
    if c:
        try:
            return int(time.mktime(time.strptime(c, "%Y-%m-%dT%H:%M:%SZ")) * 1000)
        except Exception:
            pass
    return _now_ms() - 90 * 86400000


def _first_recorder_ts(r, key) -> int | None:
    """记录器第一个实时点(无 source 字段)的 ts；没有则 None。"""
    best = None
    for raw in r.zrange(key, 0, -1):
        try:
            o = json.loads(raw)
        except Exception:
            continue
        if "source" not in o:                       # 实时记录器点没有 source
            ts = int(o.get("ts", 0))
            if best is None or ts < best:
                best = ts
    return best


def _fetch_income(acct, start_ms, end_ms):
    """返回 [(time_ms, income_float), ...]（全类型：已实现/资金费/手续费/划转）。"""
    key, sec = acct["api_key"], acct["secret_key"]
    events, cur = [], start_ms
    while cur <= end_ms:
        we = min(cur + INCOME_WIN_MS, end_ms)
        ps = cur
        while True:
            resp = _signed_get(FAPI, "/fapi/v1/income", key, sec,
                               {"startTime": ps, "endTime": we, "limit": 1000})
            if resp.status_code != 200:
                raise RuntimeError(f"income HTTP {resp.status_code}: {resp.text[:120]}")
            arr = resp.json()
            if not isinstance(arr, list) or not arr:
                break
            for x in arr:
                events.append((int(x["time"]), float(x["income"])))
            if len(arr) < 1000:
                break
            ps = int(arr[-1]["time"]) + 1
            if ps > we:
                break
        cur = we + 1
    events.sort()
    return events


def _fetch_snapshot_daily(acct):
    """[(ts, margin, wallet), ...]，仅 USDT 资产，最多 ~30 天。"""
    resp = _signed_get(SAPI, "/sapi/v1/accountSnapshot", acct["api_key"], acct["secret_key"],
                       {"type": "FUTURES", "limit": 30})
    if resp.status_code != 200:
        return []
    out = []
    for v in resp.json().get("snapshotVos", []):
        ts = int(v.get("updateTime", 0))
        a = next((x for x in v.get("data", {}).get("assets", []) if x.get("asset") == "USDT"), None)
        if ts and a:
            out.append((ts, float(a.get("marginBalance", 0)), float(a.get("walletBalance", 0))))
    return out


def _interp_unrealized(t, snap_u):
    """按日快照的未实现盈亏序列(piecewise linear)估算 t 时刻的未实现; 区间外 carry 端点值。"""
    if not snap_u:
        return 0.0
    if t <= snap_u[0][0]:
        return snap_u[0][1]
    if t >= snap_u[-1][0]:
        return snap_u[-1][1]
    for i in range(1, len(snap_u)):
        t0, u0 = snap_u[i - 1]
        t1, u1 = snap_u[i]
        if t0 <= t <= t1:
            return u0 + (u1 - u0) * (t - t0) / (t1 - t0) if t1 > t0 else u0
    return snap_u[-1][1]


def backfill_one(r, acct) -> int:
    aid = acct["account_id"]
    if acct.get("is_testnet"):
        print(f"  [{aid}] testnet 跳过")
        return 0
    key = f"equity_history:binance:{aid}"
    now = _now_ms()
    cap = _first_recorder_ts(r, key) or now          # 只回填到第一个实时点之前

    # 当前钱包净值，用作 income 重建的锚点
    try:
        acc = _signed_get(FAPI, "/fapi/v2/account", acct["api_key"], acct["secret_key"])
        W_now = float(acc.json().get("totalWalletBalance", 0))
    except Exception as e:
        print(f"  [{aid}] 取 account 失败: {e}")
        return 0

    # income 小时级重建
    points = {}     # ts -> (equity, wallet, source)
    try:
        events = _fetch_income(acct, _created_ms(acct), now)
    except Exception as e:
        print(f"  [{aid}] income 失败({e}), 退回每日快照")
        events = []

    # 日快照: 提供历史"未实现盈亏"(margin−wallet)，把钱包重建抬成净值，避免与记录器段出现台阶
    try:
        snap_daily = _fetch_snapshot_daily(acct)
    except Exception:
        snap_daily = []
    snap_u = sorted((ts, m - w) for ts, m, w in snap_daily)   # [(ts, 未实现)]

    income_start = None
    if events:
        total = sum(inc for _, inc in events)
        base_wallet = W_now - total                  # 第一笔 income 之前的钱包
        income_start = events[0][0]
        cum = 0.0
        for t, inc in events:
            cum += inc
            if t >= cap:                             # 不与实时点重叠
                continue
            bkt = (t // BUCKET_MS) * BUCKET_MS        # 按 5 分钟聚合，最后一笔覆盖
            wal = base_wallet + cum
            u = _interp_unrealized(bkt, snap_u)       # 插值未实现盈亏
            points[bkt] = (wal + u, wal, "income")    # 净值 = 钱包 + 未实现(与记录器口径一致)

    # income 覆盖不到的更早一段: 用每日快照(本身就是含未实现的净值)
    for ts, margin, wallet in snap_daily:
        if ts >= cap:
            continue
        if income_start is not None and ts >= income_start:
            continue                                  # income 已覆盖
        if margin > 0:
            points[ts] = (margin, wallet, "snapshot")

    if not points:
        print(f"  [{aid}] 无可回填数据")
        return 0

    # 清掉旧的历史回填段（cap 之前），重写（幂等）
    r.zremrangebyscore(key, 0, cap - 1)
    pipe = r.pipeline()
    for ts, (eq, wal, src) in points.items():
        snap = {"ts": ts, "equity": round(eq, 8), "available": None,
                "wallet": round(wal, 8), "upnl": round(eq - wal, 8), "source": src}
        pipe.zadd(key, {json.dumps(snap, separators=(',', ':'), sort_keys=True): ts})
    pipe.execute()

    n_inc = sum(1 for v in points.values() if v[2] == "income")
    n_snap = len(points) - n_inc
    span_d = (max(points) - min(points)) / 86400000
    print(f"  [{aid}] 回填 {len(points)} 点 (income 小时级 {n_inc} + 日快照 {n_snap}), 跨 {span_d:.1f} 天")
    return len(points)


def main():
    r = redis.Redis(host=os.environ.get("REDIS_HOST", "127.0.0.1"),
                    port=int(os.environ.get("REDIS_PORT", 6379)),
                    password=os.environ.get("REDIS_PASSWORD") or None,
                    decode_responses=True)
    accts = _load_accounts()
    print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] equity_backfill v2: {len(accts)} 个 binance 账户")
    tot = sum(backfill_one(r, a) for a in accts)
    print(f"  完成: 共回填 {tot} 个历史净值点")
    return 0


if __name__ == "__main__":
    sys.exit(main())
