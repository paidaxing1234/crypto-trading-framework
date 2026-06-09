#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pit100 策略的原始数据小时级记录器 —— 把 Redis 里缺的三类因子输入攒起来。

为什么必须现在就跑:
  - 79 因子中 ~26 个依赖持仓量(OI)、3 个依赖多空比, 而币安 /futures/data/* 只保留 ~30 天历史,
    不自己攒, 永远只有 30 天。
  - Redis 的 1h K线缺 taker_buy_base(主动买入"币量", kline 数组下标[9]); 现有 buy_amount 是
    quote 量([10]), 部分因子两者都要。

每小时写入(ZSET, score=bar开盘 ts_ms):
  oi:binance:{sym}:1h        {"timestamp","oi","oi_value"}            ← openInterestHist(sumOpenInterest/Value)
  lsr_top:binance:{sym}:1h   {"timestamp","ratio","long","short"}     ← topLongShortPositionRatio
  lsr_acct:binance:{sym}:1h  {"timestamp","ratio","long","short"}     ← globalLongShortAccountRatio
  kline_tbb:binance:{sym}:1h {"timestamp","taker_buy_base"}           ← /fapi/v1/klines[9] (仅已收盘bar)

首跑自动回填(limit=500 ≈ 20天, 已是币安能给的接近上限); 之后增量。幂等(同 score 成员先删后写)。
限速: /futures/data 共享低限额(~1000次/5分钟), 按 ~3 次/秒匀速; fapi klines 走独立 weight, 并发小池。

cron: 0 * * * *  (每小时第0分, 全市场约 8-12 分钟跑完)
"""
from __future__ import annotations
import os, sys, json, time, glob
from concurrent.futures import ThreadPoolExecutor, as_completed
import requests
import redis

FAPI = "https://fapi.binance.com"
KEEP_MS = 120 * 86400000          # 各时序保留 120 天
HOUR_MS = 3600000
BACKFILL_LIMIT = 500              # 首跑回填条数(币安 /futures/data 上限 500)
INCR_LIMIT = 3                    # 增量每次拉最近3个点(容忍上轮缺口)
FD_PACE_SEC = 0.34                # /futures/data 节奏: ~3次/秒 → 1800次≈10分钟
HTTP_TIMEOUT = 12

_session = requests.Session()


def _get(path: str, params: dict, retries: int = 3):
    for attempt in range(retries):
        try:
            r = _session.get(f"{FAPI}{path}", params=params, timeout=HTTP_TIMEOUT)
            if r.status_code == 429:
                time.sleep(5 * (attempt + 1))
                continue
            r.raise_for_status()
            return r.json()
        except Exception:
            if attempt < retries - 1:
                time.sleep(1 + attempt)
    return None


def _symbols() -> list:
    """全市场 USDT 永续(币本位标的, 排除 TradFi 股票/商品合约)"""
    data = _get("/fapi/v1/exchangeInfo", {})
    out = []
    for s in (data or {}).get("symbols", []):
        if (s.get("contractType") == "PERPETUAL" and s.get("quoteAsset") == "USDT"
                and s.get("status") == "TRADING"
                and s.get("underlyingType", "COIN") == "COIN"):
            out.append(s["symbol"])
    return sorted(out)


def _zwrite(r: redis.Redis, key: str, rows: list, ts_field: str = "timestamp"):
    """幂等写: 同 score 先删后加, 再裁旧。rows 内元素须含 ts_field(ms)。"""
    if not rows:
        return 0
    pipe = r.pipeline()
    for row in rows:
        ts = int(row[ts_field])
        pipe.zremrangebyscore(key, ts, ts)
        pipe.zadd(key, {json.dumps(row, separators=(',', ':'), sort_keys=True): ts})
    pipe.zremrangebyscore(key, 0, int(time.time() * 1000) - KEEP_MS)
    pipe.execute()
    return len(rows)


def _limit_for(r: redis.Redis, key: str) -> int:
    return BACKFILL_LIMIT if r.zcard(key) == 0 else INCR_LIMIT


def record_futures_data(r: redis.Redis, sym: str) -> dict:
    """OI + 两个多空比(低限额端点, 由调用方控制节奏)"""
    stats = {}
    # --- OI ---
    key = f"oi:binance:{sym}:1h"
    arr = _get("/futures/data/openInterestHist",
               {"symbol": sym, "period": "1h", "limit": _limit_for(r, key)})
    rows = [{"timestamp": int(x["timestamp"]),
             "oi": float(x["sumOpenInterest"]),
             "oi_value": float(x["sumOpenInterestValue"])} for x in (arr or [])]
    stats["oi"] = _zwrite(r, key, rows)
    time.sleep(FD_PACE_SEC)
    # --- top trader position L/S ---
    key = f"lsr_top:binance:{sym}:1h"
    arr = _get("/futures/data/topLongShortPositionRatio",
               {"symbol": sym, "period": "1h", "limit": _limit_for(r, key)})
    rows = [{"timestamp": int(x["timestamp"]),
             "ratio": float(x["longShortRatio"]),
             "long": float(x["longAccount"]), "short": float(x["shortAccount"])}
            for x in (arr or [])]
    stats["lsr_top"] = _zwrite(r, key, rows)
    time.sleep(FD_PACE_SEC)
    # --- global account L/S ---
    key = f"lsr_acct:binance:{sym}:1h"
    arr = _get("/futures/data/globalLongShortAccountRatio",
               {"symbol": sym, "period": "1h", "limit": _limit_for(r, key)})
    rows = [{"timestamp": int(x["timestamp"]),
             "ratio": float(x["longShortRatio"]),
             "long": float(x["longAccount"]), "short": float(x["shortAccount"])}
            for x in (arr or [])]
    stats["lsr_acct"] = _zwrite(r, key, rows)
    time.sleep(FD_PACE_SEC)
    return stats


def record_tbb(r: redis.Redis, sym: str) -> int:
    """taker_buy_base 补全(fapi klines, 独立 weight 池, 可并发)。只写已收盘 bar。"""
    key = f"kline_tbb:binance:{sym}:1h"
    limit = BACKFILL_LIMIT if r.zcard(key) == 0 else INCR_LIMIT
    arr = _get("/fapi/v1/klines", {"symbol": sym, "interval": "1h", "limit": limit})
    if not arr:
        return 0
    now = int(time.time() * 1000)
    rows = [{"timestamp": int(k[0]), "taker_buy_base": float(k[9])}
            for k in arr if int(k[6]) < now]          # k[6]=close_time, 只要已收盘
    return _zwrite(r, key, rows)


def main() -> int:
    t0 = time.time()
    r = redis.Redis(host=os.environ.get("REDIS_HOST", "127.0.0.1"),
                    port=int(os.environ.get("REDIS_PORT", 6379)),
                    password=os.environ.get("REDIS_PASSWORD") or None,
                    decode_responses=True)
    syms = _symbols()
    print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] pit100_recorder: {len(syms)} 个 USDT 永续")
    if not syms:
        return 1

    # ① taker_buy_base: fapi 池, 4 并发
    n_tbb = 0
    with ThreadPoolExecutor(max_workers=4) as ex:
        futs = {ex.submit(record_tbb, r, s): s for s in syms}
        for f in as_completed(futs):
            try:
                n_tbb += f.result()
            except Exception:
                pass
    print(f"  tbb 写入 {n_tbb} 条, 耗时 {time.time()-t0:.0f}s")

    # ② /futures/data: 串行匀速(限额低)
    n_oi = n_t = n_a = fail = 0
    for i, s in enumerate(syms):
        try:
            st = record_futures_data(r, s)
            n_oi += st.get("oi", 0); n_t += st.get("lsr_top", 0); n_a += st.get("lsr_acct", 0)
        except Exception:
            fail += 1
        if (i + 1) % 100 == 0:
            print(f"  futures/data 进度 {i+1}/{len(syms)} ({time.time()-t0:.0f}s)")
    print(f"  OI {n_oi} | lsr_top {n_t} | lsr_acct {n_a} | 失败 {fail} | 总耗时 {time.time()-t0:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
