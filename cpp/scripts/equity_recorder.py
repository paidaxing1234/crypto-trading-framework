#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按账户记录净值时序到 Redis —— 为前端的净值曲线 / 收益率 / 夏普 / 最大回撤提供数据源。

为什么独立成脚本(不改 C++ server):
  - C++ account_monitor 只在内存保留"最新净值", 不存历史; 重启就丢。
  - 净值曲线/夏普需要"时序", 而历史不补记 => 越早开始存越好。
  - 独立 cron 脚本零改动、零重启风险, 且只读 /fapi/v2/account(不下单)。

每次运行: 对每个 binance 账户查 /fapi/v2/account, 把一份快照写进 Redis 时序:
  ZADD equity_history:binance:{account_id}  score=ts_ms  value=JSON{ts, equity, available, wallet, upnl}
  另存最新一份到 hash  equity_latest:binance:{account_id}  (含 return_rate, 若 config 有 initial_capital)

建议 cron: 每 5 分钟一次。  */5 * * * * /usr/bin/python3 .../equity_recorder.py >> /var/log/equity_recorder.log 2>&1
"""
from __future__ import annotations
import os, sys, json, time, hmac, hashlib, urllib.parse, glob
import requests
import redis

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ACCT_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "strategies", "acount_configs"))
# 起始本金单独成文件, 不动 C++ server 加载的账户 config。 {account_id: 本金}
INIT_CAP_FILE = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "strategies", "configs", "initial_capital.json"))
MAINNET = "https://fapi.binance.com"
TESTNET = "https://testnet.binancefuture.com"
KEEP_DAYS = 400          # 时序保留天数 (ZREMRANGEBYSCORE 裁旧)
HTTP_TIMEOUT = 10
INCOME_WINDOW_MS = 7 * 86400 * 1000   # /fapi/v1/income 单窗口跨度上限


def _now_ms() -> int:
    return int(time.time() * 1000)


def _signed_get(base: str, path: str, api_key: str, secret: str, params: dict | None = None):
    params = dict(params or {})
    params["timestamp"] = _now_ms()
    params["recvWindow"] = 5000
    qs = urllib.parse.urlencode(params)
    sig = hmac.new(secret.encode(), qs.encode(), hashlib.sha256).hexdigest()
    url = f"{base}{path}?{qs}&signature={sig}"
    return requests.get(url, headers={"X-MBX-APIKEY": api_key}, timeout=HTTP_TIMEOUT)


def _income_count(base: str, api_key: str, secret: str, start_ms: int, end_ms: int,
                  income_type: str = "COMMISSION") -> int:
    """统计 [start_ms, end_ms] 内某类 income 条数。 每笔成交一条 COMMISSION => 近似成交笔数。
    按 7 天窗口 + limit=1000 翻页, 翻页用 lastTime+1 推进。"""
    total = 0
    cur = start_ms
    while cur <= end_ms:
        win_end = min(cur + INCOME_WINDOW_MS, end_ms)
        page_start = cur
        while True:
            params = {"incomeType": income_type, "startTime": page_start,
                      "endTime": win_end, "limit": 1000}
            resp = _signed_get(base, "/fapi/v1/income", api_key, secret, params)
            if resp.status_code != 200:
                raise RuntimeError(f"income HTTP {resp.status_code}: {resp.text[:120]}")
            arr = resp.json()
            if not arr:
                break
            total += len(arr)
            if len(arr) < 1000:
                break
            page_start = int(arr[-1]["time"]) + 1   # 下一页从最后一条之后开始
            if page_start > win_end:
                break
        cur = win_end + 1
    return total


def _update_trade_count(r: redis.Redis, aid: str, base: str, api_key: str, secret: str,
                        created_at_ms: int, now_ms: int) -> int | None:
    """增量累计成交笔数到 trade_count:binance:{id}; 首次从账户 created_at 回填。返回累计值。"""
    cursor_key = f"equity_recorder:income_cursor:binance:{aid}"
    count_key = f"trade_count:binance:{aid}"
    cursor = r.get(cursor_key)
    start = int(cursor) if cursor else created_at_ms     # 首次: 从开户时间回填
    try:
        new_n = _income_count(base, api_key, secret, start, now_ms)
    except Exception as e:
        print(f"  [{aid}] 成交笔数查询失败(本次跳过): {e}")
        return None
    if new_n:
        r.incrby(count_key, new_n)
    r.set(cursor_key, now_ms + 1)                        # 下次从 now+1 开始, 不重不漏
    return int(r.get(count_key) or 0)


def _load_initial_capital() -> dict:
    """读取 {account_id: 起始本金}; 文件不存在或损坏返回空 dict(降级为不显示收益率)。"""
    try:
        return json.load(open(INIT_CAP_FILE, encoding="utf-8"))
    except FileNotFoundError:
        return {}
    except Exception as e:
        print(f"  [警告] initial_capital.json 解析失败, 忽略: {e}")
        return {}


def _load_accounts() -> list[dict]:
    accts = []
    for fp in sorted(glob.glob(os.path.join(ACCT_DIR, "*.json"))):
        try:
            d = json.load(open(fp, encoding="utf-8"))
        except Exception as e:
            print(f"  [跳过] {os.path.basename(fp)} 解析失败: {e}")
            continue
        if (d.get("exchange", "").lower() == "binance"
                and d.get("api_key") and d.get("secret_key")):
            accts.append(d)
    return accts


def record_one(r: redis.Redis, acct: dict, init_caps: dict) -> bool:
    aid = acct["account_id"]
    base = TESTNET if acct.get("is_testnet") else MAINNET
    try:
        resp = _signed_get(base, "/fapi/v2/account", acct["api_key"], acct["secret_key"])
    except Exception as e:
        print(f"  [{aid}] 请求异常: {e}")
        return False
    if resp.status_code != 200:
        print(f"  [{aid}] HTTP {resp.status_code}: {resp.text[:120]}")
        return False
    a = resp.json()
    ts = _now_ms()
    snap = {
        "ts": ts,
        "equity": float(a.get("totalMarginBalance", 0)),       # 净值 = 钱包余额 + 未实现盈亏
        "available": float(a.get("availableBalance", 0)),       # 可用余额
        "wallet": float(a.get("totalWalletBalance", 0)),        # 钱包余额
        "upnl": float(a.get("totalUnrealizedProfit", 0)),       # 未实现盈亏
    }
    hist_key = f"equity_history:binance:{aid}"
    r.zadd(hist_key, {json.dumps(snap, separators=(',', ':')): ts})
    r.zremrangebyscore(hist_key, 0, ts - KEEP_DAYS * 86400000)   # 裁旧

    # 成交笔数(增量累计; 首次从开户时间回填)
    created_at = acct.get("created_at")
    created_ms = ts
    if created_at:
        try:
            created_ms = int(time.mktime(time.strptime(created_at, "%Y-%m-%dT%H:%M:%SZ")) * 1000)
        except Exception:
            pass
    trade_count = _update_trade_count(r, aid, base, acct["api_key"], acct["secret_key"],
                                      created_ms, ts)

    # 最新快照 + 收益率(若 initial_capital.json 配了本金)
    latest = dict(snap)
    if trade_count is not None:
        latest["trade_count"] = trade_count
    init_cap = init_caps.get(aid)
    if init_cap:
        latest["initial_capital"] = float(init_cap)
        latest["return_rate"] = (snap["equity"] - float(init_cap)) / float(init_cap)
        latest["pnl"] = snap["equity"] - float(init_cap)
    r.hset(f"equity_latest:binance:{aid}", mapping={k: str(v) for k, v in latest.items()})

    print(f"  [{aid}] 净值={snap['equity']:.2f} 可用={snap['available']:.2f} "
          f"未实现={snap['upnl']:+.2f}"
          + (f" 收益率={latest['return_rate']*100:+.2f}%" if init_cap else " (未配本金)")
          + (f" 成交={trade_count}笔" if trade_count is not None else ""))
    return True


def main() -> int:
    r = redis.Redis(host=os.environ.get("REDIS_HOST", "127.0.0.1"),
                    port=int(os.environ.get("REDIS_PORT", 6379)),
                    password=os.environ.get("REDIS_PASSWORD") or None,
                    decode_responses=True)
    accts = _load_accounts()
    init_caps = _load_initial_capital()
    print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] equity_recorder: {len(accts)} 个 binance 账户, "
          f"{len(init_caps)} 个配了起始本金")
    ok = sum(record_one(r, a, init_caps) for a in accts)
    print(f"  完成: {ok}/{len(accts)} 成功")
    return 0 if ok == len(accts) else 1


if __name__ == "__main__":
    sys.exit(main())
