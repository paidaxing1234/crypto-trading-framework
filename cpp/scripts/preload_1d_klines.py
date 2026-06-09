#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
预加载 120 天 1d K 线到 Redis

为 ApolloFund 等日频策略一次性拉满 Binance + OKX 所有 USDT 永续合约的 120 天 1d K线。
拉完后 data_recorder 会自动从 1m 聚合维护 1d K线。

功能:
    - 自动发现 Binance / OKX 全市场 USDT 永续合约
    - 拉取过去 120 天 1d K 线
    - 写入 Redis (与 data_recorder 完全一致的 ZSET 格式)
    - 写入后自动 ZREMRANGEBYRANK 截断到 120 条 (Redis 始终只有 120 天)
    - 设置 4 个月过期时间 (与 data_recorder 配置一致)

使用方法:
    python preload_1d_klines.py                       # 默认 binance + okx, 120 天
    python preload_1d_klines.py --exchange binance    # 仅 Binance
    python preload_1d_klines.py --exchange okx        # 仅 OKX
    python preload_1d_klines.py --days 120            # 自定义天数

环境变量:
    REDIS_HOST       Redis 主机 (默认 127.0.0.1)
    REDIS_PORT       Redis 端口 (默认 6379)
    USE_PROXY        是否走代理 (默认 0)
    PROXY_HOST       代理主机 (默认 127.0.0.1)
    PROXY_PORT       代理端口 (默认 7890)
"""

import os
import sys
import time
import json
import argparse
from datetime import datetime
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List, Dict, Optional

# 复用 preload_klines_to_redis.py 中的加载器和存储类
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

from preload_klines_to_redis import (
    BinanceKlineLoader, OKXKlineLoader, RedisKlineStorage,
    interval_to_ms, BaseKlineLoader,
    REDIS_HOST, REDIS_PORT, REDIS_PASSWORD,
    DEFAULT_USE_PROXY,
)

# 与 data_recorder.cpp 完全一致
MAX_KLINES_1D = 120              # Redis 中保留的最大 1d K 线条数
EXPIRE_4_MONTHS = 120 * 24 * 60 * 60  # 4 个月过期

DAY_MS = 86400 * 1000


def fetch_and_store_one(
    loader: BaseKlineLoader,
    storage: RedisKlineStorage,
    symbol: str,
    days: int,
) -> Dict:
    """拉取并存储一个 symbol 的 1d K线（UTC 0:00 日界 / 仅保留已收盘）

    - Binance: API 1d 默认 UTC 0 对齐, 直接拉
    - OKX:     用 1Dutc bar (强制 UTC 0 对齐, 默认 1D 是北京日界)
    收盘判定: ts + 86400000 <= now_ms (跳过当日还在滚动的)
    """
    interval = "1d"
    interval_ms = interval_to_ms(interval)
    now_ms = int(time.time() * 1000)
    end_time = now_ms
    start_time = now_ms - (days + 1) * interval_ms

    try:
        klines = loader.get_klines(
            symbol=symbol, interval=interval,
            start_time=start_time, end_time=end_time,
            limit=loader.batch_size,
        )
    except Exception as e:
        return {"symbol": symbol, "fetched": 0, "stored": 0, "error": str(e)}

    if not klines:
        return {"symbol": symbol, "fetched": 0, "stored": 0, "error": "no data"}

    # 过滤掉还在滚动的 K线（ts + interval_ms <= now_ms 才算已收盘）
    closed_klines = [k for k in klines if k["timestamp"] + interval_ms <= now_ms]

    if not closed_klines:
        return {"symbol": symbol, "fetched": len(klines), "stored": 0,
                "error": "no closed kline"}

    stored = storage.store_klines(symbol, interval, loader.exchange_name, closed_klines)

    # 截断到 MAX_KLINES_1D 条 + 设置过期时间（与 data_recorder 一致）
    key = f"kline:{loader.exchange_name}:{symbol}:{interval}"
    try:
        pipe = storage.client.pipeline()
        # ZREMRANGEBYRANK 0 -(MAX+1)：保留最新的 MAX 条
        pipe.zremrangebyrank(key, 0, -(MAX_KLINES_1D + 1))
        pipe.expire(key, EXPIRE_4_MONTHS)
        pipe.execute()
    except Exception as e:
        return {"symbol": symbol, "fetched": len(klines), "stored": stored,
                "error": f"trim failed: {e}"}

    return {"symbol": symbol, "fetched": len(klines), "stored": stored, "error": None}


def preload_exchange(
    loader: BaseKlineLoader,
    storage: RedisKlineStorage,
    days: int,
    max_workers: int = 5,
) -> Dict:
    """对一个交易所的所有 USDT 永续合约进行预加载"""
    exchange = loader.exchange_name
    print(f"\n{'='*60}")
    print(f"  [{exchange.upper()}] 开始预加载 1d K 线 ({days} 天)")
    print(f"{'='*60}")

    # 1. 获取交易对列表
    print(f"[{exchange.upper()}] 获取 USDT 永续合约列表...")
    symbols = loader.get_exchange_info()
    if not symbols:
        print(f"[{exchange.upper()}] 未获取到交易对列表，跳过")
        return {"exchange": exchange, "total": 0, "success": 0, "failed": 0}

    # OKX 只拉 USDT 永续，过滤 USDC 等
    if exchange == "okx":
        symbols = [s for s in symbols if s.endswith("-USDT-SWAP")]
    else:
        # Binance 只拉 USDT 结尾，过滤 BUSD/USDC 等
        symbols = [s for s in symbols if s.endswith("USDT")]

    print(f"[{exchange.upper()}] 共 {len(symbols)} 个 USDT 永续合约")

    # 2. 并发拉取
    success = 0
    failed = 0
    failed_list: List[str] = []
    t0 = time.time()

    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {
            pool.submit(fetch_and_store_one, loader, storage, sym, days): sym
            for sym in symbols
        }

        for i, future in enumerate(as_completed(futures), 1):
            res = future.result()
            sym = res["symbol"]
            if res["error"]:
                failed += 1
                failed_list.append(f"{sym}: {res['error']}")
                if failed <= 5:
                    print(f"  [{i}/{len(symbols)}] ✗ {sym}: {res['error']}")
            else:
                success += 1
                if i % 50 == 0 or i == len(symbols):
                    elapsed = time.time() - t0
                    rate = i / elapsed if elapsed > 0 else 0
                    print(f"  [{i}/{len(symbols)}] ✓ 进度 | "
                          f"成功 {success} | 失败 {failed} | "
                          f"速率 {rate:.1f}/s | 耗时 {elapsed:.0f}s")

    elapsed = time.time() - t0
    print(f"\n[{exchange.upper()}] 完成: "
          f"成功 {success}/{len(symbols)} | 失败 {failed} | 耗时 {elapsed:.1f}s")

    if failed > 0 and failed <= 20:
        print(f"[{exchange.upper()}] 失败列表:")
        for fl in failed_list:
            print(f"  - {fl}")

    return {
        "exchange": exchange,
        "total": len(symbols),
        "success": success,
        "failed": failed,
    }


def verify_redis_data(storage: RedisKlineStorage, exchange: str, sample_size: int = 5):
    """抽样验证 Redis 中数据"""
    print(f"\n[{exchange.upper()}] 抽样验证 Redis 数据:")
    pattern = f"kline:{exchange}:*:1d"
    try:
        keys = list(storage.client.scan_iter(match=pattern, count=1000))
    except Exception as e:
        print(f"  扫描失败: {e}")
        return

    if not keys:
        print(f"  未找到任何 {pattern} key")
        return

    print(f"  共 {len(keys)} 个 1d K线 key")
    sample = keys[:sample_size]
    for key in sample:
        try:
            count = storage.client.zcard(key)
            ttl = storage.client.ttl(key)
            oldest = storage.client.zrange(key, 0, 0, withscores=True)
            newest = storage.client.zrange(key, -1, -1, withscores=True)
            if oldest and newest:
                old_dt = datetime.utcfromtimestamp(int(oldest[0][1]) / 1000).strftime('%Y-%m-%d')
                new_dt = datetime.utcfromtimestamp(int(newest[0][1]) / 1000).strftime('%Y-%m-%d')
                print(f"  {key}: {count} 条 | TTL {ttl//86400}天 | "
                      f"{old_dt} ~ {new_dt}")
        except Exception as e:
            print(f"  {key}: 读取失败 {e}")


def main():
    parser = argparse.ArgumentParser(description="预加载 120 天 1d K 线到 Redis")
    parser.add_argument("--exchange", default="all",
                        choices=["binance", "okx", "all"],
                        help="交易所 (默认: all)")
    parser.add_argument("--days", type=int, default=MAX_KLINES_1D,
                        help=f"拉取天数 (默认: {MAX_KLINES_1D})")
    parser.add_argument("--testnet", action="store_true",
                        help="使用测试网")
    parser.add_argument("--max-workers", type=int, default=5,
                        help="并发线程数 (默认: 5)")
    parser.add_argument("--use-proxy", action="store_true",
                        default=DEFAULT_USE_PROXY,
                        help="启用代理")
    args = parser.parse_args()

    # 截断到 120 (与 data_recorder 一致，避免后续被截掉浪费)
    if args.days > MAX_KLINES_1D:
        print(f"[警告] --days={args.days} > {MAX_KLINES_1D}，"
              f"Redis 会自动截断到 {MAX_KLINES_1D} 条，建议保持 {MAX_KLINES_1D}")

    print("=" * 60)
    print("  1d K 线预加载脚本")
    print("=" * 60)
    print(f"交易所: {args.exchange}")
    print(f"天数:   {args.days}")
    print(f"代理:   {args.use_proxy}")
    print(f"并发:   {args.max_workers}")
    print("=" * 60)

    # 连接 Redis
    storage = RedisKlineStorage()
    if not storage.connect():
        print("[错误] 无法连接 Redis，退出")
        sys.exit(1)

    # 选择加载器
    results = []
    t_start = time.time()

    if args.exchange in ("binance", "all"):
        loader = BinanceKlineLoader(testnet=args.testnet, use_proxy=args.use_proxy)
        results.append(preload_exchange(loader, storage, args.days, args.max_workers))

    if args.exchange in ("okx", "all"):
        loader = OKXKlineLoader(testnet=args.testnet, use_proxy=args.use_proxy)
        results.append(preload_exchange(loader, storage, args.days, args.max_workers))

    # 验证
    for r in results:
        verify_redis_data(storage, r["exchange"])

    # 总结
    total_elapsed = time.time() - t_start
    print(f"\n{'='*60}")
    print(f"  全部完成 | 总耗时 {total_elapsed:.1f}s")
    print(f"{'='*60}")
    for r in results:
        print(f"  [{r['exchange'].upper()}] "
              f"成功 {r['success']}/{r['total']} | 失败 {r['failed']}")
    print(f"{'='*60}")

    storage.disconnect()


if __name__ == "__main__":
    main()
