#!/usr/bin/env python3
"""
从Redis中已有的1m K线重新聚合生成 5m/15m/30m K线
包含 amount 和 buy_amount 字段
"""

import json
import sys
import redis
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

REDIS_HOST = "localhost"
REDIS_PORT = 6379
REDIS_DB = 0

TARGET_INTERVALS = ["5m", "15m", "30m"]
INTERVAL_MULTIPLIER = {"5m": 5, "15m": 15, "30m": 30}


def get_redis():
    return redis.Redis(host=REDIS_HOST, port=REDIS_PORT, db=REDIS_DB, decode_responses=True)


def get_symbols(r, exchange):
    """获取所有有1m数据的symbol"""
    keys = r.keys(f"kline:{exchange}:*:1m")
    symbols = []
    for k in keys:
        parts = k.split(":")
        if len(parts) == 4:
            symbols.append(parts[2])
    return sorted(symbols)


def aggregate_symbol(symbol, exchange):
    """对单个symbol从1m聚合生成5m/15m/30m"""
    r = get_redis()
    source_key = f"kline:{exchange}:{symbol}:1m"

    # 读取所有1m K线
    raw = r.zrangebyscore(source_key, "-inf", "+inf")
    if not raw:
        return symbol, {}

    klines_1m = []
    for item in raw:
        try:
            klines_1m.append(json.loads(item))
        except json.JSONDecodeError:
            continue

    if not klines_1m:
        return symbol, {}

    results = {}
    for interval in TARGET_INTERVALS:
        multiplier = INTERVAL_MULTIPLIER[interval]
        interval_ms = multiplier * 60 * 1000
        target_key = f"kline:{exchange}:{symbol}:{interval}"

        aggregated = {}
        for kline in klines_1m:
            ts = kline['timestamp']
            aligned_ts = (ts // interval_ms) * interval_ms

            if aligned_ts not in aggregated:
                aggregated[aligned_ts] = {
                    'timestamp': aligned_ts,
                    'open': kline['open'],
                    'high': kline['high'],
                    'low': kline['low'],
                    'close': kline['close'],
                    'volume': kline['volume'],
                    'amount': kline.get('amount', 0.0),
                    'buy_amount': kline.get('buy_amount', 0.0),
                    'symbol': symbol,
                    'exchange': exchange,
                    'interval': interval,
                    'type': 'kline'
                }
            else:
                agg = aggregated[aligned_ts]
                agg['high'] = max(agg['high'], kline['high'])
                agg['low'] = min(agg['low'], kline['low'])
                agg['close'] = kline['close']
                agg['volume'] += kline['volume']
                agg['amount'] += kline.get('amount', 0.0)
                agg['buy_amount'] += kline.get('buy_amount', 0.0)

        if aggregated:
            pipe = r.pipeline()
            for ts, kl in aggregated.items():
                pipe.zadd(target_key, {json.dumps(kl): ts})
            pipe.execute()

        results[interval] = len(aggregated)

    return symbol, results


def main():
    exchange = sys.argv[1] if len(sys.argv) > 1 else "binance"
    workers = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    r = get_redis()
    symbols = get_symbols(r, exchange)
    print(f"[{exchange}] 找到 {len(symbols)} 个symbol，开始聚合 {TARGET_INTERVALS}...")
    print(f"并发数: {workers}")

    start = time.time()
    done = 0
    total_bars = {iv: 0 for iv in TARGET_INTERVALS}

    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {executor.submit(aggregate_symbol, sym, exchange): sym for sym in symbols}
        for future in as_completed(futures):
            sym, results = future.result()
            done += 1
            for iv, count in results.items():
                total_bars[iv] += count
            if done % 50 == 0 or done == len(symbols):
                elapsed = time.time() - start
                print(f"  进度: {done}/{len(symbols)} ({elapsed:.1f}s)")

    elapsed = time.time() - start
    print(f"\n完成! 耗时 {elapsed:.1f}s")
    for iv in TARGET_INTERVALS:
        print(f"  {iv}: {total_bars[iv]} 根K线")


if __name__ == "__main__":
    main()
