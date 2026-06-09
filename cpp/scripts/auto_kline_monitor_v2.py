#!/usr/bin/env python3
"""
自动K线监控和补全脚本 v2

功能:
1. 监控所有币种的各周期K线连续性
2. 通过OKX REST API自动拉取缺失的1分钟K线
3. 对于其他周期(5m/15m/30m/1h)，先检查1分钟K线是否足够聚合
4. 聚合后写入Redis，保持格式一致
5. 定时运行（默认30分钟）
6. 显示运行时间

作者: Claude Code
日期: 2026-01-28
"""

import json
import time
import subprocess
import sys
import requests
from datetime import datetime
from typing import List, Dict, Tuple, Optional

# ==================== 配置 ====================

REDIS_HOST = "127.0.0.1"
REDIS_PORT = 6379

# OKX REST API配置
OKX_REST_API = "https://www.okx.com"  # 主网
# OKX_REST_API = "https://www.okx.com"  # 模拟盘使用: https://www.okx.com

# Binance REST API配置
BINANCE_REST_API = "https://fapi.binance.com"  # U本位合约主网
# BINANCE_REST_API = "https://testnet.binancefuture.com"  # 测试网

# 代理配置（与kline_gap_filler.cpp保持一致）
USE_PROXY = True
PROXY_HOST = "127.0.0.1"
PROXY_PORT = 7890

# HTTP代理设置
if USE_PROXY:
    PROXIES = {
        'http': f'http://{PROXY_HOST}:{PROXY_PORT}',
        'https': f'http://{PROXY_HOST}:{PROXY_PORT}',
    }
else:
    PROXIES = None

# 请求超时和重试配置
REQUEST_TIMEOUT = 30  # 30秒超时
MAX_RETRIES = 3  # 最多重试3次
RETRY_DELAY = 2  # 重试延迟2秒
FETCH_TIMEOUT = 10  # 整个拉取操作的超时时间（秒）

# 监控的K线周期
INTERVALS = ["1m", "5m", "15m", "30m", "1h"]

# 周期对应的毫秒数
INTERVAL_MS = {
    "1m": 60 * 1000,
    "5m": 5 * 60 * 1000,
    "15m": 15 * 60 * 1000,
    "30m": 30 * 60 * 1000,
    "1h": 60 * 60 * 1000,
}

# OKX K线周期映射
OKX_INTERVAL_MAP = {
    "1m": "1m",
    "5m": "5m",
    "15m": "15m",
    "30m": "30m",
    "1h": "1H",
}

# Binance K线周期映射
BINANCE_INTERVAL_MAP = {
    "1m": "1m",
    "5m": "5m",
    "15m": "15m",
    "30m": "30m",
    "1h": "1h",
}

# 聚合配置: {目标周期: (基础周期, 倍数)}
AGGREGATION_CONFIG = {
    "5m": ("1m", 5),
    "15m": ("1m", 15),
    "30m": ("1m", 30),
    "1h": ("1m", 60),
}

# 运行间隔（秒）
RUN_INTERVAL = 5  # 5秒（快速连续检查）

# 日志文件
LOG_FILE = "/tmp/auto_kline_monitor.log"

# 每次最多拉取的K线数量
MAX_KLINES_PER_REQUEST = 100

# ==================== 工具函数 ====================

def log(message: str, level: str = "INFO"):
    """写入日志"""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    log_line = f"[{timestamp}] [{level}] {message}"
    print(log_line)

    try:
        with open(LOG_FILE, 'a', encoding='utf-8') as f:
            f.write(log_line + '\n')
    except Exception as e:
        print(f"写入日志失败: {e}")

def align_timestamp(ts_ms: int, period_ms: int) -> int:
    """对齐时间戳到周期边界"""
    return (ts_ms // period_ms) * period_ms

def format_timestamp(ts_ms: int) -> str:
    """格式化时间戳"""
    return datetime.fromtimestamp(ts_ms / 1000).strftime("%Y-%m-%d %H:%M:%S")

def redis_cmd(cmd: List[str]) -> str:
    """执行redis-cli命令"""
    try:
        result = subprocess.run(
            ["redis-cli"] + cmd,
            capture_output=True,
            text=True,
            timeout=10
        )
        return result.stdout.strip()
    except Exception as e:
        log(f"Redis命令执行失败: {e}", "ERROR")
        return ""

# ==================== OKX REST API ====================

def fetch_okx_klines(symbol: str, interval: str, start_ts: int, end_ts: int) -> List[Dict]:
    """
    从OKX REST API拉取K线数据

    Args:
        symbol: 交易对，如 BTC-USDT-SWAP
        interval: K线周期，如 1m, 5m, 15m
        start_ts: 开始时间戳（毫秒）
        end_ts: 结束时间戳（毫秒）

    Returns:
        K线列表
    """
    okx_interval = OKX_INTERVAL_MAP.get(interval, interval)
    url = f"{OKX_REST_API}/api/v5/market/history-candles"

    klines = []
    current_end = end_ts
    fetch_start_time = time.time()  # 记录开始时间

    while current_end >= start_ts:
        # 检查是否超时
        if time.time() - fetch_start_time > FETCH_TIMEOUT:
            log(f"拉取OKX K线超时 ({FETCH_TIMEOUT}秒)，已拉取 {len(klines)} 根", "WARN")
            break
        params = {
            "instId": symbol,
            "bar": okx_interval,
            "after": str(current_end),
            "limit": str(MAX_KLINES_PER_REQUEST)
        }

        # 重试机制
        success = False
        for retry in range(MAX_RETRIES):
            try:
                response = requests.get(
                    url,
                    params=params,
                    proxies=PROXIES,
                    timeout=REQUEST_TIMEOUT
                )
                response.raise_for_status()
                data = response.json()

                if data.get("code") != "0":
                    log(f"OKX API错误: {data.get('msg')}", "ERROR")
                    break

                candles = data.get("data", [])
                if not candles:
                    success = True
                    break

                # OKX返回格式: [ts, o, h, l, c, vol, volCcy, volCcyQuote, confirm]
                for candle in candles:
                    ts = int(candle[0])
                    if start_ts <= ts <= end_ts:
                        kline = {
                            "timestamp": ts,
                            "open": float(candle[1]),
                            "high": float(candle[2]),
                            "low": float(candle[3]),
                            "close": float(candle[4]),
                            "volume": float(candle[5])
                        }
                        klines.append(kline)

                # 获取最早的时间戳，继续向前拉取
                if candles:
                    current_end = int(candles[-1][0]) - 1
                else:
                    break

                success = True
                break  # 成功，跳出重试循环

            except Exception as e:
                if retry < MAX_RETRIES - 1:
                    log(f"拉取OKX K线失败 (重试 {retry + 1}/{MAX_RETRIES}): {e}", "WARN")
                    time.sleep(RETRY_DELAY)
                else:
                    log(f"拉取OKX K线失败 (已重试{MAX_RETRIES}次): {e}", "ERROR")
                    return klines  # 返回已拉取的部分

        if not success:
            break

        # 避免请求过快
        time.sleep(0.1)

    # 按时间戳排序
    klines.sort(key=lambda x: x["timestamp"])
    return klines

# ==================== Binance REST API ====================

def fetch_binance_klines(symbol: str, interval: str, start_ts: int, end_ts: int) -> List[Dict]:
    """
    从Binance REST API拉取K线数据

    Args:
        symbol: 交易对，如 BTCUSDT
        interval: K线周期，如 1m, 5m, 15m
        start_ts: 开始时间戳（毫秒）
        end_ts: 结束时间戳（毫秒）

    Returns:
        K线列表
    """
    binance_interval = BINANCE_INTERVAL_MAP.get(interval, interval)
    url = f"{BINANCE_REST_API}/fapi/v1/klines"

    klines = []
    current_start = start_ts
    fetch_start_time = time.time()  # 记录开始时间

    while current_start <= end_ts:
        # 检查是否超时
        if time.time() - fetch_start_time > FETCH_TIMEOUT:
            log(f"拉取Binance K线超时 ({FETCH_TIMEOUT}秒)，已拉取 {len(klines)} 根", "WARN")
            break
        params = {
            "symbol": symbol,
            "interval": binance_interval,
            "startTime": current_start,
            "endTime": end_ts,
            "limit": 1500  # Binance最大1500
        }

        # 重试机制
        success = False
        for retry in range(MAX_RETRIES):
            try:
                response = requests.get(
                    url,
                    params=params,
                    proxies=PROXIES,
                    timeout=REQUEST_TIMEOUT
                )
                response.raise_for_status()
                candles = response.json()

                if not candles:
                    success = True
                    break

                # Binance返回格式: [openTime, open, high, low, close, volume, closeTime, ...]
                for candle in candles:
                    ts = int(candle[0])
                    if start_ts <= ts <= end_ts:
                        kline = {
                            "timestamp": ts,
                            "open": float(candle[1]),
                            "high": float(candle[2]),
                            "low": float(candle[3]),
                            "close": float(candle[4]),
                            "volume": float(candle[5])
                        }
                        klines.append(kline)

                # 获取最后一个时间戳，继续向后拉取
                if candles:
                    last_ts = int(candles[-1][0])
                    if last_ts >= end_ts:
                        break
                    current_start = last_ts + INTERVAL_MS[interval]
                else:
                    break

                success = True
                break  # 成功，跳出重试循环

            except Exception as e:
                if retry < MAX_RETRIES - 1:
                    log(f"拉取Binance K线失败 (重试 {retry + 1}/{MAX_RETRIES}): {e}", "WARN")
                    time.sleep(RETRY_DELAY)
                else:
                    log(f"拉取Binance K线失败 (已重试{MAX_RETRIES}次): {e}", "ERROR")
                    return klines  # 返回已拉取的部分

        if not success:
            break

        # 避免请求过快
        time.sleep(0.1)

    # 按时间戳排序并去重
    klines_dict = {k["timestamp"]: k for k in klines}
    klines = sorted(klines_dict.values(), key=lambda x: x["timestamp"])
    return klines

# ==================== Redis操作 ====================

class RedisClient:
    def __init__(self):
        pass

    def get_all_symbols(self) -> List[Tuple[str, str]]:
        """获取所有交易对 (exchange, symbol)"""
        output = redis_cmd(["KEYS", "kline:*:1m"])
        if not output:
            return []

        keys = output.split('\n')
        symbols = []

        for key in keys:
            parts = key.split(':')
            if len(parts) >= 4:
                exchange = parts[1]
                symbol = parts[2]
                # 只处理U本位合约
                if self._is_usdt_contract(exchange, symbol):
                    symbols.append((exchange, symbol))

        return symbols

    def _is_usdt_contract(self, exchange: str, symbol: str) -> bool:
        """检查是否为U本位合约"""
        if exchange == "okx":
            return "-USDT-SWAP" in symbol
        elif exchange == "binance":
            return symbol.endswith("USDT")
        return False

    def get_klines(self, exchange: str, symbol: str, interval: str) -> List[Dict]:
        """获取K线数据"""
        key = f"kline:{exchange}:{symbol}:{interval}"
        output = redis_cmd(["ZRANGE", key, "0", "-1", "WITHSCORES"])

        if not output:
            return []

        lines = output.split('\n')
        klines = []

        for i in range(0, len(lines), 2):
            if i + 1 < len(lines):
                try:
                    kline_str = lines[i]
                    if kline_str and kline_str[0] == '{':
                        kline = json.loads(kline_str)
                        klines.append(kline)
                except:
                    pass

        return klines

    def write_kline(self, exchange: str, symbol: str, interval: str, kline: Dict) -> bool:
        """写入K线到Redis"""
        key = f"kline:{exchange}:{symbol}:{interval}"

        # 确保格式正确
        kline_data = {
            "type": "kline",
            "exchange": exchange,
            "symbol": symbol,
            "interval": interval,
            "timestamp": kline["timestamp"],
            "open": kline["open"],
            "high": kline["high"],
            "low": kline["low"],
            "close": kline["close"],
            "volume": kline["volume"]
        }

        value = json.dumps(kline_data)

        try:
            redis_cmd(["ZADD", key, str(kline["timestamp"]), value])
            return True
        except Exception as e:
            log(f"写入K线失败: {e}", "ERROR")
            return False

    def check_continuity(self, exchange: str, symbol: str, interval: str) -> Tuple[int, int, List[Tuple[int, int]]]:
        """
        检查K线连续性
        返回: (总数, 缺失数, 缺失段列表[(start_ts, end_ts)])
        """
        klines = self.get_klines(exchange, symbol, interval)

        if not klines:
            return 0, 0, []

        timestamps = sorted([k["timestamp"] for k in klines])
        total = len(timestamps)

        interval_ms = INTERVAL_MS[interval]
        gaps = []

        for i in range(1, len(timestamps)):
            expected = timestamps[i-1] + interval_ms
            actual = timestamps[i]

            if actual != expected:
                # 发现缺口
                gap_start = expected
                gap_end = actual - interval_ms
                gaps.append((gap_start, gap_end))

        missing_count = sum((end - start) // interval_ms + 1 for start, end in gaps)

        return total, missing_count, gaps

    def deduplicate_klines(self, exchange: str, symbol: str, interval: str) -> int:
        """
        去重K线数据（删除相同时间戳的重复数据）

        Args:
            exchange: 交易所
            symbol: 交易对
            interval: K线周期

        Returns:
            删除的重复数据数量
        """
        key = f"kline:{exchange}:{symbol}:{interval}"

        # 获取所有K线数据
        output = redis_cmd(["ZRANGE", key, "0", "-1", "WITHSCORES"])

        if not output:
            return 0

        lines = output.split('\n')

        # 解析所有K线，按时间戳分组
        timestamp_groups = {}  # {timestamp: [kline_json_str, ...]}

        for i in range(0, len(lines), 2):
            if i + 1 < len(lines):
                try:
                    kline_str = lines[i]
                    score_str = lines[i + 1]

                    if kline_str and kline_str[0] == '{':
                        kline = json.loads(kline_str)
                        timestamp = kline.get("timestamp")

                        if timestamp:
                            if timestamp not in timestamp_groups:
                                timestamp_groups[timestamp] = []
                            timestamp_groups[timestamp].append(kline_str)
                except Exception as e:
                    log(f"解析K线数据失败: {e}", "ERROR")
                    continue

        # 统计重复数据
        removed_count = 0

        for timestamp, kline_strs in timestamp_groups.items():
            if len(kline_strs) > 1:
                # 发现重复数据，删除所有副本，只保留第一个
                removed_count += len(kline_strs) - 1

                # 删除该时间戳的所有数据
                for kline_str in kline_strs:
                    redis_cmd(["ZREM", key, kline_str])

                # 重新添加第一个（保留最早的数据）
                redis_cmd(["ZADD", key, str(timestamp), kline_strs[0]])

        return removed_count

# ==================== K线聚合 ====================

def aggregate_klines(klines_1m: List[Dict], target_interval: str, aligned_ts: int) -> Optional[Dict]:
    """
    从1分钟K线聚合成目标周期K线

    Args:
        klines_1m: 1分钟K线列表（已排序）
        target_interval: 目标周期
        aligned_ts: 对齐后的时间戳

    Returns:
        聚合后的K线，如果数据不足则返回None
    """
    if not klines_1m:
        return None

    base_interval, multiplier = AGGREGATION_CONFIG[target_interval]

    if len(klines_1m) < multiplier:
        return None

    # 只取前multiplier根
    klines_to_agg = klines_1m[:multiplier]

    # 聚合
    aggregated = {
        "timestamp": aligned_ts,
        "open": klines_to_agg[0]["open"],
        "close": klines_to_agg[-1]["close"],
        "high": max(k["high"] for k in klines_to_agg),
        "low": min(k["low"] for k in klines_to_agg),
        "volume": sum(k["volume"] for k in klines_to_agg)
    }

    return aggregated

# ==================== 主处理逻辑 ====================

def process_symbol(redis_client: RedisClient, exchange: str, symbol: str):
    """处理单个交易对的K线补全"""
    log(f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    log(f"处理 {exchange}:{symbol}")

    # 0. 先对所有周期进行去重
    log(f"步骤0: 去重所有周期的K线数据")
    for interval in INTERVALS:
        removed_count = redis_client.deduplicate_klines(exchange, symbol, interval)
        if removed_count > 0:
            log(f"  {interval} 去重: 删除 {removed_count} 条重复数据", "WARN")

    # 1. 检查1分钟K线
    total_1m, missing_1m, gaps_1m = redis_client.check_continuity(exchange, symbol, "1m")
    log(f"1m K线: 总数={total_1m}, 缺失={missing_1m}, 缺口数={len(gaps_1m)}")

    if missing_1m > 0:
        log(f"发现1分钟K线缺失 {missing_1m} 根，开始拉取", "WARN")

        total_fetched = 0
        for gap_start, gap_end in gaps_1m:
            log(f"  拉取缺口: {format_timestamp(gap_start)} ~ {format_timestamp(gap_end)}")

            # 根据交易所选择对应的API
            if exchange == "okx":
                fetched = fetch_okx_klines(symbol, "1m", gap_start, gap_end)
            elif exchange == "binance":
                fetched = fetch_binance_klines(symbol, "1m", gap_start, gap_end)
            else:
                log(f"  不支持的交易所: {exchange}", "ERROR")
                continue

            for kline in fetched:
                if redis_client.write_kline(exchange, symbol, "1m", kline):
                    total_fetched += 1

            log(f"  ✓ 拉取并写入 {len(fetched)} 根1分钟K线")

        if total_fetched > 0:
            log(f"✓ 共拉取并写入 {total_fetched} 根1分钟K线")

    # 2. 检查其他周期K线
    for interval in ["5m", "15m", "30m", "1h"]:
        total, missing, gaps = redis_client.check_continuity(exchange, symbol, interval)
        log(f"{interval} K线: 总数={total}, 缺失={missing}, 缺口数={len(gaps)}")

        if missing > 0:
            log(f"发现{interval}K线缺失 {missing} 根，尝试聚合补全", "WARN")

            # 获取所有1分钟K线
            klines_1m = redis_client.get_klines(exchange, symbol, "1m")
            klines_1m_dict = {k["timestamp"]: k for k in klines_1m}

            base_interval, multiplier = AGGREGATION_CONFIG[interval]
            interval_ms = INTERVAL_MS[interval]

            filled_count = 0
            for gap_start, gap_end in gaps:
                # 遍历缺口中的每个时间点
                current_ts = gap_start
                skip_gap = False  # 标记是否跳过当前缺口

                while current_ts <= gap_end and not skip_gap:
                    # 检查对应的1分钟K线是否足够
                    required_1m = []
                    for i in range(multiplier):
                        ts_1m = current_ts + i * INTERVAL_MS["1m"]
                        if ts_1m in klines_1m_dict:
                            required_1m.append(klines_1m_dict[ts_1m])
                        else:
                            break

                    if len(required_1m) == multiplier:
                        # 去重
                        dedup_map = {}
                        for k in required_1m:
                            dedup_map[k["timestamp"]] = k

                        dedup_klines = sorted(dedup_map.values(), key=lambda x: x["timestamp"])

                        if len(dedup_klines) >= multiplier:
                            # 聚合
                            aggregated = aggregate_klines(dedup_klines, interval, current_ts)
                            if aggregated:
                                # 写入Redis
                                if redis_client.write_kline(exchange, symbol, interval, aggregated):
                                    filled_count += 1
                    else:
                        # 1分钟K线不足，需要先拉取
                        log(f"  {format_timestamp(current_ts)} 的1分钟K线不足，拉取中...", "WARN")
                        # 拉取这个时间段的1分钟K线
                        end_ts_1m = current_ts + (multiplier - 1) * INTERVAL_MS["1m"]

                        # 根据交易所选择对应的API
                        if exchange == "okx":
                            fetched_1m = fetch_okx_klines(symbol, "1m", current_ts, end_ts_1m)
                        elif exchange == "binance":
                            fetched_1m = fetch_binance_klines(symbol, "1m", current_ts, end_ts_1m)
                        else:
                            fetched_1m = []

                        # 如果拉取失败（返回空列表），跳过当前缺口
                        if not fetched_1m:
                            log(f"  无法拉取1分钟K线，跳过缺口 {format_timestamp(gap_start)} ~ {format_timestamp(gap_end)}", "WARN")
                            skip_gap = True
                            break

                        for kline in fetched_1m:
                            redis_client.write_kline(exchange, symbol, "1m", kline)
                            klines_1m_dict[kline["timestamp"]] = kline

                    current_ts += interval_ms

            if filled_count > 0:
                log(f"  ✓ 聚合并写入 {filled_count} 根{interval}K线")

def run_monitor():
    """运行一次监控"""
    start_time = time.time()

    log("╔════════════════════════════════════════════════════════════╗")
    log("║     自动K线监控和补全 - 开始运行                           ║")
    log("╚════════════════════════════════════════════════════════════╝")

    try:
        redis_client = RedisClient()

        # 获取所有交易对
        symbols = redis_client.get_all_symbols()
        log(f"找到 {len(symbols)} 个U本位合约")

        # 处理每个交易对
        for exchange, symbol in symbols:
            try:
                process_symbol(redis_client, exchange, symbol)
            except Exception as e:
                log(f"处理 {exchange}:{symbol} 时出错: {e}", "ERROR")

        # 计算运行时间
        end_time = time.time()
        elapsed_time = end_time - start_time

        # 格式化时间
        if elapsed_time < 60:
            time_str = f"{elapsed_time:.2f} 秒"
        elif elapsed_time < 3600:
            minutes = int(elapsed_time // 60)
            seconds = elapsed_time % 60
            time_str = f"{minutes} 分 {seconds:.2f} 秒"
        else:
            hours = int(elapsed_time // 3600)
            minutes = int((elapsed_time % 3600) // 60)
            seconds = elapsed_time % 60
            time_str = f"{hours} 小时 {minutes} 分 {seconds:.2f} 秒"

        log("╔════════════════════════════════════════════════════════════╗")
        log("║     监控完成                                                ║")
        log(f"║     运行时间: {time_str:44s} ║")
        log("╚════════════════════════════════════════════════════════════╝")

    except Exception as e:
        log(f"监控运行出错: {e}", "ERROR")
        # 即使出错也显示运行时间
        end_time = time.time()
        elapsed_time = end_time - start_time
        log(f"运行时间: {elapsed_time:.2f} 秒")

# ==================== 主程序 ====================

def main():
    """主程序"""
    log("自动K线监控脚本启动 v2")
    log(f"运行间隔: {RUN_INTERVAL // 60} 分钟")
    log(f"日志文件: {LOG_FILE}")
    log(f"OKX REST API: {OKX_REST_API}")
    log(f"Binance REST API: {BINANCE_REST_API}")
    log("支持交易所: OKX, Binance")

    if len(sys.argv) > 1 and sys.argv[1] == "--once":
        # 只运行一次
        log("单次运行模式")
        run_monitor()
    else:
        # 循环运行
        log("循环运行模式")
        while True:
            run_monitor()
            log(f"等待 {RUN_INTERVAL} 秒后再次运行...")
            time.sleep(RUN_INTERVAL)

if __name__ == "__main__":
    main()
