#!/usr/bin/env python3
"""从 data.binance.vision 拉 K 线 CSV 写入 Redis (多线程版)

用法:
    python3 refill_from_vision.py --symbol BTCUSDT --interval 1m --days 60
    python3 refill_from_vision.py --symbol ALL --interval 1m --days 60 --workers 8
    python3 refill_from_vision.py --interval ALL --days 60 --workers 8

数据源: https://data.binance.vision/data/futures/um/daily/klines/{symbol}/{interval}/{symbol}-{interval}-{date}.zip
"""
import argparse
import io
import json
import csv
import sys
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone, timedelta
from typing import List, Dict

import redis
import requests

REDIS_HOST = "127.0.0.1"
REDIS_PORT = 6379
BASE_URL = "https://data.binance.vision/data/futures/um/daily/klines"

# 各 interval 默认保留天数 (UTC+0 日界)
DEFAULT_DAYS = {
    "1m": 60,
    "5m": 60,
    "15m": 60,
    "30m": 60,
    "1h": 180,
    "4h": 60,
    "8h": 60,
    "1d": 120,
}

# 全市场币种获取 (来自 binance fapi exchangeInfo)
EXCHANGE_INFO_URL = "https://fapi.binance.com/fapi/v1/exchangeInfo"


def get_redis():
    return redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)


def get_all_usdt_perp_symbols() -> List[str]:
    """从 Binance fapi 获取全部 USDT 永续合约币种"""
    resp = requests.get(EXCHANGE_INFO_URL, timeout=30)
    resp.raise_for_status()
    data = resp.json()
    symbols = []
    for s in data.get("symbols", []):
        if (s.get("contractType") == "PERPETUAL"
            and s.get("quoteAsset") == "USDT"
            and s.get("status") == "TRADING"):
            symbols.append(s["symbol"])
    return sorted(symbols)


def download_one_zip(symbol: str, interval: str, date_str: str, session: requests.Session) -> List[Dict]:
    """下载一天的 zip, 解压 CSV, 返回 kline list"""
    url = f"{BASE_URL}/{symbol}/{interval}/{symbol}-{interval}-{date_str}.zip"
    try:
        resp = session.get(url, timeout=30)
        if resp.status_code == 404:
            return []
        resp.raise_for_status()
    except Exception:
        return []

    try:
        with zipfile.ZipFile(io.BytesIO(resp.content)) as zf:
            names = zf.namelist()
            if not names:
                return []
            with zf.open(names[0]) as f:
                reader = csv.reader(io.TextIOWrapper(f, encoding="utf-8"))
                klines = []
                for row in reader:
                    if len(row) < 11:
                        continue
                    # binance.vision 有时第一行是 header
                    if row[0] == "open_time":
                        continue
                    try:
                        klines.append({
                            "timestamp": int(row[0]),
                            "open": float(row[1]),
                            "high": float(row[2]),
                            "low": float(row[3]),
                            "close": float(row[4]),
                            "volume": float(row[5]),
                            "amount": float(row[7]),
                            "trades": int(row[8]),
                            "buy_amount": float(row[10]),
                            "symbol": symbol,
                            "exchange": "binance",
                            "interval": interval,
                            "type": "kline",
                        })
                    except (ValueError, IndexError):
                        continue
                return klines
    except zipfile.BadZipFile:
        return []


def fetch_symbol(symbol: str, interval: str, days: int, session: requests.Session) -> Dict:
    """拉一个币种一个周期的所有 zip, 返回所有 kline"""
    now = datetime.now(timezone.utc)
    dates = []
    for i in range(1, days + 1):
        d = now - timedelta(days=i)
        dates.append(d.strftime("%Y-%m-%d"))
    dates.reverse()

    all_klines = []
    for date_str in dates:
        klines = download_one_zip(symbol, interval, date_str, session)
        all_klines.extend(klines)
    return {"symbol": symbol, "interval": interval, "klines": all_klines}


def write_to_redis(r: redis.Redis, symbol: str, interval: str, klines: List[Dict]) -> int:
    """批量写入 Redis ZSET, 返回写入条数"""
    if not klines:
        return 0
    key = f"kline:binance:{symbol}:{interval}"
    pipe = r.pipeline()
    for k in klines:
        value = json.dumps(k, separators=(",", ":"), sort_keys=True)
        pipe.zadd(key, {value: k["timestamp"]})
    pipe.execute()
    return len(klines)


def refill_all_symbols(r: redis.Redis, symbols: List[str], interval: str, days: int, workers: int):
    """流式拉取: 用 Queue 控制内存, 每个币下载完立即写 Redis 立即释放

    关键设计:
    - workers 个下载线程 (生产者): 一个币下载完 → 立即 put 到队列 → 释放
    - 1 个主线程 (消费者): 从队列拿 → 写 Redis → 释放
    - 队列容量 = workers (= 8), 同一时刻内存中最多 8 个币的 kline list

    内存上限 = 8 个币 × 86400 行 × ~500 bytes ≈ 350 MB (单进程, 无堆积)
    """
    import queue, threading

    n = len(symbols)
    print(f"  开始: {n} 币 × {days} 天 ({interval}), workers={workers} (流式模式, 无堆积)")
    t0 = time.time()

    # 计算日期列表 (复用)
    now = datetime.now(timezone.utc)
    date_strs = []
    for i in range(1, days + 1):
        d = now - timedelta(days=i)
        date_strs.append(d.strftime("%Y-%m-%d"))
    date_strs.reverse()

    # 队列容量 = workers, 限制同时驻留内存的币数
    result_queue: "queue.Queue" = queue.Queue(maxsize=workers)
    sentinel = object()

    # 共享 session (复用 TCP 连接)
    session = requests.Session()
    adapter = requests.adapters.HTTPAdapter(pool_connections=workers, pool_maxsize=workers * 2)
    session.mount("https://", adapter)

    # 任务队列: 待下载币种
    work_queue: "queue.Queue" = queue.Queue()
    for sym in symbols:
        work_queue.put(sym)
    for _ in range(workers):
        work_queue.put(None)  # poison pill: 每个 worker 一个停止信号

    download_failed: List[str] = []
    fail_lock = threading.Lock()

    def worker_download():
        while True:
            sym = work_queue.get()
            if sym is None:
                work_queue.task_done()
                break
            try:
                # 下载该币所有日期 (按顺序累积一个币的 list, 单币 ~30 MB)
                klines = []
                for date_str in date_strs:
                    klines.extend(download_one_zip(sym, interval, date_str, session))
                if klines:
                    # 阻塞 put: 队列满了就等主线程消费
                    result_queue.put((sym, klines))
                else:
                    with fail_lock:
                        download_failed.append(sym)
                    result_queue.put((sym, None))  # 占位, 让主线程也计数
            except Exception as e:
                with fail_lock:
                    download_failed.append(sym)
                print(f"    [DOWNLOAD ERROR] {sym}: {e}")
                result_queue.put((sym, None))
            finally:
                work_queue.task_done()
        # worker 退出时也送一个哨兵给主线程
        result_queue.put(sentinel)

    # 启动 workers
    threads = []
    for _ in range(workers):
        t = threading.Thread(target=worker_download, daemon=True)
        t.start()
        threads.append(t)

    # 主线程: 消费, 写 Redis
    done = 0
    total_klines = 0
    sentinels_seen = 0
    while sentinels_seen < workers:
        item = result_queue.get()
        if item is sentinel:
            sentinels_seen += 1
            continue
        sym, klines = item
        if klines:
            try:
                total_klines += write_to_redis(r, sym, interval, klines)
            except Exception as e:
                print(f"    [REDIS ERROR] {sym}: {e}")
                with fail_lock:
                    download_failed.append(sym)
        # 消费完, klines 立即释放
        del klines
        done += 1
        elapsed = time.time() - t0
        speed = done / elapsed if elapsed > 0 else 0
        eta = (n - done) / speed if speed > 0 else 0
        if done % 20 == 0 or done == n:
            print(f"  [{done}/{n}] {interval} | total_rows={total_klines:,} | "
                  f"{speed:.1f} sym/s | ETA {eta:.0f}s | failed={len(download_failed)}")

    # 等所有线程退出
    for t in threads:
        t.join(timeout=5)

    print(f"  ✓ {interval}: {n} 币, {total_klines:,} 行, {time.time()-t0:.1f}s, failed={len(download_failed)}")
    if download_failed:
        print(f"    失败币种 (前10): {download_failed[:10]}")
    return total_klines, download_failed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbol", default="ALL", help="币种 BTCUSDT 或 ALL")
    parser.add_argument("--interval", default="ALL", help="周期: 1m, 1h, 1d, ALL")
    parser.add_argument("--days", type=int, default=0, help="天数 (0=用默认值)")
    parser.add_argument("--workers", type=int, default=8, help="并发线程数")
    args = parser.parse_args()

    r = get_redis()
    r.ping()
    print(f"Redis 连接成功 ({REDIS_HOST}:{REDIS_PORT})")

    # 解析币种
    if args.symbol.upper() == "ALL":
        print(f"获取 Binance USDT 永续合约列表...")
        symbols = get_all_usdt_perp_symbols()
        print(f"  共 {len(symbols)} 个 USDT 永续币")
    else:
        symbols = [args.symbol.upper()]

    # 解析 interval
    if args.interval.upper() == "ALL":
        intervals = ["1m", "5m", "15m", "30m", "1h", "4h", "8h", "1d"]
    else:
        intervals = [args.interval]

    print(f"参数: workers={args.workers}, intervals={intervals}, n_symbols={len(symbols)}")
    print()

    overall_t0 = time.time()
    grand_total = 0
    for interval in intervals:
        days = args.days or DEFAULT_DAYS.get(interval, 60)
        print(f"=== {interval} ({days} 天) ===")
        rows, failed = refill_all_symbols(r, symbols, interval, days, args.workers)
        grand_total += rows

    print()
    print(f"=== 全部完成 ===")
    print(f"  总行数: {grand_total:,}")
    print(f"  总耗时: {time.time()-overall_t0:.1f}s")
    print()
    print(f"  Redis 内存:")
    info = r.info("memory")
    print(f"    used_memory_human = {info.get('used_memory_human')}")


if __name__ == "__main__":
    main()
