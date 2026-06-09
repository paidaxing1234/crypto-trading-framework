#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Binance USDT 永续合约 funding rate 日频预加载 / 增量补缺

设计:
    1. 扫描 Redis 中过去 120 天每个币种缺失哪些 UTC 日
    2. 只拉缺失的天（跳过今天 UTC，因为当天 3 笔 8h 未全部结算）
    3. 写入 Redis 后，删除超过 120 天的旧数据

Redis 格式:
    key:   funding_rate:binance:{SYMBOL}:1d
    score: UTC 日起始毫秒
    value: JSON {"timestamp", "symbol", "funding_rate", "date"}

使用方法:
    python preload_funding_rate.py                       # 默认 120 天
    python preload_funding_rate.py --days 90             # 自定义天数
    python preload_funding_rate.py --max-workers 3       # 降低并发避免限频
    python preload_funding_rate.py --force               # 忽略缓存，全量重拉

建议 cron (UTC 00:05，等当日第一笔 funding 结算完再跑):
    5 0 * * * /usr/bin/python3 /path/to/preload_funding_rate.py >> /var/log/funding_rate.log 2>&1

环境变量:
    REDIS_HOST / REDIS_PORT / REDIS_PASSWORD
    USE_PROXY / PROXY_HOST / PROXY_PORT
"""

import os
import sys
import time
import json
import argparse
import requests
import redis
from datetime import datetime, timezone, timedelta
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, List, Set, Tuple

# ---------- 常量 ----------
BINANCE_FAPI = "https://fapi.binance.com"
MAX_DAYS = 120
EXPIRE_SECONDS = MAX_DAYS * 24 * 60 * 60
DAY_MS = 86400 * 1000

REDIS_HOST = os.environ.get("REDIS_HOST", "127.0.0.1")
REDIS_PORT = int(os.environ.get("REDIS_PORT", 6379))
REDIS_PASSWORD = os.environ.get("REDIS_PASSWORD", None)

USE_PROXY = os.environ.get("USE_PROXY", "0") == "1"
PROXY_HOST = os.environ.get("PROXY_HOST", "127.0.0.1")
PROXY_PORT = os.environ.get("PROXY_PORT", "7890")

STABLES = {'BUSDUSDT', 'USDCUSDT', 'TUSDUSDT', 'DAIUSDT',
           'FDUSDUSDT', 'USDPUSDT', 'EURUSDT', 'USTUSDT'}
LEV_KEYWORDS = ('UP', 'DOWN', 'BULL', 'BEAR')

# ---------- HTTP ----------
_session = requests.Session()
if USE_PROXY:
    proxy_url = f"http://{PROXY_HOST}:{PROXY_PORT}"
    _session.proxies = {"http": proxy_url, "https": proxy_url}


def _binance_get(path: str, params: dict = None,
                 max_retries: int = 3, retry_delay: float = 10.0):
    for attempt in range(max_retries):
        try:
            resp = _session.get(f"{BINANCE_FAPI}{path}",
                                params=params, timeout=15)
            if resp.status_code == 429:
                wait = retry_delay * (2 ** attempt)
                print(f"  [429] 限频，等待 {wait:.0f}s...")
                time.sleep(wait)
                continue
            resp.raise_for_status()
            return resp.json()
        except Exception as e:
            if attempt < max_retries - 1:
                time.sleep(retry_delay)
            else:
                raise
    return None


def _date_to_ms(date_str: str) -> int:
    dt = datetime.strptime(date_str, '%Y-%m-%d').replace(tzinfo=timezone.utc)
    return int(dt.timestamp() * 1000)


def _ms_to_date(ts_ms: int) -> str:
    return datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).strftime('%Y-%m-%d')


def _today_utc() -> str:
    return datetime.now(timezone.utc).strftime('%Y-%m-%d')


def _is_valid_symbol(symbol: str) -> bool:
    if symbol in STABLES:
        return False
    if not symbol.endswith('USDT'):
        return False
    base = symbol.replace('USDT', '')
    for kw in LEV_KEYWORDS:
        if base.endswith(kw):
            return False
    return True


# ---------- 日期工具 ----------

def build_expected_dates(days: int) -> List[str]:
    """生成过去 days 天的 UTC 日期列表（不含今天）"""
    today = datetime.now(timezone.utc).date()
    return [(today - timedelta(days=d)).strftime('%Y-%m-%d')
            for d in range(1, days + 1)]


def get_existing_dates(r: redis.Redis, symbol: str) -> Set[str]:
    """从 Redis 读取该币种已有的日期集合"""
    key = f"funding_rate:binance:{symbol}:1d"
    members = r.zrange(key, 0, -1)
    dates = set()
    for m in members:
        try:
            data = json.loads(m)
            dates.add(data['date'])
        except (json.JSONDecodeError, KeyError):
            pass
    return dates


def find_missing_dates(r: redis.Redis, symbol: str,
                       expected: List[str]) -> List[str]:
    """找出该币种缺失的日期"""
    existing = get_existing_dates(r, symbol)
    return [d for d in expected if d not in existing]


# ---------- Binance ----------

def get_all_symbols() -> List[str]:
    data = _binance_get("/fapi/v1/exchangeInfo")
    if not data:
        return []
    symbols = []
    for s in data.get('symbols', []):
        if (s.get('contractType') == 'PERPETUAL'
                and s.get('status') == 'TRADING'
                and s.get('quoteAsset') == 'USDT'
                and _is_valid_symbol(s['symbol'])):
            symbols.append(s['symbol'])
    return sorted(symbols)


def fetch_funding_for_dates(
    symbol: str, missing_dates: List[str]
) -> Tuple[str, Dict[str, float]]:
    """只拉缺失日期范围的 funding rate，按 UTC 日加总

    为减少 API 调用，取 missing_dates 的最小/最大日期作为拉取范围，
    然后只保留 missing_dates 中的日期。
    """
    if not missing_dates:
        return symbol, {}

    sorted_dates = sorted(missing_dates)
    start_ms = _date_to_ms(sorted_dates[0])
    end_ms = _date_to_ms(sorted_dates[-1]) + DAY_MS

    all_fr = []
    cur_start = start_ms
    while cur_start < end_ms:
        params = {
            'symbol': symbol,
            'startTime': cur_start,
            'endTime': end_ms,
            'limit': 1000,
        }
        data = _binance_get("/fapi/v1/fundingRate", params)
        if not data or len(data) == 0:
            break
        all_fr.extend(data)
        cur_start = int(data[-1]['fundingTime']) + 1
        if len(data) < 1000:
            break

    missing_set = set(missing_dates)
    today = _today_utc()
    day_fr: Dict[str, float] = {}
    for item in all_fr:
        dt = _ms_to_date(int(item['fundingTime']))
        if dt == today:
            continue
        if dt in missing_set:
            day_fr[dt] = day_fr.get(dt, 0.0) + float(item['fundingRate'])

    # 只保留当天有完整 3 笔结算的日期（每天 00:00/08:00/16:00）
    # 统计每天收到几笔
    day_count: Dict[str, int] = {}
    for item in all_fr:
        dt = _ms_to_date(int(item['fundingTime']))
        if dt in missing_set and dt != today:
            day_count[dt] = day_count.get(dt, 0) + 1

    # 过滤掉不足 3 笔的日期（可能是新上市当天只有 1-2 笔）
    incomplete = {d for d, c in day_count.items() if c < 3}
    for d in incomplete:
        day_fr.pop(d, None)

    return symbol, day_fr


# ---------- Redis 写入 ----------

def store_to_redis(r: redis.Redis, symbol: str,
                   day_fr: Dict[str, float]) -> int:
    if not day_fr:
        return 0

    key = f"funding_rate:binance:{symbol}:1d"
    pipe = r.pipeline()
    count = 0
    for date_str, fr_sum in day_fr.items():
        ts_ms = _date_to_ms(date_str)
        entry = json.dumps({
            "timestamp": ts_ms,
            "symbol": symbol,
            "funding_rate": fr_sum,
            "date": date_str,
        })
        pipe.zremrangebyscore(key, ts_ms, ts_ms)
        pipe.zadd(key, {entry: ts_ms})
        count += 1

    pipe.execute()
    return count


def trim_old_data(r: redis.Redis, symbol: str, days: int):
    """删除超过 days 天的旧数据，并刷新 TTL"""
    key = f"funding_rate:binance:{symbol}:1d"
    cutoff_ms = _date_to_ms(
        (datetime.now(timezone.utc).date() - timedelta(days=days))
        .strftime('%Y-%m-%d')
    )
    pipe = r.pipeline()
    pipe.zremrangebyscore(key, '-inf', cutoff_ms - 1)
    pipe.expire(key, EXPIRE_SECONDS)
    pipe.execute()


# ---------- 单币种处理 ----------

def process_one_symbol(
    r: redis.Redis, symbol: str,
    expected_dates: List[str], force: bool, days: int
) -> Dict:
    try:
        if force:
            missing = expected_dates
        else:
            missing = find_missing_dates(r, symbol, expected_dates)

        if not missing:
            trim_old_data(r, symbol, days)
            return {"symbol": symbol, "missing": 0,
                    "stored": 0, "error": None}

        sym, day_fr = fetch_funding_for_dates(symbol, missing)
        stored = store_to_redis(r, sym, day_fr)
        trim_old_data(r, symbol, days)

        return {"symbol": sym, "missing": len(missing),
                "stored": stored, "error": None}
    except Exception as e:
        return {"symbol": symbol, "missing": 0,
                "stored": 0, "error": str(e)}


# ---------- 验证 ----------

def verify_redis(r: redis.Redis, sample_size: int = 5):
    print("\n[验证] 抽样检查 Redis 数据:")
    keys = list(r.scan_iter(match="funding_rate:binance:*:1d", count=1000))
    print(f"  共 {len(keys)} 个 funding_rate key")

    today = _today_utc()
    for key in keys[:sample_size]:
        key_str = key.decode() if isinstance(key, bytes) else key
        count = r.zcard(key)
        newest = r.zrange(key, -1, -1)
        if newest:
            data = json.loads(newest[0])
            flag = " ⚠️ 含今天(不完整)" if data['date'] == today else ""
            print(f"  {key_str}: {count} 天 | "
                  f"最新: {data['date']} fr={data['funding_rate']:.6f}{flag}")


# ---------- Main ----------

def main():
    parser = argparse.ArgumentParser(
        description="Binance funding rate 增量补缺 + 清理")
    parser.add_argument("--days", type=int, default=MAX_DAYS,
                        help=f"保留天数 (默认: {MAX_DAYS})")
    parser.add_argument("--max-workers", type=int, default=5,
                        help="并发线程数 (默认: 5)")
    parser.add_argument("--force", action="store_true",
                        help="忽略已有数据，全量重拉")
    parser.add_argument("--use-proxy", action="store_true",
                        default=USE_PROXY, help="启用代理")
    args = parser.parse_args()

    today = _today_utc()
    print("=" * 60)
    print("  Funding Rate 增量补缺")
    print("=" * 60)
    print(f"当前 UTC:  {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"保留天数:  {args.days}")
    print(f"拉取范围:  不含今天 ({today})，只拉已完整结算的 UTC 日")
    print(f"模式:      {'全量重拉' if args.force else '增量补缺'}")
    print(f"并发:      {args.max_workers}")
    print("=" * 60)

    if args.use_proxy and not USE_PROXY:
        proxy_url = f"http://{PROXY_HOST}:{PROXY_PORT}"
        _session.proxies = {"http": proxy_url, "https": proxy_url}

    r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT,
                    password=REDIS_PASSWORD, decode_responses=True)
    try:
        r.ping()
        print("[Redis] 连接成功")
    except Exception as e:
        print(f"[Redis] 连接失败: {e}")
        sys.exit(1)

    expected_dates = build_expected_dates(args.days)
    print(f"[日期] 期望范围: {expected_dates[-1]} ~ {expected_dates[0]} "
          f"({len(expected_dates)} 天)")

    print("[Binance] 获取 USDT 永续合约列表...")
    symbols = get_all_symbols()
    if not symbols:
        print("[错误] 未获取到交易对")
        sys.exit(1)
    print(f"[Binance] 共 {len(symbols)} 个合约")

    success = 0
    failed = 0
    skipped = 0
    total_stored = 0
    total_missing = 0
    failed_list = []
    t0 = time.time()

    with ThreadPoolExecutor(max_workers=args.max_workers) as pool:
        futures = {
            pool.submit(process_one_symbol, r, sym,
                        expected_dates, args.force, args.days): sym
            for sym in symbols
        }
        for i, future in enumerate(as_completed(futures), 1):
            res = future.result()
            if res["error"]:
                failed += 1
                failed_list.append(f"{res['symbol']}: {res['error']}")
                if failed <= 5:
                    print(f"  [{i}/{len(symbols)}] ✗ {res['symbol']}: "
                          f"{res['error']}")
            elif res["missing"] == 0:
                skipped += 1
            else:
                success += 1
                total_stored += res["stored"]
                total_missing += res["missing"]

            if i % 50 == 0 or i == len(symbols):
                elapsed = time.time() - t0
                rate = i / elapsed if elapsed > 0 else 0
                print(f"  [{i}/{len(symbols)}] "
                      f"补缺 {success} | 跳过 {skipped} | 失败 {failed} | "
                      f"{rate:.1f}/s | {elapsed:.0f}s")

    elapsed = time.time() - t0
    print(f"\n{'=' * 60}")
    print(f"[完成] 总币种: {len(symbols)}")
    print(f"  需补缺: {success} 个币种, 共 {total_missing} 天缺失, "
          f"实际写入 {total_stored} 天")
    print(f"  已完整: {skipped} 个币种 (跳过)")
    print(f"  失败:   {failed} 个币种")
    print(f"  耗时:   {elapsed:.1f}s")

    if 0 < failed <= 20:
        print("\n[失败列表]")
        for fl in failed_list:
            print(f"  - {fl}")

    verify_redis(r, sample_size=5)

    print(f"\n{'=' * 60}")
    print("  完成")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    main()
