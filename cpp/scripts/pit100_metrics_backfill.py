#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用 Binance Vision 日度 metrics 归档回填 OI/多空比深历史(一次性), 补 REST 30 天限制之前的部分。

数据源: https://data.binance.vision/data/futures/um/daily/metrics/{SYM}/{SYM}-metrics-YYYY-MM-DD.zip
  CSV 5分钟粒度; 只取整点行(create_time = HH:00:00) → 与 REST /futures/data/* 的小时点**逐位一致**
  (已实证: BTCUSDT 2026-06-01 12:00 的 sum_open_interest == REST oi, ratio 仅精度差)。

写入与 pit100_data_recorder.py 完全相同的 key/格式(幂等, 同 score 先删后写):
  oi:binance:{sym}:1h        {"timestamp","oi","oi_value"}
  lsr_top:binance:{sym}:1h   {"timestamp","ratio","long","short"}   long=r/(1+r), short=1/(1+r)
  lsr_acct:binance:{sym}:1h  {"timestamp","ratio","long","short"}

另: 顺手把 kline_tbb(taker买入币量) 用 REST klines limit=1000 加深到 ~41 天(一次一币一调用)。

用法: python3 pit100_metrics_backfill.py [--days 40] [--workers 8]
之后的增量由小时 cron 的 pit100_data_recorder.py 持续向前写。
"""
from __future__ import annotations
import argparse
import csv
import io
import json
import os
import sys
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timedelta, timezone
import requests
import redis

VISION = "https://data.binance.vision/data/futures/um/daily/metrics"
FAPI = "https://fapi.binance.com"
HOUR_MS = 3600000
HTTP_TIMEOUT = 20

_tls = {}


def _session() -> requests.Session:
    import threading
    tid = threading.get_ident()
    if tid not in _tls:
        _tls[tid] = requests.Session()
    return _tls[tid]


def _symbols() -> list:
    r = requests.get(f"{FAPI}/fapi/v1/exchangeInfo", timeout=HTTP_TIMEOUT).json()
    return sorted(s["symbol"] for s in r.get("symbols", [])
                  if s.get("contractType") == "PERPETUAL" and s.get("quoteAsset") == "USDT"
                  and s.get("status") == "TRADING" and s.get("underlyingType", "COIN") == "COIN")


def _zwrite(r: redis.Redis, key: str, rows: list):
    if not rows:
        return 0
    pipe = r.pipeline()
    for row in rows:
        ts = int(row["timestamp"])
        pipe.zremrangebyscore(key, ts, ts)
        pipe.zadd(key, {json.dumps(row, separators=(',', ':'), sort_keys=True): ts})
    pipe.execute()
    return len(rows)


def backfill_symbol(r: redis.Redis, sym: str, dates: list) -> dict:
    """下载该币各日 metrics zip, 取整点行写入 3 个 key。返回统计。"""
    n_pt, n_404, n_err = 0, 0, 0
    oi_rows, lt_rows, la_rows = [], [], []
    for d in dates:
        url = f"{VISION}/{sym}/{sym}-metrics-{d}.zip"
        try:
            resp = _session().get(url, timeout=HTTP_TIMEOUT)
            if resp.status_code == 404:        # 该日未上市/无数据
                n_404 += 1
                continue
            resp.raise_for_status()
            z = zipfile.ZipFile(io.BytesIO(resp.content))
            with z.open(z.namelist()[0]) as fp:
                for row in csv.DictReader(io.TextIOWrapper(fp, encoding="utf-8")):
                    ct = row.get("create_time", "")
                    if not ct.endswith(":00:00"):      # 只要整点(小时)行
                        continue
                    ts = int(datetime.strptime(ct, "%Y-%m-%d %H:%M:%S")
                             .replace(tzinfo=timezone.utc).timestamp() * 1000)
                    try:
                        oi_rows.append({"timestamp": ts,
                                        "oi": float(row["sum_open_interest"]),
                                        "oi_value": float(row["sum_open_interest_value"])})
                    except (ValueError, KeyError):
                        pass
                    for src_col, acc in (("sum_toptrader_long_short_ratio", lt_rows),
                                         ("count_long_short_ratio", la_rows)):
                        try:
                            ratio = float(row[src_col])
                            acc.append({"timestamp": ts, "ratio": ratio,
                                        "long": ratio / (1 + ratio),
                                        "short": 1 / (1 + ratio)})
                        except (ValueError, KeyError, ZeroDivisionError):
                            pass
                    n_pt += 1
        except Exception:
            n_err += 1
    _zwrite(r, f"oi:binance:{sym}:1h", oi_rows)
    _zwrite(r, f"lsr_top:binance:{sym}:1h", lt_rows)
    _zwrite(r, f"lsr_acct:binance:{sym}:1h", la_rows)
    return {"pts": n_pt, "404": n_404, "err": n_err}


def deepen_tbb(r: redis.Redis, sym: str) -> int:
    """REST klines limit=1000(~41天) 加深 taker_buy_base; 只写已收盘 bar。"""
    try:
        arr = _session().get(f"{FAPI}/fapi/v1/klines",
                             params={"symbol": sym, "interval": "1h", "limit": 1000},
                             timeout=HTTP_TIMEOUT).json()
    except Exception:
        return 0
    if not isinstance(arr, list):
        return 0
    now = int(time.time() * 1000)
    rows = [{"timestamp": int(k[0]), "taker_buy_base": float(k[9])}
            for k in arr if int(k[6]) < now]
    return _zwrite(r, f"kline_tbb:binance:{sym}:1h", rows)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=int, default=40, help="回填总深度(天)")
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()

    r = redis.Redis(host=os.environ.get("REDIS_HOST", "127.0.0.1"),
                    port=int(os.environ.get("REDIS_PORT", 6379)),
                    password=os.environ.get("REDIS_PASSWORD") or None,
                    decode_responses=True)
    syms = _symbols()
    today = datetime.now(timezone.utc).date()
    # 昨日及更早(今日的日度归档尚未生成); REST recorder 已覆盖近 ~20 天, 多回填重写无害(幂等)
    dates = [str(today - timedelta(days=i)) for i in range(1, args.days + 1)]
    t0 = time.time()
    print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] vision 回填: {len(syms)} 币 × {len(dates)} 天 "
          f"({dates[-1]} ~ {dates[0]})")

    tot_pts = tot_404 = tot_err = done = 0
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = {ex.submit(backfill_symbol, r, s, dates): s for s in syms}
        for f in as_completed(futs):
            st = f.result()
            tot_pts += st["pts"]; tot_404 += st["404"]; tot_err += st["err"]
            done += 1
            if done % 50 == 0:
                print(f"  进度 {done}/{len(syms)} ({time.time()-t0:.0f}s)")
    print(f"  metrics: 整点 {tot_pts} 条 | 缺日(未上市) {tot_404} | 错误 {tot_err} "
          f"| 耗时 {time.time()-t0:.0f}s")

    n_tbb = 0
    with ThreadPoolExecutor(max_workers=4) as ex:
        futs = {ex.submit(deepen_tbb, r, s): s for s in syms}
        for f in as_completed(futs):
            n_tbb += f.result()
    print(f"  tbb 加深: {n_tbb} 条 | 总耗时 {time.time()-t0:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
