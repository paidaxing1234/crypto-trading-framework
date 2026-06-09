#!/usr/bin/env python3
"""导出账户交割单(逐笔成交明细) → CSV

数据源: Binance UM 期货私有接口
  1) /fapi/v1/income     — 发现该账户交易过的 symbol 集合 + 每币首末时间(income 留存约3个月)
  2) /fapi/v1/userTrades — 按 symbol 拉全部逐笔成交(fromId 翻页, 不受7天窗限制)

输出: tradinginformation/{account_id}_交割单_{YYYYMMDD}.csv
列: time_utc, time_cst, symbol, side, position_side, price, qty, quote_qty,
    realized_pnl, commission, commission_asset, maker, order_id, trade_id

限频: userTrades weight=5, income weight=30, IP 上限 2400/min。
      userTrades 间隔 0.15s(≈2000 weight/min), income 间隔 1.0s。撞 429 指数退避。

用法:
  python3 export_trade_statements.py                      # 全部账户
  python3 export_trade_statements.py --account <account_id>  # 单账户
"""
import argparse
import csv
import glob
import hashlib
import hmac
import json
import os
import time
import urllib.parse
from datetime import datetime, timezone, timedelta

import requests

BASE = "https://fapi.binance.com"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))          # cpp/
CONF_DIR = os.path.join(ROOT, "strategies", "acount_configs")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "tradinginformation")
CST = timezone(timedelta(hours=8))

TRADE_SLEEP = 0.15
INCOME_SLEEP = 1.0
LOOKBACK_DAYS = 92          # income 留存约 3 个月


def log(msg: str):
    print(f"[{datetime.now(CST).strftime('%H:%M:%S')}] {msg}", flush=True)


class Client:
    def __init__(self, api_key: str, secret: str):
        self.ak, self.sk = api_key, secret
        self.sess = requests.Session()
        self.sess.headers["X-MBX-APIKEY"] = api_key

    def get(self, path: str, params: dict, max_retries: int = 5):
        for attempt in range(max_retries):
            p = dict(params)
            p["timestamp"] = int(time.time() * 1000)
            p["recvWindow"] = 10000
            qs = urllib.parse.urlencode(p)
            sig = hmac.new(self.sk.encode(), qs.encode(), hashlib.sha256).hexdigest()
            try:
                r = self.sess.get(f"{BASE}{path}?{qs}&signature={sig}", timeout=15)
                if r.status_code in (429, 418):
                    wait = 2 ** (attempt + 1)
                    log(f"  限频 {r.status_code}, 退避 {wait}s ...")
                    time.sleep(wait)
                    continue
                r.raise_for_status()
                return r.json()
            except requests.RequestException as e:
                if attempt == max_retries - 1:
                    raise
                time.sleep(1 + attempt)
        return None


def discover_symbols(cli: Client, t0: int, t1: int):
    """用 income 翻页发现交易过的 symbol + 每币 [首, 末] 时间(只看带 symbol 的记录)"""
    spans = {}
    cur, total = t0, 0
    while cur < t1:
        rows = cli.get("/fapi/v1/income", {"startTime": cur, "endTime": t1, "limit": 1000})
        time.sleep(INCOME_SLEEP)
        if not rows:
            break
        total += len(rows)
        for it in rows:
            sym = it.get("symbol") or ""
            ts = int(it["time"])
            if sym:
                lo, hi = spans.get(sym, (ts, ts))
                spans[sym] = (min(lo, ts), max(hi, ts))
        last_ts = int(rows[-1]["time"])
        if len(rows) < 1000:
            break
        cur = max(last_ts, cur + 1)          # 同毫秒重复靠 trade 去重兜底
    log(f"  income 扫描 {total} 条 → {len(spans)} 个交易过的 symbol")
    return spans


def trades_for_symbol(cli: Client, sym: str, lo: int, hi: int):
    """先用 7 天窗定位首笔, 再 fromId 翻页拉全(fromId 不受时间窗限制)"""
    first_id = None
    cur = max(0, lo - 86_400_000)            # 首笔 income 前 1 天起找
    end = hi + 86_400_000
    while cur < end and first_id is None:
        w_end = min(cur + 7 * 86_400_000 - 1, end)
        rows = cli.get("/fapi/v1/userTrades",
                       {"symbol": sym, "startTime": cur, "endTime": w_end, "limit": 5})
        time.sleep(TRADE_SLEEP)
        if rows:
            first_id = rows[0]["id"]
            break
        cur = w_end + 1
    if first_id is None:
        return []
    out, fid = [], first_id
    while True:
        rows = cli.get("/fapi/v1/userTrades", {"symbol": sym, "fromId": fid, "limit": 1000})
        time.sleep(TRADE_SLEEP)
        if not rows:
            break
        out.extend(rows)
        if len(rows) < 1000:
            break
        fid = rows[-1]["id"] + 1
    return out


def export_account(cfg_path: str):
    cfg = json.load(open(cfg_path))
    aid = cfg.get("account_id") or os.path.basename(cfg_path).replace(".json", "")
    cli = Client(cfg["api_key"], cfg["secret_key"])
    t1 = int(time.time() * 1000)
    t0 = t1 - LOOKBACK_DAYS * 86_400_000
    log(f"== 账户 {aid}: 扫描 income 发现 symbol ==")
    spans = discover_symbols(cli, t0, t1)

    all_trades = []
    for i, (sym, (lo, hi)) in enumerate(sorted(spans.items()), 1):
        rows = trades_for_symbol(cli, sym, lo, hi)
        all_trades.extend(rows)
        if i % 25 == 0 or i == len(spans):
            log(f"  [{i}/{len(spans)}] 累计 {len(all_trades)} 笔")

    # 去重 + 按时间排序
    seen, uniq = set(), []
    for t in all_trades:
        k = (t["symbol"], t["id"])
        if k not in seen:
            seen.add(k)
            uniq.append(t)
    uniq.sort(key=lambda t: (int(t["time"]), t["symbol"], int(t["id"])))

    os.makedirs(OUT_DIR, exist_ok=True)
    out_path = os.path.join(OUT_DIR, f"{aid}_交割单_{datetime.now(CST).strftime('%Y%m%d')}.csv")
    with open(out_path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["time_utc", "time_cst", "symbol", "side", "position_side",
                    "price", "qty", "quote_qty", "realized_pnl",
                    "commission", "commission_asset", "maker", "order_id", "trade_id"])
        for t in uniq:
            ts = int(t["time"]) / 1000
            w.writerow([
                datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                datetime.fromtimestamp(ts, CST).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                t["symbol"], t["side"], t.get("positionSide", ""),
                t["price"], t["qty"], t["quoteQty"], t.get("realizedPnl", ""),
                t.get("commission", ""), t.get("commissionAsset", ""),
                t.get("maker", ""), t["orderId"], t["id"]])
    log(f"  ✓ {aid}: {len(uniq)} 笔 → {out_path}")
    return aid, len(uniq), out_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--account", default="", help="只导某个 account_id, 默认全部")
    args = ap.parse_args()
    results = []
    for cfg_path in sorted(glob.glob(os.path.join(CONF_DIR, "*.json"))):
        cfg = json.load(open(cfg_path))
        aid = cfg.get("account_id", "")
        if args.account and aid != args.account:
            continue
        if not cfg.get("api_key"):
            log(f"跳过 {cfg_path}(无密钥)")
            continue
        results.append(export_account(cfg_path))
    log("===== 汇总 =====")
    for aid, n, p in results:
        log(f"  {aid}: {n} 笔 → {p}")


if __name__ == "__main__":
    main()
