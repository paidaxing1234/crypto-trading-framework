#!/usr/bin/env python3
"""00:00 UTC 边界观测器: 测量新 K线到达 Redis 的时序, 定责数据延迟根因。

背景: 00:00 UTC(全周期同收+资金费率结算)时, 529 币的新 8h bar 曾延迟 12.8s 才出现在
Redis, 且呈"静默后一次性出现"的队列泄洪形状(出现在策略调仓时点附近)。
本脚本在边界前 30s 启动, 持续 ~150s, 每 100ms 采样:
  1. 样本币 (流动性高/中/低) 的 1m/1h/8h 新 bar 首次出现时刻  → 区分"逐步到达"vs"批量泄洪"
  2. 全市场 8h 新bar覆盖率(同步率曲线, 与策略门槛同口径)
  3. Redis 健康: PING 延迟 / bgsave 是否进行中 / ops_per_sec
输出: /var/log/boundary_probe.log (追加)。纯只读, 对实盘无影响。

cron(CST): 59 7 * * *  → 23:59 UTC 启动, 覆盖 00:00 UTC 边界。
"""
import json
import time
from datetime import datetime, timezone

import redis

SAMPLE = ["BTCUSDT", "ETHUSDT", "SOLUSDT", "SAGAUSDT", "BILLUSDT",
          "ICXUSDT", "GMTUSDT", "ONTUSDT", "STGUSDT", "DEXEUSDT"]
INTERVALS = ["1m", "1h", "8h"]
HOUR_MS = 3600_000
LOG = "/var/log/boundary_probe.log"


def log(msg: str):
    line = f"[{datetime.now(timezone.utc).strftime('%H:%M:%S.%f')[:-3]}Z] {msg}"
    with open(LOG, "a") as f:
        f.write(line + "\n")


def main():
    r = redis.Redis(decode_responses=True)
    now_ms = int(time.time() * 1000)
    boundary = (now_ms // (8 * HOUR_MS) + 1) * 8 * HOUR_MS   # 下一个 8h 边界
    if boundary - now_ms > 120_000:   # 距边界超过2分钟: 说明起跑时间不对, 等到前30s
        time.sleep(max(0, (boundary - now_ms) / 1000 - 30))

    all8h = sorted(set(k.split(":")[2] for k in r.scan_iter("kline:binance:*:8h")))
    log(f"=== 边界观测开始: boundary={datetime.fromtimestamp(boundary/1000, timezone.utc).strftime('%m-%d %H:%M')}Z, 全市场8h键 {len(all8h)} 个 ===")

    # 口径: 策略等的是【刚收盘的 bar】(score = 边界 - 周期长度, 聚合器在边界时刻写出),
    #       而非戳=边界的新 bar(那根要等下个周期收盘才写)。
    IV_MS = {"1m": 60_000, "1h": 3600_000, "8h": 8 * 3600_000}
    first_seen = {}          # (sym, iv) -> 首次看到刚收盘bar的毫秒
    sync_marks = []          # (t_ms, 覆盖率) 里程碑
    last_ratio_logged = -1.0
    closed_8h = boundary - IV_MS["8h"]
    t_end = boundary + 120_000
    while int(time.time() * 1000) < t_end:
        t0 = time.time()
        now = int(t0 * 1000)
        # ① 样本币刚收盘bar探测 (pipeline)
        p = r.pipeline()
        todo = [(s, iv) for s in SAMPLE for iv in INTERVALS if (s, iv) not in first_seen]
        for s, iv in todo:
            p.zrange(f"kline:binance:{s}:{iv}", -1, -1, withscores=True)
        for (s, iv), res in zip(todo, p.execute()):
            if res and res[0][1] >= boundary - IV_MS[iv]:
                first_seen[(s, iv)] = now
                log(f"  首见收盘bar {s}:{iv}  +{(now - boundary) / 1000:+.2f}s")
        # ② 全市场 8h 覆盖率 (每~500ms 一次, 与策略同步门同口径)
        if now >= boundary and (now // 500) % 2 == 0:
            p = r.pipeline()
            for s in all8h:
                p.zrange(f"kline:binance:{s}:8h", -1, -1, withscores=True)
            res = p.execute()
            n_new = sum(1 for x in res if x and x[0][1] >= closed_8h)
            ratio = n_new / max(1, len(all8h))
            if ratio - last_ratio_logged >= 0.10 or (ratio >= 0.8 and last_ratio_logged < 0.8):
                log(f"  8h覆盖率 {n_new}/{len(all8h)} = {ratio:.1%}  +{(now - boundary) / 1000:+.2f}s")
                last_ratio_logged = ratio
                sync_marks.append((now, ratio))
            if ratio >= 0.999 and now - boundary > 5_000:
                break
        # ③ Redis 健康采样 (每~2s)
        if (now // 2000) % 1 == 0 and now % 2000 < 150:
            tp = time.time()
            r.ping()
            ping_ms = (time.time() - tp) * 1000
            info = r.info("persistence")
            if ping_ms > 5 or info.get("rdb_bgsave_in_progress"):
                log(f"  [redis] ping={ping_ms:.1f}ms bgsave={info.get('rdb_bgsave_in_progress')}  +{(now - boundary) / 1000:+.2f}s")
        time.sleep(max(0, 0.1 - (time.time() - t0)))

    # 汇总
    log("--- 汇总(相对边界秒) ---")
    for iv in INTERVALS:
        ts = sorted((v - boundary) / 1000 for (s, i), v in first_seen.items() if i == iv)
        if ts:
            log(f"  {iv}: 首个 {ts[0]:+.2f}s | 中位 {ts[len(ts)//2]:+.2f}s | 最慢 {ts[-1]:+.2f}s | n={len(ts)}")
    if sync_marks:
        hit = next((t for t, q in sync_marks if q >= 0.8), None)
        log(f"  8h覆盖率80%达标: {'+%.2fs' % ((hit - boundary) / 1000) if hit else '观测窗内未达标'}")
    log("=== 观测结束 ===\n")


if __name__ == "__main__":
    main()
