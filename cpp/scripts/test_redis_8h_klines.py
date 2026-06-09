#!/usr/bin/env python3
"""
诊断脚本：实时高频监测 Redis 中 8h K线数据变化
使用 Lua 脚本在 Redis 服务端批量取 score，单轮 <1ms
"""

import redis
import json
import time
import os
import logging
from datetime import datetime

REDIS_HOST = "127.0.0.1"
REDIS_PORT = 6379
REDIS_PASSWORD = ""

EXCHANGE = "binance"
INTERVAL = "8h"

TEST_SYMBOLS = ["BTCUSDT", "ETHUSDT", "SOLUSDT", "XRPUSDT", "DOGEUSDT"]

# 日志配置
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
STRATEGIES_DIR = os.path.join(os.path.dirname(SCRIPT_DIR), "strategies")
LOG_DIR = os.path.join(STRATEGIES_DIR, "logs")
os.makedirs(LOG_DIR, exist_ok=True)
LOG_FILE = os.path.join(LOG_DIR, f"redis_8h_monitor_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log")

logger = logging.getLogger("redis_8h_monitor")
logger.setLevel(logging.INFO)
# 文件handler
fh = logging.FileHandler(LOG_FILE, encoding="utf-8")
fh.setFormatter(logging.Formatter("%(asctime)s %(message)s", datefmt="%Y-%m-%d %H:%M:%S"))
logger.addHandler(fh)
# 终端handler
sh = logging.StreamHandler()
sh.setFormatter(logging.Formatter("%(message)s"))
logger.addHandler(sh)

# Lua脚本：批量取每个key的最新score（时间戳），不取value，避免JSON传输
LUA_BATCH_SCORES = """
local res = {}
for i, key in ipairs(KEYS) do
    local r = redis.call('ZREVRANGE', key, 0, 0, 'WITHSCORES')
    if r and #r >= 2 then
        res[i] = tonumber(r[2])
    else
        res[i] = -1
    end
end
return res
"""


def main():
    logger.info("=" * 60)
    logger.info("  Redis 8h K线 实时监测 (Lua脚本模式)")
    logger.info(f"  日志文件: {LOG_FILE}")
    logger.info("=" * 60)

    try:
        r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, password=REDIS_PASSWORD or None, decode_responses=True)
        r.ping()
        logger.info(f"[OK] Redis 连接成功 ({REDIS_HOST}:{REDIS_PORT})")
    except Exception as e:
        logger.error(f"[FAIL] Redis 连接失败: {e}")
        return

    # 注册Lua脚本
    lua_script = r.register_script(LUA_BATCH_SCORES)

    # 1. 初始快照
    logger.info(f"\n--- 初始状态快照 ---")
    rebalance_interval_ms = 8 * 60 * 60 * 1000
    now_ms = int(time.time() * 1000)
    current_period = now_ms // rebalance_interval_ms

    pattern = f"kline:{EXCHANGE}:*:{INTERVAL}"
    keys = list(r.scan_iter(match=pattern, count=1000))
    logger.info(f"找到 {len(keys)} 个 8h K线 key")
    logger.info(f"当前时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    logger.info(f"当前8h周期: {current_period}")

    if not keys:
        all_kline_keys = list(r.scan_iter(match=f"kline:{EXCHANGE}:*", count=100))
        if all_kline_keys:
            intervals_found = set()
            for k in all_kline_keys[:50]:
                parts = k.split(":")
                if len(parts) >= 4:
                    intervals_found.add(parts[3])
            logger.info(f"[INFO] Redis 中存在的K线周期: {intervals_found}")
        else:
            logger.info("[WARN] Redis 中没有任何 kline 数据")
        return

    # 提取symbol和key映射
    key_to_symbol = {}
    for key in keys:
        parts = key.split(":")
        if len(parts) >= 4:
            key_to_symbol[key] = parts[2]

    # Lua批量取初始score
    scores = lua_script(keys=keys)
    baseline = {}
    for i, key in enumerate(keys):
        ts = int(scores[i]) if scores[i] and int(scores[i]) > 0 else 0
        baseline[key] = ts

    # 打印测试币种
    for symbol in TEST_SYMBOLS:
        key = f"kline:{EXCHANGE}:{symbol}:{INTERVAL}"
        ts = baseline.get(key, 0)
        if ts == 0:
            logger.info(f"  {symbol}: 无数据")
            continue
        age_hours = (now_ms - ts) / 3600000
        ts_str = datetime.fromtimestamp(ts / 1000).strftime('%Y-%m-%d %H:%M:%S')
        logger.info(f"  {symbol}: 最新: {ts_str} ({age_hours:.1f}h前)")

    # 初始同步率 - 找Redis里最新的K线周期，看多少币种到了这个周期
    # 这和策略调仓判断逻辑一致：new_kline_ts // interval -> current_period -> 统计arrived
    valid_ts = [ts for ts in baseline.values() if ts > 0]
    total = len(baseline)
    if valid_ts:
        max_period = max(ts // rebalance_interval_ms for ts in valid_ts)
        max_period_time = datetime.fromtimestamp(max_period * rebalance_interval_ms / 1000).strftime('%Y-%m-%d %H:%M')
        in_max_period = sum(1 for ts in valid_ts if ts // rebalance_interval_ms >= max_period)
        logger.info(f"\n  最新K线周期: {max_period} (开盘: {max_period_time})")
        logger.info(f"  同步率: {in_max_period}/{total} ({in_max_period/total*100:.1f}%) — >=80%时策略触发调仓")

    # 2. 实时Lua监测
    logger.info(f"\n--- 实时监测中 (Lua脚本模式, Ctrl+C 退出) ---\n")

    poll_count = 0
    change_count = 0
    last_status_time = time.time()

    try:
        while True:
            poll_start = time.time()

            # Lua脚本一次调用取所有score
            scores = lua_script(keys=keys)

            poll_elapsed_ms = (time.time() - poll_start) * 1000

            changes_this_round = []
            for i, key in enumerate(keys):
                latest_ts = int(scores[i]) if scores[i] and int(scores[i]) > 0 else 0
                old_ts = baseline.get(key, 0)

                if latest_ts > old_ts:
                    symbol = key_to_symbol.get(key, "?")
                    # K线timestamp是开盘时间，收盘时间 = 开盘时间 + 8h
                    close_time = latest_ts + rebalance_interval_ms
                    # 延迟 = 现在 - 收盘时间（正数=收盘后多久检测到，负数=还没收盘）
                    delay_ms = int(time.time() * 1000) - close_time
                    changes_this_round.append({
                        "symbol": symbol,
                        "ts": latest_ts,
                        "time": datetime.fromtimestamp(latest_ts / 1000).strftime('%Y-%m-%d %H:%M:%S'),
                        "detect_time": datetime.now().strftime('%H:%M:%S.%f')[:-3],
                        "delay_ms": delay_ms,
                        "period": latest_ts // rebalance_interval_ms,
                    })
                    baseline[key] = latest_ts
                    change_count += 1

            poll_count += 1

            if changes_this_round:
                for c in changes_this_round:
                    delay_str = f"{c['delay_ms']/1000:.1f}s" if abs(c['delay_ms']) < 600000 else f"{c['delay_ms']/3600000:.1f}h"
                    status = "已收盘" if c['delay_ms'] >= 0 else "未收盘"
                    logger.info(f"  [{c['detect_time']}] {c['symbol']:12s} | K线开盘: {c['time']} | 周期:{c['period']} | {status} 距收盘:{delay_str}")

                # 同步率：找最新K线周期，统计有多少币种到了这个周期（策略调仓判断逻辑）
                valid_ts = [ts for ts in baseline.values() if ts > 0]
                if valid_ts:
                    max_period = max(ts // rebalance_interval_ms for ts in valid_ts)
                    in_max_period = sum(1 for ts in valid_ts if ts // rebalance_interval_ms >= max_period)
                    ratio = in_max_period / len(baseline) if baseline else 0
                    trigger = "✓ 触发调仓" if ratio >= 0.8 else ""
                    logger.info(f"  >> 同步率(周期{max_period}): {in_max_period}/{len(baseline)} ({ratio*100:.1f}%) {trigger} | Lua耗时: {poll_elapsed_ms:.1f}ms")

            now = time.time()
            if now - last_status_time >= 1800:
                logger.info(f"  [{datetime.now().strftime('%H:%M:%S')}] 心跳 | 轮询 {poll_count} 次 | 变化 {change_count} | Lua耗时: {poll_elapsed_ms:.1f}ms")
                last_status_time = now

            time.sleep(0.001)

    except KeyboardInterrupt:
        logger.info(f"\n已停止 | 总轮询: {poll_count} | 总变化: {change_count}")


if __name__ == "__main__":
    main()
