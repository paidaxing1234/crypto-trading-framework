#!/usr/bin/env python3
"""调仓轨迹记录器: 每个 8h 边界后运行, 把三个策略的调仓里程碑 + 边界观测器的数据到达
曲线合并成一份统一时间线, 追加到 /var/log/rebalance_trace.log。

记录内容(相对边界的偏移秒):
  - 预热/预取(边界前): [预热] [币种检查](five_mom 预取)
  - 触发: 同步率达标 / 新周期
  - 数据阶段: [当日数据](apollo) / [数据] 完成(mastercombo)
  - 信号/持仓/下单波([已报] 按波汇总)/调仓完成
  - 收尾: [成交] 笔数与最后时刻 / [滑点]
  - boundary_probe 的观测块(若有)

cron(CST): 3 0,8,16 * * *   (边界后3分钟, 调仓含收尾已结束)
"""
import os
import re
import sys
import time
from datetime import datetime, timedelta, timezone

LOG_DIR = "/path/to/Real-account-trading-framework/cpp/strategies/logs"
OUT = "/var/log/rebalance_trace.log"
PROBE_LOG = "/var/log/boundary_probe.log"
CST = timezone(timedelta(hours=8))

STRATS = [
    ("five_mom",    "ycx2_five_mom_factor_binance_testnet_{d}.log", (0, 8, 16)),
    ("apollo",      "hyfdaytest_apollo_fund_{d}.log",               (8,)),
    ("mastercombo", "hyfcombo_mastercombo_{d}.log",                 (8,)),
]

# 里程碑模式: (tag, regex, 是否聚合成"波")
PATTERNS = [
    ("预热",     re.compile(r"\[预热\]")),
    ("预取跳过", re.compile(r"预取仍新鲜")),
    ("币种检查", re.compile(r"\[币种检查\] 可交易")),
    ("触发",     re.compile(r"同步率达标|开始调仓|\[新周期\]")),
    ("当日数据", re.compile(r"\[当日数据\] (完成|预热命中)")),
    ("数据完成", re.compile(r"\[数据\] 完成")),
    ("信号",     re.compile(r"\[因子排名\]|信号: \d+ 条|signal_date")),
    ("持仓",     re.compile(r"\[持仓查询\] 获取到")),
    ("调仓开始", re.compile(r"\[调仓#\d+\] 开始")),
    ("已报",     re.compile(r"\[已报\]")),
    ("调仓完成", re.compile(r"\[调仓#\d+\] 完成")),
    ("成交",     re.compile(r"\[成交\] ")),
    ("成交未查到", re.compile(r"未查到成交记录")),
    ("滑点",     re.compile(r"\[滑点\]")),
]
TS_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]")


def parse_lines(path, t_lo, t_hi):
    """取 [t_lo, t_hi] (CST datetime) 窗口内的 (ts, line)"""
    out = []
    if not os.path.exists(path):
        return out
    with open(path, errors="replace") as f:
        for line in f:
            m = TS_RE.match(line)
            if not m:
                continue
            ts = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").replace(tzinfo=CST)
            if t_lo <= ts <= t_hi:
                out.append((ts, line.rstrip()))
    return out


def fmt_off(ts, boundary):
    return f"{(ts - boundary).total_seconds():+8.2f}s"


def trace_strategy(name, pattern, boundary, t_lo, t_hi, w):
    # 日志按 CST 日滚动; 00:00 边界的"前"在前一天文件里
    days = {t_lo.strftime("%Y%m%d"), t_hi.strftime("%Y%m%d")}
    lines = []
    for d in sorted(days):
        lines += parse_lines(os.path.join(LOG_DIR, pattern.format(d=d)), t_lo, t_hi)
    lines.sort(key=lambda x: x[0])

    w.write(f"\n  ── {name} ──\n")
    if not lines:
        w.write("    (窗口内无日志)\n")
        return
    bao, cheng = [], []          # 已报/成交 聚合
    for ts, line in lines:
        tag = next((t for t, rx in PATTERNS if rx.search(line)), None)
        if tag is None:
            continue
        if tag == "已报":
            bao.append(ts)
            continue
        if tag == "成交":
            cheng.append(ts)
            continue
        # 截断长行
        body = line.split("] ", 2)[-1][:110]
        w.write(f"    {fmt_off(ts, boundary)}  {body}\n")
        if tag == "调仓完成" and bao:
            # 在完成行前补"波"汇总
            waves, cur = [], [bao[0]]
            for t in bao[1:]:
                if (t - cur[-1]).total_seconds() > 0.08:
                    waves.append(cur)
                    cur = [t]
                else:
                    cur.append(t)
            waves.append(cur)
            for i, wv in enumerate(waves, 1):
                w.write(f"    {fmt_off(wv[0], boundary)}  └ 已报 波{i}: {len(wv)} 单\n")
            bao = []
    if cheng:
        w.write(f"    {fmt_off(cheng[-1], boundary)}  成交确认 {len(cheng)} 笔 "
                f"(首 {fmt_off(cheng[0], boundary).strip()})\n")


def probe_block(boundary_utc, w):
    """从 boundary_probe.log 取该边界的观测块"""
    if not os.path.exists(PROBE_LOG):
        return
    key = boundary_utc.strftime("%m-%d %H:%M")
    blocks, cur, hit = [], [], False
    with open(PROBE_LOG, errors="replace") as f:
        for line in f:
            if "边界观测开始" in line:
                cur, hit = [line], (f"boundary={key}" in line)
            elif cur:
                cur.append(line)
                if "观测结束" in line:
                    if hit:
                        blocks.append(cur[:])
                    cur, hit = [], False
    if blocks:
        w.write("\n  ── 数据到达观测(boundary_probe) ──\n")
        for line in blocks[-1]:
            w.write("    " + line.rstrip()[:130] + "\n")


def main():
    now = datetime.now(CST)
    # 最近一个 8h 边界(CST 00/08/16)
    b_hour = (now.hour // 8) * 8
    boundary = now.replace(hour=b_hour, minute=0, second=0, microsecond=0)
    if (now - boundary).total_seconds() < 60:     # 边界刚过不足1分钟: 用上一个
        boundary -= timedelta(hours=8)
    t_lo = boundary - timedelta(minutes=12)       # 覆盖 23:50/07:50 起的预热
    t_hi = boundary + timedelta(minutes=3)

    with open(OUT, "a") as w:
        w.write(f"\n{'=' * 78}\n")
        w.write(f"边界 {boundary.strftime('%Y-%m-%d %H:%M CST')} "
                f"(= {boundary.astimezone(timezone.utc).strftime('%m-%d %H:%M UTC')})  "
                f"[生成于 {now.strftime('%H:%M:%S')}]\n")
        for name, pat, hours in STRATS:
            if boundary.hour not in hours:
                continue
            trace_strategy(name, pat, boundary, t_lo, t_hi, w)
        probe_block(boundary.astimezone(timezone.utc), w)
    print(f"trace 已写入 {OUT} (边界 {boundary.strftime('%m-%d %H:%M CST')})")


if __name__ == "__main__":
    main()
