#!/bin/bash
# stats_api 看门狗: 探活端口, 不通才杀旧拉新。
# 必须是独立脚本: 若直接写在 crontab 里, pkill -f 'python3 stats_api.py' 会匹配到
# cron 自身的 sh -c 命令行(里面含这串文字), 把自己父 shell 杀掉, 启动命令永远跑不到。
# (2026-06-06 实际发生过: 前端三列空 = stats_api 死 + 看门狗自杀复活失败)

if curl -sf -m 4 http://127.0.0.1:8003/api/health >/dev/null 2>&1; then
    exit 0
fi

echo "[watchdog $(date '+%F %T')] health 不通, 重启 stats_api" >> /var/log/stats_api.log
# [s]tats 正则技巧: 不匹配本脚本自身/cron shell 的命令行
pkill -9 -f '[s]tats_api\.py' 2>/dev/null
sleep 1
cd /path/to/Real-account-trading-framework/cpp/scripts || exit 1
setsid /usr/bin/python3 stats_api.py >> /var/log/stats_api.log 2>&1 < /dev/null &
sleep 2
if curl -sf -m 4 http://127.0.0.1:8003/api/health >/dev/null 2>&1; then
    echo "[watchdog $(date '+%F %T')] 重启成功" >> /var/log/stats_api.log
else
    echo "[watchdog $(date '+%F %T')] 重启后 health 仍不通!" >> /var/log/stats_api.log
fi
