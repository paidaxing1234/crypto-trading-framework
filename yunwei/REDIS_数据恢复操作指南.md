# Redis 数据恢复操作指南

> 用途：Redis 被清空 / 重启丢数据 / 换机后，从零重建全部行情与衍生数据。
> 适用：本机（CST=UTC+8）。所有脚本在 `cpp/scripts/`，**幂等**（同 score 先删后写），重复跑无害。
> 上次执行：2026-06-04（一次完整恢复，本指南即据此整理）。

---

## 0. 背景：Redis 里有哪些数据、谁维护

| 数据 | Redis Key 模式 | 来源 / 维护者 | 丢了怎么补 |
|---|---|---|---|
| K线(8周期) | `kline:binance:{sym}:{1m,5m,15m,30m,1h,4h,8h,1d}` | **实时**: `trading_server_full`(C++) 秒级写 + `fast_kline_filler.py` 补口；**历史**: `refill_from_vision.py` | 见 §2 |
| 资金费率 | `funding_rate:binance:{sym}:1d` | `preload_funding_rate.py`（cron 每日 00:00 UTC） | 见 §3 |
| pit100 OI/多空比/taker买量 | `oi:` `lsr_top:` `lsr_acct:` `kline_tbb:` `binance:{sym}:1h` | `pit100_metrics_backfill.py`(深历史) + `pit100_data_recorder.py`(增量) | 见 §4 |
| 净值/统计时序 | `equity_history:` `equity_latest:` `trade_count:` | `equity_recorder.py`（cron 每5分钟） | cron 自动重建，无需手动 |

**根因提醒（务必先做 §1）**：之前丢数据是因为 **Redis 没开 RDB 持久化**（`save` 为空），一重启就全没。恢复后一定确认持久化是开的。

---

## 1.（最先做）确认/开启 Redis 持久化，防止再次重启丢光

```bash
# 看当前 save 规则(空 = 没开持久化, 危险!)
redis-cli CONFIG GET save

# 持久化策略 = cron 安全分钟主动 BGSAVE(主) + 长周期自动 save(兜底)
redis-cli CONFIG SET save "7200 1"      # 兜底: cron 失效时最多丢2小时(K线可重建)
redis-cli CONFIG REWRITE
# cron(应已存在, 没有就补): 每小时 :20/:40 主动存盘, dump≈95s,
# 距任何 1h/8h/1d 边界 ≥18 分钟, 也避开 07:50-07:55 CST 的策略预热窗
# 20,40 * * * * /usr/bin/redis-cli BGSAVE >/dev/null 2>&1
```
> ⚠️ 两个教训:
> 1. 不要用 `60 10000` 高频规则:持续K线写入使其每 ~2.5 分钟连轴 fork 21GB(2026-06-06);
> 2. **不要让自动 save 的随机相位撞 K线边界**:`1800 1` 曾让 BGSAVE 横跨 00:00 UTC 边界,
>    fork-COW 把写放大数倍, 7000+笔收盘写突发排空拖到 3.2s(2026-06-07 probe 实测)。
>    所以改为 cron 固定安全分钟主动存盘。

落盘位置 / 磁盘核对：
```bash
redis-cli CONFIG GET dir          # /var/lib/redis/persist
df -h /var/lib/redis/persist      # 独立 49G 盘; 满载 dump.rdb ~13G, 存盘峰值~26G, 需≥30G空闲
```
> 满载内存 ~21G、RDB ~13G、存盘瞬间(旧+新两份)峰值 ~26G。49G 盘够，留意别让数据集大幅超过 22G。

---

## 2. K线（8 周期，全市场）

### 2a. 历史回填（vision 静态归档，只到昨天）
脚本 `refill_from_vision.py`：从 `data.binance.vision` 拉日度 K线 zip，写入 Redis。
**每周期默认天数已内置**（无需指定）：`1m/5m/15m/30m=60d, 1h=180d, 4h/8h=60d, 1d=120d`。

```bash
cd /path/to/Real-account-trading-framework/cpp/scripts
nohup python3 refill_from_vision.py --interval ALL --days 0 --workers 16 \
    >> /var/log/vision_refill.log 2>&1 &
# 进度: tail -f /var/log/vision_refill.log   (按 1m→5m→...→1d 顺序, 全市场约 30-60 分钟)
# 重起前先杀: pkill -f refill_from_vision
```
> `--days 0` = 用每周期默认天数。单周期可 `--interval 1h --days 180`。

### 2b. 今天的实时尾巴（vision 补不到今天）
vision 只到昨天 UTC。今天 00:00→现在 的 bar 由两者之一补：
- **首选**：启动 `trading_server_full`（见 §5），它实时写新 bar；
- **补口**：`fast_kline_filler.py`（REST 拉 1m + 聚合到各周期），默认补 12h，可 `--hours 72`：
```bash
nohup python3 fast_kline_filler.py --loop >> /var/log/fast_kline_filler.log 2>&1 &
# 一次性补更长: python3 fast_kline_filler.py --hours 72
```

### 2c. 核对
```bash
for iv in 1m 5m 15m 30m 1h 4h 8h 1d; do
  echo "$iv: $(redis-cli --scan --pattern "kline:binance:*:$iv" | wc -l) 币, BTC $(redis-cli ZCARD kline:binance:BTCUSDT:$iv) 根"
done
# 预期: 每周期 ~529 币; BTC 1m≈86400(60天) / 1h≈4320(180天) / 1d≈120
redis-cli ZRANGE kline:binance:BTCUSDT:1m -1 -1   # 最新bar应接近当前UTC时间
```

---

## 3. 资金费率

```bash
cd /path/to/Real-account-trading-framework/cpp/scripts
nohup python3 preload_funding_rate.py --days 120 >> /var/log/funding_rate_cron.log 2>&1 &
# 核对: redis-cli --scan --pattern 'funding_rate:*' | wc -l   (预期 ~526 币)
```
> 已有 cron：`0 8 * * *`(CST)= 00:00 UTC 每日自动补。脚本会跳过当天 UTC(未结算完)。

---

## 4. pit100 额外 REST 数据（OI / 多空比 / taker买量）

币安 `/futures/data/*` 只留 ~30 天，必须自己攒。两步：

### 4a. 深历史回填（vision metrics 归档，40 天）
```bash
cd /path/to/Real-account-trading-framework/cpp/scripts
nohup python3 pit100_metrics_backfill.py --days 40 --workers 8 >> /var/log/pit100_recorder.log 2>&1 &
```

### 4b. 最近几小时增量（REST，补 metrics 归档到今天的缺口）
```bash
nohup python3 pit100_data_recorder.py >> /var/log/pit100_recorder.log 2>&1 &
```
### 4c. 核对
```bash
for p in oi lsr_top lsr_acct kline_tbb; do echo "$p: $(redis-cli --scan --pattern "$p:binance:*:1h" | wc -l) 币"; done
# 预期: oi/lsr_top/lsr_acct ~527; kline_tbb 起步可能少几十(下一整点 cron 自动补齐)
```
> cron：`2 * * * *` 每小时；带 `pgrep -f bnperp_pit100_live.py || ...` 守卫——pit100 策略在跑时由策略内后台线程维护，cron 自动跳过(不重复拉)。

---

## 5. 系统恢复（数据≠系统）

数据齐了，还要确认 C++ 服务在跑（它负责实时K线尾巴 + 接收策略下单）：
```bash
pgrep -af trading_server_full || echo "未运行, 需重启"
# 启动(在 tmux 里, 项目约定):
# cd /path/to/Real-account-trading-framework/cpp/build && ./trading_server_full
```
> ⚠️ 重启交易服务器是动实盘的操作，确认无正在进行的调仓窗口再做。

---

## 6. 全部完成后：强制存盘 + 终检
```bash
redis-cli BGSAVE
sleep 10; redis-cli INFO persistence | grep -E "rdb_last_bgsave_status|rdb_last_save_time"
redis-cli DBSIZE
redis-cli INFO memory | grep used_memory_human   # 满载约 21G
df -h /var/lib/redis/persist                       # 可用应 ~34G
```

---

## 7. 一键恢复（顺序汇总，复制即用）
```bash
cd /path/to/Real-account-trading-framework/cpp/scripts
# 1) 持久化(30分钟一存; 勿用高频规则, 见 §1 警告)
redis-cli CONFIG SET save "1800 1" && redis-cli CONFIG REWRITE
# 2) K线历史(后台, 最久)
nohup python3 refill_from_vision.py --interval ALL --days 0 --workers 16 >> /var/log/vision_refill.log 2>&1 &
# 3) 资金费率
nohup python3 preload_funding_rate.py --days 120 >> /var/log/funding_rate_cron.log 2>&1 &
# 4) pit100 数据
nohup python3 pit100_metrics_backfill.py --days 40 --workers 8 >> /var/log/pit100_recorder.log 2>&1 &
nohup python3 pit100_data_recorder.py >> /var/log/pit100_recorder.log 2>&1 &
# 5) 今日尾巴(若 trading_server 没在跑)
nohup python3 fast_kline_filler.py --loop >> /var/log/fast_kline_filler.log 2>&1 &
# 6) 等 vision 跑完(tail -f /var/log/vision_refill.log)后: redis-cli BGSAVE
```

## 现有 cron（维护用，恢复后无需重设）
```
0 8 * * *   资金费率(=00:00 UTC)
*/5 * * * * 净值记录器
*/5 * * * * stats_api 看门狗
10 0 * * *  净值历史回填(=16:10 UTC)
2 * * * *   pit100 数据(策略在跑则跳过)
```
