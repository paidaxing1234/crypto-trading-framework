#!/usr/bin/env python3
"""
快速K线补全脚本

核心优化:
1. redis-py pipeline 批量操作，替代 redis-cli 子进程
2. 按交易所分组：同一交易所串行请求（避免429），不同交易所并行
3. 只扫描最近N小时的1m K线缺失（默认12h），不全量扫描
4. 补全1m后立即聚合 5m/15m/30m/4h/8h
5. 429限流自动退避重试

用法:
    python fast_kline_filler.py                # 补全最近12小时
    python fast_kline_filler.py --hours 24     # 补全最近24小时
    python fast_kline_filler.py --loop         # 循环模式，每60秒跑一次
    python fast_kline_filler.py --loop --interval 30  # 循环模式，每30秒跑一次
"""

import os
import sys
import json
import time
import argparse
from collections import defaultdict
import requests
import redis
from datetime import datetime
from typing import List, Dict, Tuple, Optional, Set
from concurrent.futures import ThreadPoolExecutor, as_completed
from threading import Lock

# ==================== 配置 ====================

REDIS_HOST = os.getenv("REDIS_HOST", "127.0.0.1")
REDIS_PORT = int(os.getenv("REDIS_PORT", "6379"))

USE_PROXY = os.getenv("USE_PROXY", "0").lower() in ("1", "true", "yes", "on")
PROXY_HOST = os.getenv("PROXY_HOST", "127.0.0.1")
PROXY_PORT = int(os.getenv("PROXY_PORT", "7890"))

PROXIES = {
    "http": f"http://{PROXY_HOST}:{PROXY_PORT}",
    "https": f"http://{PROXY_HOST}:{PROXY_PORT}",
} if USE_PROXY else None

BINANCE_BATCH = 1500
OKX_BATCH = 100
REQUEST_TIMEOUT = 15

# 请求间隔 - 同一交易所内串行，间隔足够避免429/418
BINANCE_DELAY = 1.5   # Binance限流严格，1.5秒间隔
OKX_DELAY = 0.2

# 429/418 退避
MAX_RETRIES = 5
BACKOFF_BASE = 10     # 基础退避10秒，418时翻倍更长

AGGREGATION = {
    "5m":  ("1m", 5),
    "15m": ("1m", 15),
    "30m": ("1m", 30),
    "1h":  ("1m", 60),
    "4h":  ("1m", 240),
    "8h":  ("1m", 480),
}

INTERVAL_MS = {
    "1m": 60_000, "5m": 300_000, "15m": 900_000, "30m": 1_800_000,
    "1h": 3_600_000, "4h": 14_400_000, "8h": 28_800_000,
}

EXPIRE_1M_TO_30M = 60 * 86400
EXPIRE_1H = 180 * 86400

# ==================== 日志 ====================

_log_lock = Lock()

def log(msg, level="INFO"):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    with _log_lock:
        print(f"[{ts}] [{level}] {msg}", flush=True)

# ==================== Redis ====================

def get_redis():
    return redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)

def scan_all_1m_keys(r):
    symbols = []
    for key in r.scan_iter("kline:*:1m", count=1000):
        parts = key.split(":")
        if len(parts) >= 4:
            ex, sym = parts[1], parts[2]
            if (ex == "okx" and "-USDT-SWAP" in sym) or (ex == "binance" and sym.endswith("USDT")):
                symbols.append((ex, sym))
    return sorted(symbols)

def fetch_active_binance_symbols():
    """从 Binance exchangeInfo 获取当前在线的永续合约列表，用于过滤已下架币种"""
    for attempt in range(3):
        try:
            resp = requests.get("https://fapi.binance.com/fapi/v1/exchangeInfo",
                                timeout=10, proxies=PROXIES)
            if resp.status_code == 200:
                data = resp.json()
                active = set()
                for s in data.get("symbols", []):
                    if s.get("contractType") == "PERPETUAL" and s.get("status") == "TRADING":
                        active.add(s["symbol"])
                return active
            elif resp.status_code in (418, 429):
                wait = BACKOFF_BASE * (attempt + 1)
                log(f"[币种过滤] Binance exchangeInfo HTTP {resp.status_code}, 等{wait}s重试", "WARN")
                time.sleep(wait)
                continue
            else:
                log(f"[币种过滤] Binance exchangeInfo HTTP {resp.status_code}", "WARN")
                return None
        except Exception as e:
            log(f"[币种过滤] Binance exchangeInfo 异常: {e}", "WARN")
            if attempt < 2:
                time.sleep(5)
                continue
            return None
    return None

def detect_1m_gaps(r, exchange, symbol, start_ms, end_ms):
    key = f"kline:{exchange}:{symbol}:1m"
    raw = r.zrangebyscore(key, start_ms, end_ms, withscores=True)
    if not raw:
        return [(start_ms, end_ms)]

    existing = set(int(score) for _, score in raw)
    gaps = []
    gap_start = None
    ts = start_ms
    while ts <= end_ms:
        if ts not in existing:
            if gap_start is None:
                gap_start = ts
        else:
            if gap_start is not None:
                gaps.append((gap_start, ts - 60_000))
                gap_start = None
        ts += 60_000
    if gap_start is not None:
        gaps.append((gap_start, end_ms))
    return gaps

# ==================== 去重 ====================

def dedup_klines(r, exchange, symbol, interval, start_ms, end_ms):
    """
    去重指定时间范围内的K线数据。
    Redis Sorted Set中同一timestamp可能有多条不同JSON value，只保留一条。
    返回删除的重复条数。
    """
    key = f"kline:{exchange}:{symbol}:{interval}"
    raw = r.zrangebyscore(key, start_ms, end_ms, withscores=True)
    if not raw:
        return 0

    # 按score(timestamp)分组
    ts_groups = defaultdict(list)
    for val, score in raw:
        ts_groups[int(score)].append(val)

    removed = 0
    pipe = r.pipeline(transaction=False)
    for ts, vals in ts_groups.items():
        if len(vals) > 1:
            # 删除所有重复的，只保留第一条
            for dup_val in vals[1:]:
                pipe.zrem(key, dup_val)
                removed += 1
    if removed > 0:
        pipe.execute()
    return removed

def dedup_all_intervals(r, exchange, symbol, dedup_1m_start, dedup_agg_start, end_ms):
    """对所有周期执行去重：1m仅最近30min，聚合周期最近3天"""
    total = 0
    # 1m: 最近30分钟
    count = dedup_klines(r, exchange, symbol, "1m", dedup_1m_start, end_ms)
    if count > 0:
        log(f"  去重 {exchange}:{symbol}:1m 删除 {count} 条重复")
        total += count
    # 聚合周期: 最近3天
    for interval in AGGREGATION.keys():  # 5m, 15m, 30m, 1h, 4h, 8h
        count = dedup_klines(r, exchange, symbol, interval, dedup_agg_start, end_ms)
        if count > 0:
            log(f"  去重 {exchange}:{symbol}:{interval} 删除 {count} 条重复")
            total += count
    return total

# ==================== API（带429重试） ====================

def _req(session, url, params, symbol, exchange):
    for attempt in range(MAX_RETRIES):
        try:
            resp = session.get(url, params=params, timeout=REQUEST_TIMEOUT)
            if resp.status_code == 418:
                # 418 = IP被临时封禁，需要等更久
                wait = BACKOFF_BASE * (2 ** attempt) * 2  # 418等更久
                log(f"[{exchange}] 418 IP封禁 {symbol}, 等{wait}s重试 ({attempt+1}/{MAX_RETRIES})", "WARN")
                time.sleep(wait)
                continue
            if resp.status_code == 429:
                wait = BACKOFF_BASE * (2 ** attempt)
                log(f"[{exchange}] 429 限流 {symbol}, 等{wait}s重试 ({attempt+1}/{MAX_RETRIES})", "WARN")
                time.sleep(wait)
                continue
            resp.raise_for_status()
            return resp
        except requests.exceptions.HTTPError as e:
            status = getattr(e.response, 'status_code', 0) if e.response else 0
            if status in (429, 418):
                multiplier = 2 if status == 418 else 1
                wait = BACKOFF_BASE * (2 ** attempt) * multiplier
                log(f"[{exchange}] {status} {symbol}, 等{wait}s重试", "WARN")
                time.sleep(wait)
                continue
            log(f"[{exchange}] HTTP错误 {symbol}: {e}", "ERROR")
            return None
        except Exception as e:
            log(f"[{exchange}] 请求失败 {symbol}: {e}", "ERROR")
            return None
    log(f"[{exchange}] {symbol} 重试{MAX_RETRIES}次仍失败，跳过", "ERROR")
    return None

def fetch_binance_1m(session, symbol, start_ms, end_ms):
    url = "https://fapi.binance.com/fapi/v1/continuousKlines"
    klines = []
    cur = start_ms
    while cur <= end_ms:
        params = {"pair": symbol, "contractType": "PERPETUAL", "interval": "1m",
                  "startTime": cur, "endTime": end_ms, "limit": BINANCE_BATCH}
        resp = _req(session, url, params, symbol, "binance")
        if not resp:
            break
        data = resp.json()
        if not data:
            break
        for c in data:
            ts = int(c[0])
            if start_ms <= ts <= end_ms:
                klines.append({"timestamp": ts, "open": float(c[1]), "high": float(c[2]),
                               "low": float(c[3]), "close": float(c[4]), "volume": float(c[5]),
                               "amount": float(c[7]) if len(c) >= 11 else 0,
                               "buy_amount": float(c[10]) if len(c) >= 11 else 0,
                               "trades": int(c[8]) if len(c) >= 11 else 0})
        last_ts = int(data[-1][0])
        if last_ts >= end_ms:
            break
        cur = last_ts + 60_000
        time.sleep(BINANCE_DELAY)
    return klines

def fetch_okx_1m(session, symbol, start_ms, end_ms):
    url = "https://www.okx.com/api/v5/market/history-candles"
    klines = []
    cur_end = end_ms
    while cur_end >= start_ms:
        params = {"instId": symbol, "bar": "1m", "after": str(cur_end + 1), "limit": str(OKX_BATCH)}
        resp = _req(session, url, params, symbol, "okx")
        if not resp:
            break
        body = resp.json()
        if body.get("code") != "0":
            log(f"OKX API错误 {symbol}: {body.get('msg')}", "ERROR")
            break
        candles = body.get("data", [])
        if not candles:
            break
        for c in candles:
            ts = int(c[0])
            if start_ms <= ts <= end_ms:
                klines.append({"timestamp": ts, "open": float(c[1]), "high": float(c[2]),
                               "low": float(c[3]), "close": float(c[4]), "volume": float(c[5])})
        earliest = int(candles[-1][0])
        if earliest <= start_ms:
            break
        cur_end = earliest - 1
        time.sleep(OKX_DELAY)
    return klines

# ==================== Redis 写入 ====================

# Lua 脚本：原子化删旧+写新，避免 data_recorder 在查-删-写之间插入数据导致重复
# KEYS[1] = sorted set key
# ARGV[1] = expire seconds
# ARGV[2,3], [4,5], ... = 交替的 score, value 对
_LUA_ATOMIC_UPSERT = """
local key = KEYS[1]
local expire = tonumber(ARGV[1])
local count = 0
for i = 2, #ARGV, 2 do
    local score = ARGV[i]
    local value = ARGV[i + 1]
    local existing = redis.call('ZRANGEBYSCORE', key, score, score)
    for _, m in ipairs(existing) do
        redis.call('ZREM', key, m)
    end
    redis.call('ZADD', key, score, value)
    count = count + 1
end
if expire > 0 then
    redis.call('EXPIRE', key, expire)
end
return count
"""

def write_klines_pipeline(r, exchange, symbol, interval, klines):
    """
    原子写入K线到Redis。使用Lua脚本保证「删除同timestamp旧数据 + 写入新数据」的原子性，
    避免与 data_recorder 并行时的竞态重复。
    """
    if not klines:
        return 0
    key = f"kline:{exchange}:{symbol}:{interval}"
    expire_sec = EXPIRE_1H if interval == "1h" else EXPIRE_1M_TO_30M

    # 构建 Lua 脚本参数：expire, score1, value1, score2, value2, ...
    args = [expire_sec]
    for k in klines:
        value = json.dumps({"type": "kline", "exchange": exchange, "symbol": symbol,
                            "interval": interval, "timestamp": k["timestamp"],
                            "open": k["open"], "high": k["high"], "low": k["low"],
                            "close": k["close"], "volume": k["volume"],
                            "amount": k.get("amount", 0), "buy_amount": k.get("buy_amount", 0),
                            "trades": k.get("trades", 0)},
                           sort_keys=True, separators=(',', ':'))
        args.append(k["timestamp"])
        args.append(value)

    # 分批执行，每批最多500条，避免Lua脚本长时间阻塞Redis
    CHUNK = 500
    for i in range(0, len(klines), CHUNK):
        chunk_end = min(i + CHUNK, len(klines))
        # args 布局: [expire, s1, v1, s2, v2, ...] → 每条kline占2个arg
        chunk_args = [expire_sec] + args[1 + i * 2 : 1 + chunk_end * 2]
        r.eval(_LUA_ATOMIC_UPSERT, 1, key, *chunk_args)

    return len(klines)

# ==================== 聚合 ====================

def aggregate_and_write(r, exchange, symbol, target_interval, start_ms, end_ms):
    _, multiplier = AGGREGATION[target_interval]
    target_ms = INTERVAL_MS[target_interval]

    base_key = f"kline:{exchange}:{symbol}:1m"
    aligned_start = (start_ms // target_ms) * target_ms
    raw = r.zrangebyscore(base_key, aligned_start, end_ms, withscores=True)
    if not raw:
        return 0

    kline_map = {}
    for val, score in raw:
        ts = int(score)
        if ts not in kline_map:
            try:
                kline_map[ts] = json.loads(val)
            except:
                pass

    target_key = f"kline:{exchange}:{symbol}:{target_interval}"
    existing_raw = r.zrangebyscore(target_key, aligned_start, end_ms, withscores=True)
    existing_ts = set(int(score) for _, score in existing_raw)

    groups = {}
    for ts, kline in kline_map.items():
        aligned = (ts // target_ms) * target_ms
        if aligned not in groups:
            groups[aligned] = []
        groups[aligned].append(kline)

    new_klines = []
    for aligned_ts in sorted(groups.keys()):
        if aligned_ts in existing_ts:
            continue
        bars = groups[aligned_ts]
        dedup = {b["timestamp"]: b for b in bars}
        sorted_bars = sorted(dedup.values(), key=lambda x: x["timestamp"])
        if len(sorted_bars) >= multiplier:
            new_klines.append({
                "timestamp": aligned_ts,
                "open": sorted_bars[0]["open"],
                "close": sorted_bars[-1]["close"],
                "high": max(b["high"] for b in sorted_bars[:multiplier]),
                "low": min(b["low"] for b in sorted_bars[:multiplier]),
                "volume": sum(b["volume"] for b in sorted_bars[:multiplier]),
                "amount": sum(b.get("amount", 0) for b in sorted_bars[:multiplier]),
                "buy_amount": sum(b.get("buy_amount", 0) for b in sorted_bars[:multiplier]),
                "trades": sum(b.get("trades", 0) for b in sorted_bars[:multiplier]),
            })

    return write_klines_pipeline(r, exchange, symbol, target_interval, new_klines) if new_klines else 0

# ==================== 按交易所串行处理 ====================

def process_exchange_batch(exchange, symbols_with_gaps, start_ms, end_ms):
    """
    按交易所串行处理，参考 kline_gap_filler.cpp 的4步流程：
    步骤1: 去重1m基础K线
    步骤2: 补全1m缺失
    步骤3: 去重聚合周期的现有数据
    步骤4: 检测聚合周期缺失，从1m聚合生成
    """
    session = requests.Session()
    if PROXIES:
        session.proxies = PROXIES

    r = get_redis()
    total_filled = 0
    total_agg = {}

    for ex, symbol, _ in symbols_with_gaps:
        # 步骤1: 去重1m
        dedup_klines(r, ex, symbol, "1m", start_ms, end_ms)

        # 步骤2: 补全1m
        gaps = detect_1m_gaps(r, ex, symbol, start_ms, end_ms)
        filled = 0
        if gaps:
            for gs, ge in gaps:
                gs_str = datetime.fromtimestamp(gs / 1000).strftime('%m-%d %H:%M')
                ge_str = datetime.fromtimestamp(ge / 1000).strftime('%m-%d %H:%M')
                if ex == "binance":
                    klines = fetch_binance_1m(session, symbol, gs, ge)
                else:
                    klines = fetch_okx_1m(session, symbol, gs, ge)
                if klines:
                    cnt = write_klines_pipeline(r, ex, symbol, "1m", klines)
                    filled += cnt
                    log(f"    补全 {ex}:{symbol} 1m [{gs_str} ~ {ge_str}] {cnt}根")

            if filled > 0:
                total_filled += filled

        # 步骤3+4: 仅当实际补全了1m数据时才去重聚合周期并重新聚合（PERF-P6）
        agg_info = {}
        if filled > 0:
            for ti in AGGREGATION:
                dedup_klines(r, ex, symbol, ti, start_ms, end_ms)
            for ti in AGGREGATION:
                count = aggregate_and_write(r, ex, symbol, ti, start_ms, end_ms)
                if count > 0:
                    agg_info[ti] = count
                    total_agg[ti] = total_agg.get(ti, 0) + count

        if filled > 0 or agg_info:
            log(f"  ✓ {ex}:{symbol} 补全 {filled}根1m" + (f" | 聚合 {agg_info}" if agg_info else ""))

    return total_filled, total_agg

# ==================== 主流程 ====================

def run_once(hours=12):
    t0 = time.time()
    now_ms = (int(time.time() * 1000) // 60_000) * 60_000 - 60_000  # 排除当前未收盘的1m K线，避免与data_recorder重复
    start_ms = now_ms - hours * 3600_000

    log(f"开始快速补全 | 最近 {hours}h | "
        f"{datetime.fromtimestamp(start_ms/1000).strftime('%m-%d %H:%M')} ~ "
        f"{datetime.fromtimestamp(now_ms/1000).strftime('%m-%d %H:%M')}")

    r = get_redis()
    symbols = scan_all_1m_keys(r)
    log(f"扫描到 {len(symbols)} 个U本位合约")
    if not symbols:
        return

    # 过滤已下架的 Binance 合约
    active_binance = fetch_active_binance_symbols()
    if active_binance:
        delisted = [(ex, sym) for ex, sym in symbols if ex == "binance" and sym not in active_binance]
        if delisted:
            symbols = [(ex, sym) for ex, sym in symbols if not (ex == "binance" and sym not in active_binance)]
            log(f"[币种过滤] 过滤 {len(delisted)} 个已下架Binance合约，剩余 {len(symbols)} 个")
    else:
        log("[币种过滤] 未能获取Binance在线合约列表，跳过过滤")

    # 阶段0：并发去重（1m最近30min，聚合周期最近3天）
    dedup_1m_start_ms = now_ms - 30 * 60_000
    dedup_agg_start_ms = now_ms - 3 * 24 * 3600_000
    log("阶段0: 并发去重...")
    t_dedup = time.time()
    total_dedup = 0

    def dedup_task(args):
        ex, sym = args
        rl = get_redis()
        return dedup_all_intervals(rl, ex, sym, dedup_1m_start_ms, dedup_agg_start_ms, now_ms)

    with ThreadPoolExecutor(max_workers=16) as pool:
        for f in as_completed({pool.submit(dedup_task, s): s for s in symbols}):
            try:
                total_dedup += f.result()
            except Exception as e:
                log(f"去重失败: {e}", "ERROR")

    dedup_time = time.time() - t_dedup
    if total_dedup > 0:
        log(f"去重完成 ({dedup_time:.1f}s) | 共删除 {total_dedup} 条重复数据")
    else:
        log(f"去重完成 ({dedup_time:.1f}s) | 无重复数据")

    # 阶段1：并发检测缺失（纯Redis，高并发安全）
    log("阶段1: 并发检测1m缺失...")
    t1 = time.time()
    binance_gaps, okx_gaps = [], []

    def detect_task(args):
        ex, sym = args
        rl = get_redis()
        gaps = detect_1m_gaps(rl, ex, sym, start_ms, now_ms)
        return (ex, sym, sum((g[1]-g[0])//60_000+1 for g in gaps))

    with ThreadPoolExecutor(max_workers=16) as pool:
        for f in as_completed({pool.submit(detect_task, s): s for s in symbols}):
            try:
                ex, sym, total = f.result()
                if total > 0:
                    (binance_gaps if ex == "binance" else okx_gaps).append((ex, sym, total))
            except Exception as e:
                log(f"检测失败: {e}", "ERROR")

    binance_gaps.sort(key=lambda x: x[2])
    okx_gaps.sort(key=lambda x: x[2])

    detect_time = time.time() - t1
    total_missing = sum(t for _,_,t in binance_gaps) + sum(t for _,_,t in okx_gaps)
    n = len(binance_gaps) + len(okx_gaps)
    log(f"检测完成 ({detect_time:.1f}s) | {n}/{len(symbols)} 有1m缺失 | 共缺 {total_missing} 根1m")
    log(f"  Binance: {len(binance_gaps)} | OKX: {len(okx_gaps)}")

    # 构建仅有缺失的symbol列表（阶段0已做全量去重，阶段2只需处理有1m缺失的币种）
    # 阶段2：Binance和OKX并行，各自内部串行（补全1m + 聚合）
    log("阶段2: 按交易所并行处理（补全1m + 聚合其他周期）...")
    t2 = time.time()
    total_filled = 0
    total_agg = {}
    lock = Lock()

    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = []
        if binance_gaps:
            futures.append(pool.submit(process_exchange_batch, "binance", binance_gaps, start_ms, now_ms))
        if okx_gaps:
            futures.append(pool.submit(process_exchange_batch, "okx", okx_gaps, start_ms, now_ms))
        for f in as_completed(futures):
            try:
                filled, agg = f.result()
                with lock:
                    total_filled += filled
                    for k, v in agg.items():
                        total_agg[k] = total_agg.get(k, 0) + v
            except Exception as e:
                log(f"补全失败: {e}", "ERROR")

    elapsed = time.time() - t0
    log("=" * 60)
    log(f"补全完成 | 总耗时 {elapsed:.1f}s (去重 {dedup_time:.1f}s + 检测 {detect_time:.1f}s + 补全 {time.time()-t2:.1f}s)")
    if total_dedup > 0:
        log(f"  去重: {total_dedup} 条")
    log(f"  1m 补全: {total_filled} 根")
    if total_agg:
        log(f"  聚合: {', '.join(f'{k}: {v}根' for k,v in sorted(total_agg.items()))}")

def main():
    parser = argparse.ArgumentParser(description="快速K线补全")
    parser.add_argument("--hours", type=int, default=12, help="补全最近N小时 (默认12)")
    parser.add_argument("--loop", action="store_true", help="循环模式")
    parser.add_argument("--interval", type=int, default=60, help="循环间隔秒数 (默认60)")
    args = parser.parse_args()

    log(f"快速K线补全启动 | 范围={args.hours}h | 代理={PROXY_HOST}:{PROXY_PORT}")

    if args.loop:
        log(f"循环模式 | 间隔={args.interval}s | Ctrl+C 停止")
        count = 0
        while True:
            count += 1
            log(f"--- 第 {count} 轮 ---")
            try:
                run_once(args.hours)
            except Exception as e:
                log(f"运行出错: {e}", "ERROR")
            log(f"等待 {args.interval}s...")
            time.sleep(args.interval)
    else:
        run_once(args.hours)

if __name__ == "__main__":
    main()
