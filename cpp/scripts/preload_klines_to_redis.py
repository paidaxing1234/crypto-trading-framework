#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
预加载历史 K 线数据到 Redis

从 Binance/OKX API 拉取历史 K 线数据并存储到 Redis，供策略使用。

使用方法：
    python preload_klines_to_redis.py [--days 60] [--interval 1m] [--exchange binance]
    python preload_klines_to_redis.py --exchange okx --days 60 --interval 1m
    python preload_klines_to_redis.py --exchange all --days 60  # 同时加载两个交易所

环境变量：
    REDIS_HOST       - Redis 主机地址 (默认: 127.0.0.1)
    REDIS_PORT       - Redis 端口 (默认: 6379)
    REDIS_PASSWORD   - Redis 密码 (默认: 空)
    USE_PROXY        - 是否启用代理: 1/true/yes/on 表示启用 (默认: 禁用)
    PROXY_HOST       - 代理主机 (默认: 127.0.0.1)
    PROXY_PORT       - 代理端口 (默认: 7890)

@author Sequence Team
@date 2026-01
"""

import os
import sys
import json
import time
import argparse
import requests
import redis
from datetime import datetime, timedelta
from typing import List, Dict, Optional
from concurrent.futures import ThreadPoolExecutor, as_completed

# ==================== 配置 ====================

# Redis 配置
REDIS_HOST = os.getenv("REDIS_HOST", "127.0.0.1")
REDIS_PORT = int(os.getenv("REDIS_PORT", "6379"))
REDIS_PASSWORD = os.getenv("REDIS_PASSWORD", "")

# Binance API 配置
BINANCE_FUTURES_URL = "https://fapi.binance.com"
BINANCE_TESTNET_URL = "https://testnet.binancefuture.com"

# OKX API 配置
OKX_API_URL = "https://www.okx.com"
OKX_TESTNET_URL = "https://www.okx.com"  # OKX 测试网使用相同域名，通过 header 区分

# 每批次最大数量
BATCH_SIZE = 300  # OKX 限制 300，Binance 限制 1500

# 并发线程数
MAX_WORKERS = 3

# 请求间隔（秒）
REQUEST_DELAY = 0.5  # OKX默认延迟
BINANCE_REQUEST_DELAY = 2.0  # Binance需要更长延迟，避免418错误
BINANCE_LIST_TIME_DELAY = 3.0  # Binance查询上线时间的延迟（更长，避免限流）


DEFAULT_USE_PROXY = os.getenv("USE_PROXY", "0").lower() in ("1", "true", "yes", "on")


class BaseKlineLoader:
    """K 线数据加载器基类"""

    def __init__(self, use_proxy: bool = DEFAULT_USE_PROXY):
        self.session = requests.Session()
        self.session.headers.update({
            "Content-Type": "application/json"
        })

        # 代理配置
        if use_proxy:
            proxy_host = os.getenv("PROXY_HOST", "127.0.0.1")
            proxy_port = os.getenv("PROXY_PORT", "7890")
            proxy_url = f"http://{proxy_host}:{proxy_port}"
            self.session.proxies = {
                "http": proxy_url,
                "https": proxy_url
            }
            print(f"[代理] 使用代理: {proxy_url}")

    def get_exchange_info(self) -> List[str]:
        raise NotImplementedError

    def get_symbol_list_time(self, symbol: str) -> Optional[int]:
        """获取合约上线时间（毫秒时间戳）

        Returns:
            上线时间戳（毫秒），如果无法获取则返回None
        """
        raise NotImplementedError

    def get_klines(self, symbol: str, interval: str, start_time: int, end_time: int, limit: int) -> List[Dict]:
        raise NotImplementedError

    @property
    def exchange_name(self) -> str:
        raise NotImplementedError

    @property
    def batch_size(self) -> int:
        return BATCH_SIZE


class BinanceKlineLoader(BaseKlineLoader):
    """Binance K 线数据加载器"""

    def __init__(self, testnet: bool = False, use_proxy: bool = True):
        super().__init__(use_proxy)
        self.base_url = BINANCE_TESTNET_URL if testnet else BINANCE_FUTURES_URL
        self.testnet = testnet
        self._list_time_cache = {}  # 缓存合约上线时间

    @property
    def exchange_name(self) -> str:
        return "binance"

    @property
    def batch_size(self) -> int:
        return 1500

    def get_exchange_info(self) -> List[str]:
        """获取所有永续合约交易对"""
        url = f"{self.base_url}/fapi/v1/exchangeInfo"
        try:
            response = self.session.get(url, timeout=30)
            if response.status_code == 200:
                data = response.json()
                symbols = []
                for sym in data.get("symbols", []):
                    if sym.get("contractType") == "PERPETUAL" and sym.get("status") == "TRADING":
                        symbols.append(sym["symbol"])
                return sorted(symbols)
        except Exception as e:
            print(f"[Binance] 获取交易对列表失败: {e}")
        return []

    def get_symbol_list_time(self, symbol: str) -> Optional[int]:
        """获取Binance合约上线时间

        通过API查询历史最早K线，使用较长延迟避免限流
        """
        # 检查缓存
        if symbol in self._list_time_cache:
            return self._list_time_cache[symbol]

        try:
            # 使用一个很早的时间戳（2017-01-01，Binance成立时间）作为起点
            url = f"{self.base_url}/fapi/v1/continuousKlines"
            early_time = int(datetime(2017, 1, 1).timestamp() * 1000)
            current_time = int(datetime.now().timestamp() * 1000)

            params = {
                "pair": symbol,
                "contractType": "PERPETUAL",
                "interval": "1d",
                "startTime": early_time,
                "endTime": current_time,
                "limit": 1  # 只获取最早的1条
            }

            # 添加延迟，避免限流
            time.sleep(BINANCE_LIST_TIME_DELAY)

            response = self.session.get(url, params=params, timeout=30)
            if response.status_code == 200:
                data = response.json()
                if data and len(data) > 0:
                    # 返回第一条K线的时间戳
                    earliest_ts = int(data[0][0])
                    # 缓存结果
                    self._list_time_cache[symbol] = earliest_ts
                    return earliest_ts
            elif response.status_code == 418:
                # IP被封禁，等待更长时间后重试一次
                print(f"[Binance] {symbol} API限流(418)，等待10秒后重试...")
                time.sleep(10)
                response = self.session.get(url, params=params, timeout=30)
                if response.status_code == 200:
                    data = response.json()
                    if data and len(data) > 0:
                        earliest_ts = int(data[0][0])
                        self._list_time_cache[symbol] = earliest_ts
                        return earliest_ts
                else:
                    print(f"[Binance] {symbol} 重试后仍然失败，跳过上线时间检测")
                    return None
        except Exception as e:
            print(f"[Binance] {symbol} 获取上线时间失败: {e}")
        return None

    def get_klines(self, symbol: str, interval: str, start_time: int, end_time: int, limit: int = 1500) -> List[Dict]:
        """获取 K 线数据（带重试和418错误处理）"""
        url = f"{self.base_url}/fapi/v1/continuousKlines"
        params = {
            "pair": symbol,
            "contractType": "PERPETUAL",
            "interval": interval,
            "startTime": start_time,
            "endTime": end_time,
            "limit": limit
        }

        max_retries = 3
        retry_delay = 10.0  # 初始重试延迟

        for attempt in range(max_retries):
            try:
                response = self.session.get(url, params=params, timeout=30)

                if response.status_code == 200:
                    data = response.json()
                    klines = []
                    for item in data:
                        if len(item) >= 11:
                            kline = {
                                "timestamp": item[0],
                                "open": float(item[1]),
                                "high": float(item[2]),
                                "low": float(item[3]),
                                "close": float(item[4]),
                                "volume": float(item[5]),
                                "amount": float(item[7]),
                                "trades": int(item[8]),
                                "buy_amount": float(item[10]),
                                "symbol": symbol,
                                "exchange": "binance",
                                "interval": interval,
                                "type": "kline"
                            }
                            klines.append(kline)
                    return klines

                elif response.status_code == 418:
                    # IP被临时封禁
                    try:
                        error_data = response.json()
                        ban_until = error_data.get('msg', '')
                        print(f"[Binance] {symbol} IP被封禁: {ban_until}")
                    except:
                        print(f"[Binance] {symbol} IP被封禁 (418)")

                    if attempt < max_retries - 1:
                        wait_time = retry_delay * (2 ** attempt)  # 指数退避
                        print(f"[Binance] {symbol} 等待 {wait_time:.1f} 秒后重试 (尝试 {attempt+1}/{max_retries})")
                        time.sleep(wait_time)
                        continue
                    else:
                        print(f"[Binance] {symbol} 达到最大重试次数，跳过")
                        return []

                elif response.status_code == 429:
                    # 请求过于频繁
                    print(f"[Binance] {symbol} 请求过于频繁 (429)")
                    if attempt < max_retries - 1:
                        wait_time = retry_delay * (2 ** attempt)
                        print(f"[Binance] {symbol} 等待 {wait_time:.1f} 秒后重试")
                        time.sleep(wait_time)
                        continue
                    else:
                        return []

                else:
                    print(f"[Binance] {symbol} API 返回 {response.status_code}: {response.text[:100]}")
                    return []

            except Exception as e:
                print(f"[Binance] {symbol} 获取 K 线失败: {e}")
                if attempt < max_retries - 1:
                    time.sleep(retry_delay)
                    continue
                return []

        return []


class OKXKlineLoader(BaseKlineLoader):
    """OKX K 线数据加载器"""

    def __init__(self, testnet: bool = False, use_proxy: bool = True):
        super().__init__(use_proxy)
        self.base_url = OKX_API_URL
        self.testnet = testnet
        self._instruments_cache = None  # 缓存instruments数据
        self._list_time_cache = {}  # 缓存合约数据起始时间
        if testnet:
            self.session.headers.update({
                "x-simulated-trading": "1"
            })

    @property
    def exchange_name(self) -> str:
        return "okx"

    @property
    def batch_size(self) -> int:
        return 300

    def _get_instruments_data(self) -> List[Dict]:
        """获取并缓存instruments数据"""
        if self._instruments_cache is not None:
            return self._instruments_cache

        url = f"{self.base_url}/api/v5/public/instruments"
        params = {"instType": "SWAP"}
        try:
            response = self.session.get(url, params=params, timeout=30)
            if response.status_code == 200:
                data = response.json()
                if data.get("code") == "0":
                    self._instruments_cache = data.get("data", [])
                    return self._instruments_cache
        except Exception as e:
            print(f"[OKX] 获取instruments数据失败: {e}")
        return []

    def get_exchange_info(self) -> List[str]:
        """获取所有永续合约交易对"""
        instruments = self._get_instruments_data()
        symbols = []
        for inst in instruments:
            inst_id = inst.get("instId", "")
            state = inst.get("state", "")
            # 只获取 USDT 永续合约且正在交易的
            if inst_id.endswith("-USDT-SWAP") and state == "live":
                symbols.append(inst_id)
        return sorted(symbols)

    def get_symbol_list_time(self, symbol: str) -> Optional[int]:
        """获取OKX合约实际数据起始时间

        通过拉取最早的K线来确定真实的数据起始时间，而不是使用listTime
        因为listTime可能早于实际开始交易的时间

        Returns:
            实际数据起始时间戳（毫秒），如果无法获取则返回None
        """
        # 检查缓存
        if symbol in self._list_time_cache:
            return self._list_time_cache[symbol]

        try:
            # 尝试获取最早的K线数据
            # 使用一个很早的时间戳（2020-01-01）作为起点
            url = f"{self.base_url}/api/v5/market/history-candles"
            early_time = int(datetime(2020, 1, 1).timestamp() * 1000)

            params = {
                "instId": symbol,
                "bar": "1D",  # 使用1D周期，更容易获取到最早数据
                "after": str(early_time),
                "limit": "100"
            }

            response = self.session.get(url, params=params, timeout=30)
            if response.status_code == 200:
                data = response.json()
                if data.get("code") == "0":
                    candles = data.get("data", [])
                    if candles:
                        # OKX返回的数据是倒序的（最新的在前），最后一条是最早的
                        earliest_ts = int(candles[-1][0])
                        # 缓存结果
                        self._list_time_cache[symbol] = earliest_ts
                        return earliest_ts

        except Exception as e:
            print(f"[OKX] {symbol} 获取实际数据起始时间失败: {e}")

        # 如果无法通过K线获取，尝试使用listTime作为fallback
        instruments = self._get_instruments_data()
        for inst in instruments:
            if inst.get("instId") == symbol:
                list_time = inst.get("listTime", "")
                if list_time:
                    list_time_int = int(list_time)
                    # 缓存结果
                    self._list_time_cache[symbol] = list_time_int
                    return list_time_int

        return None

    def convert_interval(self, interval: str) -> str:
        """转换周期格式到 OKX 格式

        日界对齐 (与 Binance / data_recorder 保持一致, 全部 UTC 0:00 对齐):
            - 1m ~ 4h: OKX 默认就是 UTC 对齐
            - 6h, 12h, 1d, 1w: OKX 默认按 UTC+8 (北京 0:00) 对齐, 必须用 *utc 后缀
        """
        mapping = {
            "1m": "1m",
            "3m": "3m",
            "5m": "5m",
            "15m": "15m",
            "30m": "30m",
            "1h": "1H",
            "2h": "2H",
            "4h": "4H",
            "6h": "6Hutc",
            "12h": "12Hutc",
            "1d": "1Dutc",
            "1w": "1Wutc",
        }
        return mapping.get(interval.lower(), interval)

    def get_klines(self, symbol: str, interval: str, start_time: int, end_time: int, limit: int = 300) -> List[Dict]:
        """获取 K 线数据

        OKX API 实际行为（经过测试验证）:
        - after=ts: 返回时间戳 < ts 的数据（倒序，最新的在前）
        - before=ts: 在查询历史数据时会被忽略，总是返回最新数据

        策略：使用 after 参数，传入 end_time + interval_ms，这样可以获取 <= end_time 的数据
        """
        url = f"{self.base_url}/api/v5/market/history-candles"
        okx_interval = self.convert_interval(interval)

        # 使用 after 参数来获取指定时间范围的数据
        # after=ts 实际返回时间戳 < ts 的数据（倒序，最新的在前）
        # 所以我们传入 end_time + interval_ms，这样可以包含 end_time 这根K线
        interval_ms = interval_to_ms(interval)
        params = {
            "instId": symbol,
            "bar": okx_interval,
            "after": str(end_time + interval_ms),  # 获取 < end_time+interval 的数据
            "limit": str(min(limit, 300))
        }

        max_retries = 3
        retry_delay = 10.0  # 初始重试延迟

        for attempt in range(max_retries):
            try:
                response = self.session.get(url, params=params, timeout=30)
                if response.status_code == 200:
                    data = response.json()
                    code = data.get("code")
                    if code == "0":
                        klines = []
                        # OKX 返回的数据是倒序的（最新的在前），需要反转
                        raw_data = data.get("data", [])
                        for item in reversed(raw_data):
                            if len(item) >= 6:
                                ts = int(item[0])
                                # 只保留在 [start_time, end_time] 范围内的K线
                                if start_time <= ts <= end_time:
                                    kline = {
                                        "timestamp": ts,
                                        "open": float(item[1]),
                                        "high": float(item[2]),
                                        "low": float(item[3]),
                                        "close": float(item[4]),
                                        "volume": float(item[5]),
                                        "symbol": symbol,
                                        "exchange": "okx",
                                        "interval": interval,
                                        "type": "kline"
                                    }
                                    klines.append(kline)
                        return klines
                    elif code == "50011":
                        # OKX 业务限流码
                        print(f"[OKX] {symbol} 请求过于频繁 (50011)")
                        if attempt < max_retries - 1:
                            wait_time = retry_delay * (2 ** attempt)
                            print(f"[OKX] {symbol} 等待 {wait_time:.1f} 秒后重试 (尝试 {attempt+1}/{max_retries})")
                            time.sleep(wait_time)
                            continue
                        else:
                            return []
                    else:
                        print(f"[OKX] {symbol} API 错误: code={code} {data.get('msg', 'Unknown error')}")
                        return []
                elif response.status_code == 429:
                    # HTTP 限流
                    print(f"[OKX] {symbol} 请求过于频繁 (429)")
                    if attempt < max_retries - 1:
                        wait_time = retry_delay * (2 ** attempt)
                        print(f"[OKX] {symbol} 等待 {wait_time:.1f} 秒后重试 (尝试 {attempt+1}/{max_retries})")
                        time.sleep(wait_time)
                        continue
                    else:
                        return []
                else:
                    print(f"[OKX] {symbol} API 返回 {response.status_code}: {response.text[:100]}")
                    return []
            except Exception as e:
                print(f"[OKX] {symbol} 获取 K 线失败: {e}")
                if attempt < max_retries - 1:
                    time.sleep(retry_delay)
                    continue
                return []

        return []


class RedisKlineStorage:
    """Redis K 线数据存储"""

    def __init__(self, host: str = REDIS_HOST, port: int = REDIS_PORT, password: str = REDIS_PASSWORD):
        self.host = host
        self.port = port
        self.password = password
        self.client: Optional[redis.Redis] = None

    def connect(self) -> bool:
        """连接到 Redis"""
        try:
            self.client = redis.Redis(
                host=self.host,
                port=self.port,
                password=self.password if self.password else None,
                decode_responses=True
            )
            self.client.ping()
            print(f"[Redis] 连接成功: {self.host}:{self.port}")
            return True
        except Exception as e:
            print(f"[Redis] 连接失败: {e}")
            return False

    def disconnect(self):
        """断开连接"""
        if self.client:
            self.client.close()
            self.client = None

    def store_klines(self, symbol: str, interval: str, exchange: str, klines: List[Dict]) -> int:
        """存储 K 线数据到 Redis（格式与data_recorder完全一致，自动去重）"""
        if not self.client or not klines:
            return 0

        key = f"kline:{exchange}:{symbol}:{interval}"

        try:
            # 获取已存在的时间戳，用于去重
            existing_timestamps = set()
            try:
                all_data = self.client.zrange(key, 0, -1, withscores=True)
                existing_timestamps = {int(score) for _, score in all_data}
            except:
                pass

            # 使用 pipeline 批量写入，跳过已存在的时间戳
            pipe = self.client.pipeline()
            stored_count = 0

            for kline in klines:
                timestamp = kline["timestamp"]
                # 只存储不存在的时间戳
                if timestamp not in existing_timestamps:
                    value = json.dumps(kline)
                    pipe.zadd(key, {value: timestamp})
                    stored_count += 1
                    existing_timestamps.add(timestamp)  # 更新已存在集合

            pipe.execute()
            return stored_count
        except Exception as e:
            print(f"[错误] 存储 {symbol} K 线失败: {e}")
            return 0

    def get_kline_count(self, symbol: str, interval: str, exchange: str) -> int:
        """获取已存储的 K 线数量"""
        if not self.client:
            return 0
        key = f"kline:{exchange}:{symbol}:{interval}"
        try:
            return self.client.zcard(key)
        except:
            return 0

    def get_latest_timestamp(self, symbol: str, interval: str, exchange: str) -> Optional[int]:
        """获取最新的 K 线时间戳"""
        if not self.client:
            return None
        key = f"kline:{exchange}:{symbol}:{interval}"
        try:
            result = self.client.zrevrange(key, 0, 0, withscores=True)
            if result:
                return int(result[0][1])
        except:
            pass
        return None

    def get_oldest_timestamp(self, symbol: str, interval: str, exchange: str) -> Optional[int]:
        """获取最早的 K 线时间戳"""
        if not self.client:
            return None
        key = f"kline:{exchange}:{symbol}:{interval}"
        try:
            result = self.client.zrange(key, 0, 0, withscores=True)
            if result:
                return int(result[0][1])
        except:
            pass
        return None

    def deduplicate_klines(self, symbol: str, interval: str, exchange: str) -> int:
        """
        去除重复的K线数据

        对于相同时间戳的K线，只保留一条（保留最后一条）

        Returns:
            删除的重复数据数量
        """
        if not self.client:
            return 0

        key = f"kline:{exchange}:{symbol}:{interval}"

        try:
            # 获取所有数据
            all_data = self.client.zrange(key, 0, -1, withscores=True)

            if len(all_data) == 0:
                return 0

            # 按时间戳分组，找出重复的
            from collections import defaultdict
            timestamp_groups = defaultdict(list)

            for value, score in all_data:
                timestamp = int(score)
                timestamp_groups[timestamp].append(value)

            # 统计重复数量
            duplicates_count = sum(len(values) - 1 for values in timestamp_groups.values() if len(values) > 1)

            if duplicates_count == 0:
                return 0

            # 删除所有数据，然后重新插入去重后的数据
            pipe = self.client.pipeline()

            # 删除整个key
            pipe.delete(key)

            # 重新插入去重后的数据（每个时间戳只保留最后一条）
            for timestamp, values in timestamp_groups.items():
                # 只保留最后一条
                value = values[-1]
                pipe.zadd(key, {value: timestamp})

            pipe.execute()

            return duplicates_count

        except Exception as e:
            print(f"[错误] 去重 {symbol} K 线失败: {e}")
            return 0

    def get_all_usdt_contracts(self) -> Dict[str, List[str]]:
        """
        从Redis扫描所有USDT合约

        Returns:
            {"okx": ["BTC-USDT-SWAP", ...], "binance": ["BTCUSDT", ...]}
        """
        if not self.client:
            return {}

        contracts = {"okx": [], "binance": []}

        try:
            # 扫描所有 kline:*:1m keys
            keys = self.client.keys("kline:*:1m")

            for key in keys:
                # key格式: kline:exchange:symbol:1m
                parts = key.split(":")
                if len(parts) != 4:
                    continue

                exchange = parts[1]
                symbol = parts[2]

                # 只处理USDT合约
                if exchange == "okx" and "-USDT-SWAP" in symbol:
                    contracts["okx"].append(symbol)
                elif exchange == "binance" and symbol.endswith("USDT"):
                    contracts["binance"].append(symbol)

            # 排序
            contracts["okx"] = sorted(contracts["okx"])
            contracts["binance"] = sorted(contracts["binance"])

        except Exception as e:
            print(f"[Redis] 扫描合约失败: {e}")

        return contracts


def interval_to_ms(interval: str) -> int:
    """将周期转换为毫秒"""
    interval = interval.lower()
    unit = interval[-1]
    value = int(interval[:-1])

    if unit == 'm':
        return value * 60 * 1000
    elif unit == 'h':
        return value * 60 * 60 * 1000
    elif unit == 'd':
        return value * 24 * 60 * 60 * 1000
    elif unit == 'w':
        return value * 7 * 24 * 60 * 60 * 1000
    else:
        return value * 60 * 1000  # 默认分钟


def check_continuity(storage: RedisKlineStorage, symbol: str, interval: str, exchange: str) -> Dict:
    """
    检查K线数据的连续性

    Args:
        storage: Redis存储
        symbol: 交易对
        interval: K线周期
        exchange: 交易所

    Returns:
        包含连续性信息的字典
    """
    key = f"kline:{exchange}:{symbol}:{interval}"
    interval_ms = interval_to_ms(interval)

    try:
        # 获取所有时间戳
        all_data = storage.client.zrange(key, 0, -1, withscores=True)
        if not all_data:
            return {
                "continuous": False,
                "count": 0,
                "missing": 0,
                "time_range": None,
                "message": "无数据"
            }

        timestamps = sorted([int(score) for _, score in all_data])
        count = len(timestamps)

        if count < 2:
            return {
                "continuous": True,
                "count": count,
                "missing": 0,
                "time_range": (timestamps[0], timestamps[0]),
                "message": "数据不足2条，无法检测连续性"
            }

        # 计算应有的K线数量
        time_span_ms = timestamps[-1] - timestamps[0]
        expected_count = (time_span_ms // interval_ms) + 1
        missing_count = expected_count - count

        # 检测间隔
        gaps = []
        for i in range(1, len(timestamps)):
            gap_ms = timestamps[i] - timestamps[i-1]
            if gap_ms > interval_ms * 1.5:  # 允许一定误差
                gap_count = (gap_ms // interval_ms) - 1
                if gap_count > 0:
                    gaps.append({
                        'start': timestamps[i-1],
                        'end': timestamps[i],
                        'count': gap_count
                    })

        continuous = (missing_count == 0 and len(gaps) == 0)

        start_time = datetime.fromtimestamp(timestamps[0]/1000).strftime('%Y-%m-%d %H:%M:%S')
        end_time = datetime.fromtimestamp(timestamps[-1]/1000).strftime('%Y-%m-%d %H:%M:%S')

        if continuous:
            message = f"✓ 连续 ({count} 根)"
        else:
            message = f"✗ 缺失 {missing_count} 根 (应有 {expected_count} 根)"

        return {
            "continuous": continuous,
            "count": count,
            "expected": expected_count,
            "missing": missing_count,
            "gaps": len(gaps),
            "time_range": (timestamps[0], timestamps[-1]),
            "time_range_str": f"{start_time} ~ {end_time}",
            "message": message
        }

    except Exception as e:
        return {
            "continuous": False,
            "count": 0,
            "missing": 0,
            "time_range": None,
            "message": f"检测失败: {e}"
        }


def load_symbol_klines(
    loader: BaseKlineLoader,
    storage: RedisKlineStorage,
    symbol: str,
    interval: str,
    days: int
) -> int:
    """加载单个交易对的 K 线数据"""
    total_loaded = 0
    interval_ms = interval_to_ms(interval)
    exchange = loader.exchange_name
    batch_size = loader.batch_size

    # 计算时间范围
    end_time = int(datetime.now().timestamp() * 1000)
    start_time = end_time - days * 24 * 60 * 60 * 1000

    # 🆕 检查合约上线时间，如果上线时间晚于计算的start_time，则使用上线时间
    list_time = loader.get_symbol_list_time(symbol)
    if list_time:
        list_time_dt = datetime.fromtimestamp(list_time / 1000)
        original_start_dt = datetime.fromtimestamp(start_time / 1000)

        if list_time > start_time:
            # 合约上线时间晚于请求的开始时间，使用上线时间
            days_since_list = (end_time - list_time) / (24 * 60 * 60 * 1000)
            print(f"[{exchange}] {symbol} 上线时间: {list_time_dt.strftime('%Y-%m-%d %H:%M:%S')} "
                  f"(距今 {days_since_list:.1f} 天，少于请求的 {days} 天)")
            print(f"[{exchange}] {symbol} 调整开始时间: {original_start_dt.strftime('%Y-%m-%d %H:%M:%S')} "
                  f"-> {list_time_dt.strftime('%Y-%m-%d %H:%M:%S')}")
            start_time = list_time
        else:
            # 合约上线时间早于请求的开始时间，正常拉取
            print(f"[{exchange}] {symbol} 上线时间: {list_time_dt.strftime('%Y-%m-%d %H:%M:%S')} "
                  f"(早于请求的 {days} 天，正常拉取)")
    else:
        print(f"[{exchange}] {symbol} 无法获取上线时间，使用默认时间范围")

    # 检查已有数据
    existing_count = storage.get_kline_count(symbol, interval, exchange)
    latest_ts = storage.get_latest_timestamp(symbol, interval, exchange)
    oldest_ts = storage.get_oldest_timestamp(symbol, interval, exchange)

    # 智能加载：检查是否需要加载更早的数据或更新最新数据
    need_load_old = True
    need_load_new = True

    if oldest_ts and oldest_ts <= start_time:
        need_load_old = False  # 已有足够早的数据

    if latest_ts:
        # 只加载最新的缺失数据
        if latest_ts > end_time - interval_ms * 2:
            need_load_new = False  # 数据已是最新

    if not need_load_old and not need_load_new:
        return 0  # 数据已完整

    # 加载更早的历史数据
    if need_load_old and oldest_ts:
        # 确保不会尝试拉取早于合约上线时间的数据
        if oldest_ts > start_time:
            current_end = oldest_ts - interval_ms
            current_start = max(start_time, current_end - batch_size * interval_ms)

            while current_start < current_end and current_start >= start_time:
                klines = loader.get_klines(symbol, interval, current_start, current_end, batch_size)

                if klines:
                    stored = storage.store_klines(symbol, interval, exchange, klines)
                    total_loaded += stored
                    current_end = klines[0]["timestamp"] - interval_ms
                    current_start = max(start_time, current_end - batch_size * interval_ms)
                else:
                    # 如果拉取失败，可能是请求的时间早于合约上线时间
                    # 尝试调整current_start，避免继续请求无效数据
                    print(f"[{exchange}] {symbol} 拉取失败，可能请求时间早于数据起始时间，停止向前拉取")
                    break

                # 根据交易所类型使用不同的延迟
                delay = BINANCE_REQUEST_DELAY if exchange == 'binance' else REQUEST_DELAY
                time.sleep(delay)
        else:
            print(f"[{exchange}] {symbol} 已有数据的起始时间早于或等于目标时间，无需向前拉取")

    # 加载最新数据
    if need_load_new:
        if latest_ts:
            current_start = latest_ts + interval_ms
        else:
            current_start = start_time

        while current_start < end_time:
            current_end = min(current_start + batch_size * interval_ms, end_time)

            klines = loader.get_klines(symbol, interval, current_start, current_end, batch_size)

            if klines:
                stored = storage.store_klines(symbol, interval, exchange, klines)
                total_loaded += stored
                current_start = klines[-1]["timestamp"] + interval_ms
            else:
                current_start = current_end

            # 根据交易所类型使用不同的延迟
            delay = BINANCE_REQUEST_DELAY if exchange == 'binance' else REQUEST_DELAY
            time.sleep(delay)

    return total_loaded


def load_historical_before_oldest(
    loader: BaseKlineLoader,
    storage: RedisKlineStorage,
    symbol: str,
    interval: str,
    days: int
) -> int:
    """
    从最早的K线时间戳往前加载历史数据

    Args:
        loader: K线加载器
        storage: Redis存储
        symbol: 交易对
        interval: K线周期
        days: 往前加载的天数

    Returns:
        加载的K线数量
    """
    total_loaded = 0
    interval_ms = interval_to_ms(interval)
    exchange = loader.exchange_name
    batch_size = loader.batch_size

    # 获取当前最早的K线时间戳
    oldest_ts = storage.get_oldest_timestamp(symbol, interval, exchange)

    if not oldest_ts:
        print(f"[{exchange.upper()}] {symbol}:{interval} - Redis中无数据，跳过")
        return 0

    # 计算目标时间范围：从最早时间戳往前推days天
    end_time = oldest_ts - interval_ms  # 从最早K线的前一根开始
    start_time = end_time - days * 24 * 60 * 60 * 1000

    print(f"[{exchange.upper()}] {symbol}:{interval}")
    print(f"  当前最早: {datetime.fromtimestamp(oldest_ts/1000).strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"  目标范围: {datetime.fromtimestamp(start_time/1000).strftime('%Y-%m-%d %H:%M:%S')} ~ {datetime.fromtimestamp(end_time/1000).strftime('%Y-%m-%d %H:%M:%S')}")

    # 从end_time往前批量拉取
    current_end = end_time
    current_start = max(start_time, current_end - batch_size * interval_ms)

    while current_start < current_end and current_start >= start_time:
        klines = loader.get_klines(symbol, interval, current_start, current_end, batch_size)

        if klines:
            stored = storage.store_klines(symbol, interval, exchange, klines)
            total_loaded += stored
            print(f"  拉取: {datetime.fromtimestamp(klines[0]['timestamp']/1000).strftime('%Y-%m-%d %H:%M:%S')} ~ {datetime.fromtimestamp(klines[-1]['timestamp']/1000).strftime('%Y-%m-%d %H:%M:%S')} (+{stored}根)")

            # 更新范围，继续往前拉取
            current_end = klines[0]["timestamp"] - interval_ms
            current_start = max(start_time, current_end - batch_size * interval_ms)
        else:
            print(f"  拉取失败，停止")
            break

        # 根据交易所类型使用不同的延迟
        delay = BINANCE_REQUEST_DELAY if exchange == 'binance' else REQUEST_DELAY
        time.sleep(delay)

    if total_loaded > 0:
        print(f"  ✓ 完成: 共加载 {total_loaded} 根K线")
    else:
        print(f"  无新数据")

    return total_loaded


def fill_gaps_in_existing_data(
    loader: BaseKlineLoader,
    storage: RedisKlineStorage,
    symbol: str,
    interval: str
) -> int:
    """
    填补现有数据中的间隔

    Args:
        loader: K线加载器
        storage: Redis存储
        symbol: 交易对
        interval: K线周期

    Returns:
        填补的K线数量
    """
    total_loaded = 0
    interval_ms = interval_to_ms(interval)
    exchange = loader.exchange_name
    batch_size = loader.batch_size

    # 获取所有现有K线的时间戳
    key = f"kline:{exchange}:{symbol}:{interval}"
    try:
        # 获取所有时间戳
        all_data = storage.client.zrange(key, 0, -1, withscores=True)
        if not all_data or len(all_data) < 2:
            return 0

        timestamps = sorted([int(score) for _, score in all_data])

        print(f"[{exchange. upper()}] {symbol}:{interval} - 检测间隔")
        print(f"  现有K线数: {len(timestamps)}")

        # 检测间隔（大于2倍interval的视为间隔）
        gaps = []
        for i in range(1, len(timestamps)):
            gap_ms = timestamps[i] - timestamps[i-1]
            if gap_ms > interval_ms * 2:  # 间隔大于2倍周期
                gap_count = (gap_ms // interval_ms) - 1
                if gap_count > 0:
                    gaps.append({
                        'start': timestamps[i-1] + interval_ms,
                        'end': timestamps[i] - interval_ms,
                        'count': gap_count
                    })

        if not gaps:
            print(f"  ✓ 数据连续，无需填补")
            return 0

        print(f"  发现 {len(gaps)} 个间隔，共缺失 {sum(g['count'] for g in gaps)} 根K线")

        # 填补每个间隔
        for idx, gap in enumerate(gaps, 1):
            gap_start = gap['start']
            gap_end = gap['end']

            print(f"  [{idx}/{len(gaps)}] 填补间隔: {datetime.fromtimestamp(gap_start/1000).strftime('%Y-%m-%d %H:%M:%S')} ~ {datetime.fromtimestamp(gap_end/1000).strftime('%Y-%m-%d %H:%M:%S')} ({gap['count']}根)")

            # 分批拉取
            current_start = gap_start
            while current_start <= gap_end:
                current_end = min(current_start + (batch_size - 1) * interval_ms, gap_end)

                klines = loader.get_klines(symbol, interval, current_start, current_end, batch_size)

                if klines and len(klines) > 0:
                    stored = storage.store_klines(symbol, interval, exchange, klines)
                    total_loaded += stored
                    print(f"    拉取: {datetime.fromtimestamp(klines[0]['timestamp']/1000).strftime('%H:%M:%S')} ~ {datetime.fromtimestamp(klines[-1]['timestamp']/1000).strftime('%H:%M:%S')} (+{stored}根)")

                    # 更新起始位置：从最后一根K线的下一根开始
                    last_ts = klines[-1]["timestamp"]
                    current_start = last_ts + interval_ms

                    # 如果已经超过gap_end，退出循环
                    if last_ts >= gap_end:
                        break
                else:
                    print(f"    拉取失败，跳过此间隔")
                    break

                # 根据交易所类型使用不同的延迟
                delay = BINANCE_REQUEST_DELAY if exchange == 'binance' else REQUEST_DELAY
                time.sleep(delay)

        if total_loaded > 0:
            print(f"  ✓ 完成: 共填补 {total_loaded} 根K线")

    except Exception as e:
        print(f"  ✗ 填补失败: {e}")

    return total_loaded


def load_from_current_time(
    loader: BaseKlineLoader,
    storage: RedisKlineStorage,
    symbol: str,
    interval: str,
    days: int
) -> int:
    """
    从当前时间往前拉取指定天数的K线数据（智能检测，只拉取缺失部分）

    Args:
        loader: K线加载器
        storage: Redis存储
        symbol: 交易对
        interval: K线周期
        days: 往前拉取的天数

    Returns:
        加载的K线数量
    """
    total_loaded = 0
    interval_ms = interval_to_ms(interval)
    exchange = loader.exchange_name
    batch_size = loader.batch_size

    # 计算目标时间范围：从当前时间往前推days天，对齐到interval边界
    current_time = int(time.time() * 1000)
    # 对齐到interval边界（向下取整）
    end_time = (current_time // interval_ms) * interval_ms
    start_time = end_time - days * 24 * 60 * 60 * 1000

    # 🆕 检查合约上线时间，如果上线时间晚于计算的start_time，则使用上线时间
    list_time = loader.get_symbol_list_time(symbol)
    if list_time:
        list_time_dt = datetime.fromtimestamp(list_time / 1000)
        original_start_dt = datetime.fromtimestamp(start_time / 1000)

        if list_time > start_time:
            # 合约上线时间晚于请求的开始时间，使用上线时间
            days_since_list = (end_time - list_time) / (24 * 60 * 60 * 1000)
            print(f"  ⚠️  合约上线时间: {list_time_dt.strftime('%Y-%m-%d %H:%M:%S')} (距今 {days_since_list:.1f} 天)")
            print(f"  ⚠️  调整开始时间: {original_start_dt.strftime('%Y-%m-%d %H:%M:%S')} -> {list_time_dt.strftime('%Y-%m-%d %H:%M:%S')}")
            start_time = list_time
        else:
            # 合约上线时间早于请求的开始时间，正常拉取
            print(f"  ℹ️  合约上线时间: {list_time_dt.strftime('%Y-%m-%d %H:%M:%S')} (早于请求的 {days} 天)")
    else:
        print(f"  ⚠️  无法获取上线时间，使用默认时间范围")

    print(f"  目标范围: {datetime.fromtimestamp(start_time/1000).strftime('%Y-%m-%d %H:%M:%S')} ~ {datetime.fromtimestamp(end_time/1000).strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"  目标天数: {days} 天")

    # 步骤1: 删除超出时间范围的旧数据
    key = f"kline:{exchange}:{symbol}:{interval}"
    try:
        # 删除早于start_time的数据
        deleted_old = storage.client.zremrangebyscore(key, '-inf', start_time - 1)
        # 删除晚于end_time的数据（未来数据）
        deleted_future = storage.client.zremrangebyscore(key, end_time + 1, '+inf')

        total_deleted = deleted_old + deleted_future
        if total_deleted > 0:
            print(f"  清理数据: 删除 {deleted_old} 根旧数据, {deleted_future} 根未来数据")
    except Exception as e:
        print(f"  清理数据失败: {e}")

    # 步骤2: 检测现有数据，找出缺失的时间段
    try:
        all_data = storage.client.zrangebyscore(key, start_time, end_time, withscores=True)
        existing_timestamps = set(int(score) for _, score in all_data)

        if len(existing_timestamps) > 0:
            print(f"  现有数据: {len(existing_timestamps)} 根K线")

        # 计算应有的所有时间戳
        expected_timestamps = set()
        current_ts = start_time
        while current_ts <= end_time:
            expected_timestamps.add(current_ts)
            current_ts += interval_ms

        # 找出缺失的时间戳
        missing_timestamps = sorted(expected_timestamps - existing_timestamps)

        if len(missing_timestamps) == 0:
            print(f"  ✓ 数据完整，无需拉取")
            return 0

        print(f"  缺失数据: {len(missing_timestamps)} 根K线")

        # 将缺失的时间戳合并为连续的时间段
        gaps = []
        if missing_timestamps:
            gap_start = missing_timestamps[0]
            gap_end = missing_timestamps[0]

            for ts in missing_timestamps[1:]:
                if ts - gap_end <= interval_ms * 1.5:  # 连续
                    gap_end = ts
                else:  # 新的间隔
                    gaps.append((gap_start, gap_end))
                    gap_start = ts
                    gap_end = ts

            gaps.append((gap_start, gap_end))

        print(f"  需要拉取 {len(gaps)} 个时间段:")
        for idx, (gap_start, gap_end) in enumerate(gaps, 1):
            gap_count = len([ts for ts in missing_timestamps if gap_start <= ts <= gap_end])
            start_str = datetime.fromtimestamp(gap_start/1000).strftime('%Y-%m-%d %H:%M:%S')
            end_str = datetime.fromtimestamp(gap_end/1000).strftime('%Y-%m-%d %H:%M:%S')
            print(f"    [{idx}] {start_str} ~ {end_str} ({gap_count}根)")

        # 步骤3: 批量拉取缺失的时间段
        for idx, (gap_start, gap_end) in enumerate(gaps, 1):
            gap_count = len([ts for ts in missing_timestamps if gap_start <= ts <= gap_end])
            start_str = datetime.fromtimestamp(gap_start/1000).strftime('%Y-%m-%d %H:%M:%S')
            end_str = datetime.fromtimestamp(gap_end/1000).strftime('%Y-%m-%d %H:%M:%S')

            # 检查这个gap是否在合约上线时间之前
            if list_time and gap_end < list_time:
                print(f"  [{idx}/{len(gaps)}] 跳过: {start_str} ~ {end_str} (早于合约上线时间)")
                continue

            # 如果gap的开始时间早于合约上线时间，调整为从上线时间开始
            if list_time and gap_start < list_time:
                original_start_str = start_str
                gap_start = list_time
                start_str = datetime.fromtimestamp(gap_start/1000).strftime('%Y-%m-%d %H:%M:%S')
                print(f"  [{idx}/{len(gaps)}] 调整起始: {original_start_str} -> {start_str}")

            print(f"  [{idx}/{len(gaps)}] 开始拉取: {start_str} ~ {end_str}")

            # 分批拉取这个时间段
            current_start = gap_start
            batch_num = 0
            consecutive_failures = 0  # 连续失败计数
            max_consecutive_failures = 3  # 最多允许3次连续失败

            while current_start <= gap_end:
                current_end = min(current_start + (batch_size - 1) * interval_ms, gap_end)
                batch_num += 1

                klines = loader.get_klines(symbol, interval, current_start, current_end, batch_size)

                if klines:
                    stored = storage.store_klines(symbol, interval, exchange, klines)
                    total_loaded += stored
                    batch_start_str = datetime.fromtimestamp(klines[0]['timestamp']/1000).strftime('%H:%M:%S')
                    batch_end_str = datetime.fromtimestamp(klines[-1]['timestamp']/1000).strftime('%H:%M:%S')
                    print(f"    批次{batch_num}: {batch_start_str} ~ {batch_end_str} (+{stored}根)")

                    consecutive_failures = 0  # 重置失败计数

                    current_start = klines[-1]["timestamp"] + interval_ms
                    if current_start > gap_end:
                        break
                else:
                    consecutive_failures += 1
                    print(f"    批次{batch_num}: 拉取失败 (连续失败{consecutive_failures}次)")

                    # 如果连续失败次数过多，停止这个gap的拉取
                    if consecutive_failures >= max_consecutive_failures:
                        print(f"    连续失败{max_consecutive_failures}次，停止拉取此时间段")
                        break

                    # 否则跳过这个批次，继续下一个
                    current_start = current_end + interval_ms

                # 根据交易所类型使用不同的延迟
                delay = BINANCE_REQUEST_DELAY if exchange == 'binance' else REQUEST_DELAY
                time.sleep(delay)

        if total_loaded > 0:
            print(f"  ✓ 完成: 共加载 {total_loaded} 根K线")

    except Exception as e:
        print(f"  ✗ 处理失败: {e}")
        import traceback
        traceback.print_exc()

    return total_loaded


def process_single_contract(
    loader: BaseKlineLoader,
    storage: RedisKlineStorage,
    symbol: str,
    interval: str,
    days: int,
    index: int,
    total: int
) -> tuple:
    """
    处理单个合约的数据加载和连续性检测

    Returns:
        (symbol, interval, loaded_count, continuity_result)
    """
    try:
        print(f"\n{'='*60}")
        print(f"[{index}/{total}] 处理合约: {symbol} | 周期: {interval}")
        print(f"{'='*60}")

        # 步骤1: 去重现有数据
        print(f"\n  【去重检查】")
        dedup_count = storage.deduplicate_klines(symbol, interval, loader.exchange_name)
        if dedup_count > 0:
            print(f"  ✓ 删除重复数据: {dedup_count} 根")
        else:
            print(f"  ✓ 无重复数据")

        # 步骤2: 从当前时间往前加载数据
        loaded = load_from_current_time(
            loader, storage, symbol, interval, days
        )

        # 步骤3: 连续性检测
        print(f"\n  【连续性检测】")
        continuity = check_continuity(storage, symbol, interval, loader.exchange_name)
        print(f"  状态: {continuity['message']}")
        if continuity['time_range_str']:
            print(f"  时间范围: {continuity['time_range_str']}")
        print(f"{'='*60}")

        return (symbol, interval, loaded, continuity)

    except Exception as e:
        print(f"  ✗ 失败: {e}")
        import traceback
        traceback.print_exc()
        return (symbol, interval, 0, None)


def load_exchange_data(
    loader: BaseKlineLoader,
    storage: RedisKlineStorage,
    symbols: List[str],
    interval: str,
    days: int,
    workers: int
) -> tuple:
    """加载单个交易所的数据"""
    exchange = loader.exchange_name
    print(f"\n[{exchange.upper()}] 开始加载 {len(symbols)} 个交易对的 {days} 天 {interval} K 线数据...")

    start_time = time.time()
    total_klines = 0
    success_count = 0
    failed_symbols = []

    # 使用线程池并发加载
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {}
        for symbol in symbols:
            future = executor.submit(
                load_symbol_klines,
                loader, storage, symbol, interval, days
            )
            futures[future] = symbol

        for i, future in enumerate(as_completed(futures)):
            symbol = futures[future]
            try:
                loaded = future.result()
                if loaded > 0:
                    total_klines += loaded
                    success_count += 1
                    print(f"[{exchange.upper()}] [{i+1}/{len(symbols)}] {symbol}: +{loaded} 条")
                else:
                    # 检查是否已有数据
                    existing = storage.get_kline_count(symbol, interval, exchange)
                    if existing > 0:
                        success_count += 1
                        print(f"[{exchange.upper()}] [{i+1}/{len(symbols)}] {symbol}: 已是最新 ({existing} 条)")
                    else:
                        failed_symbols.append(symbol)
                        print(f"[{exchange.upper()}] [{i+1}/{len(symbols)}] {symbol}: 无数据")
            except Exception as e:
                failed_symbols.append(symbol)
                print(f"[{exchange.upper()}] [{i+1}/{len(symbols)}] {symbol}: 失败 - {e}")

    elapsed = time.time() - start_time
    return total_klines, success_count, failed_symbols, elapsed


def load_latest_exchange_data(
    loader: BaseKlineLoader,
    storage: RedisKlineStorage,
    symbols: List[str],
    interval: str,
    latest_count: int,
    workers: int
) -> Dict:
    """加载每个交易对最近已收盘的 N 根 K 线，用于快速预热 Redis。"""
    exchange = loader.exchange_name
    interval_ms = interval_to_ms(interval)
    latest_count = max(1, latest_count)

    # 排除当前未收盘K线，只拉最近已收盘的K线。
    end_time = (int(datetime.now().timestamp() * 1000) // interval_ms) * interval_ms - interval_ms
    start_time = end_time - (latest_count - 1) * interval_ms

    print(f"\n[{exchange.upper()}] 预热 {len(symbols)} 个交易对最近 {latest_count} 根 {interval} K线")
    print(f"  时间范围: {datetime.fromtimestamp(start_time/1000).strftime('%Y-%m-%d %H:%M:%S')} ~ "
          f"{datetime.fromtimestamp(end_time/1000).strftime('%Y-%m-%d %H:%M:%S')}")

    start_clock = time.time()
    total_loaded = 0
    success_count = 0
    failed_symbols = []

    def load_one(symbol: str) -> tuple:
        try:
            if exchange == "okx":
                time.sleep(REQUEST_DELAY)
            klines = loader.get_klines(symbol, interval, start_time, end_time, latest_count)
            stored = storage.store_klines(symbol, interval, exchange, klines)
            return symbol, stored, len(klines), None
        except Exception as e:
            return symbol, 0, 0, e

    effective_workers = 1 if exchange == "okx" else workers
    with ThreadPoolExecutor(max_workers=effective_workers) as executor:
        futures = {executor.submit(load_one, symbol): symbol for symbol in symbols}
        for i, future in enumerate(as_completed(futures), 1):
            symbol, stored, fetched, error = future.result()
            if error:
                failed_symbols.append(symbol)
                print(f"[{exchange.upper()}] [{i}/{len(symbols)}] {symbol}: 失败 - {error}")
            elif fetched > 0:
                success_count += 1
                total_loaded += stored
                print(f"[{exchange.upper()}] [{i}/{len(symbols)}] {symbol}: 获取 {fetched} 根, 新写入 {stored} 根")
            else:
                failed_symbols.append(symbol)
                print(f"[{exchange.upper()}] [{i}/{len(symbols)}] {symbol}: 无数据")

    elapsed = time.time() - start_clock
    print(f"\n[{exchange.upper()}] 最近K线预热完成")
    print(f"  成功: {success_count}/{len(symbols)}")
    print(f"  失败/无数据: {len(failed_symbols)}")
    print(f"  新写入: {total_loaded} 根")
    print(f"  耗时: {elapsed:.1f}s")

    return {
        "total": total_loaded,
        "success": success_count,
        "failed": failed_symbols,
        "elapsed": elapsed,
        "symbols": symbols
    }


def aggregate_klines_from_1m(
    storage: RedisKlineStorage,
    symbol: str,
    exchange: str,
    target_interval: str
) -> int:
    """
    从1min K线聚合生成其他周期的K线

    Args:
        storage: Redis存储
        symbol: 交易对
        exchange: 交易所
        target_interval: 目标周期 (5m/15m/30m)

    Returns:
        生成的K线数量
    """
    if target_interval not in ["5m", "15m", "30m"]:
        print(f"  ✗ 不支持的聚合周期: {target_interval}")
        return 0

    # 计算聚合倍数
    multiplier = {
        "5m": 5,
        "15m": 15,
        "30m": 30
    }[target_interval]

    source_key = f"kline:{exchange}:{symbol}:1m"
    target_key = f"kline:{exchange}:{symbol}:{target_interval}"

    try:
        # 获取所有1min K线数据
        all_data = storage.client.zrange(source_key, 0, -1, withscores=True)

        if not all_data:
            return 0

        # 解析K线数据
        klines_1m = []
        for value, score in all_data:
            kline = json.loads(value)
            kline['timestamp'] = int(score)
            klines_1m.append(kline)

        # 按时间戳排序
        klines_1m.sort(key=lambda x: x['timestamp'])

        # 聚合K线
        interval_ms = multiplier * 60 * 1000
        aggregated = {}

        for kline in klines_1m:
            ts = kline['timestamp']
            # 对齐到目标周期边界
            aligned_ts = (ts // interval_ms) * interval_ms

            if aligned_ts not in aggregated:
                # 创建新的聚合K线
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
                    'interval': target_interval,
                    'type': 'kline'
                }
            else:
                # 更新聚合K线
                agg = aggregated[aligned_ts]
                agg['high'] = max(agg['high'], kline['high'])
                agg['low'] = min(agg['low'], kline['low'])
                agg['close'] = kline['close']  # 最后一根的close
                agg['volume'] += kline['volume']
                agg['amount'] += kline.get('amount', 0.0)
                agg['buy_amount'] += kline.get('buy_amount', 0.0)

        # 存储聚合后的K线（使用pipeline批量写入）
        if aggregated:
            pipe = storage.client.pipeline()
            for ts, kline in aggregated.items():
                value = json.dumps(kline)
                pipe.zadd(target_key, {value: ts})
            pipe.execute()

            # 去重
            dedup_count = storage.deduplicate_klines(symbol, target_interval, exchange)

            return len(aggregated)

        return 0

    except Exception as e:
        print(f"  ✗ 聚合失败: {e}")
        return 0


def aggregate_all_intervals(
    storage: RedisKlineStorage,
    symbol: str,
    exchange: str,
    target_intervals: List[str]
) -> Dict[str, int]:
    """
    从1min聚合生成多个周期的K线

    Args:
        storage: Redis存储
        symbol: 交易对
        exchange: 交易所
        target_intervals: 目标周期列表 (如 ["5m", "15m", "30m"])

    Returns:
        每个周期生成的K线数量
    """
    results = {}

    for interval in target_intervals:
        count = aggregate_klines_from_1m(storage, symbol, exchange, interval)
        results[interval] = count
        if count > 0:
            print(f"  ✓ {interval}: 聚合生成 {count} 根K线")
        else:
            print(f"  - {interval}: 无数据或聚合失败")

    return results


def deduplicate_all_contracts(
    storage: RedisKlineStorage,
    exchange: str = "all",
    interval: str = "all",
    workers: int = MAX_WORKERS
) -> None:
    """
    批量去重所有合约的K线数据

    Args:
        storage: Redis存储
        exchange: 交易所 (okx/binance/all)
        interval: K线周期 (1m/5m/15m/30m/1h/all)
        workers: 并发线程数
    """
    print("=" * 60)
    print("       K 线数据批量去重工具")
    print("=" * 60)
    print()

    # 获取所有合约
    all_contracts = storage.get_all_usdt_contracts()

    # 筛选交易所
    if exchange == "all":
        exchanges = list(all_contracts.keys())
    else:
        exchanges = [exchange] if exchange in all_contracts else []

    if not exchanges:
        print("未找到任何合约")
        return

    # 确定要处理的周期
    if interval == "all":
        intervals = ["1m", "5m", "15m", "30m", "1h"]
    else:
        intervals = [interval]

    total_dedup = 0
    total_processed = 0

    for exch in exchanges:
        symbols = all_contracts.get(exch, [])
        if not symbols:
            continue

        print(f"\n[{exch.upper()}] 开始去重 {len(symbols)} 个合约...")

        for intv in intervals:
            print(f"\n  周期: {intv}")
            print(f"  {'='*56}")

            dedup_count = 0
            processed = 0

            # 使用线程池并发去重
            with ThreadPoolExecutor(max_workers=workers) as executor:
                futures = {}
                for symbol in symbols:
                    future = executor.submit(
                        storage.deduplicate_klines,
                        symbol, intv, exch
                    )
                    futures[future] = symbol

                for i, future in enumerate(as_completed(futures)):
                    symbol = futures[future]
                    try:
                        removed = future.result()
                        processed += 1
                        if removed > 0:
                            dedup_count += removed
                            print(f"  [{i+1}/{len(symbols)}] {symbol}: 删除 {removed} 根重复数据")
                        else:
                            # 只在有重复时才打印
                            pass
                    except Exception as e:
                        print(f"  [{i+1}/{len(symbols)}] {symbol}: 失败 - {e}")

            if dedup_count > 0:
                print(f"  ✓ {intv} 周期: 共删除 {dedup_count} 根重复数据")
            else:
                print(f"  ✓ {intv} 周期: 无重复数据")

            total_dedup += dedup_count
            total_processed += processed

    print(f"\n{'='*60}")
    print(f"去重完成:")
    print(f"  处理合约数: {total_processed}")
    print(f"  删除重复数: {total_dedup} 根")
    print(f"{'='*60}")


def main():
    parser = argparse.ArgumentParser(description="预加载 K 线数据到 Redis")
    parser.add_argument("--mode", type=str, default="current", choices=["auto", "manual", "current", "deduplicate", "latest"],
                        help="运行模式: current=从当前时间往前加载(默认), auto=从Redis最早时间戳往前加载, manual=手动指定参数, latest=只拉最近N根, deduplicate=批量去重")
    parser.add_argument("--days", type=int, default=60, help="加载天数 (默认: 60)")
    parser.add_argument("--interval", type=str, default="1m", help="K 线周期 (默认: 1m, deduplicate模式可用all)")
    parser.add_argument("--exchange", type=str, default="all", choices=["binance", "okx", "all"],
                        help="交易所 (默认: all)")
    parser.add_argument("--testnet", action="store_true", help="使用测试网")
    parser.add_argument("--symbols", type=str, default="", help="指定交易对，逗号分隔 (默认: 全部)")
    parser.add_argument("--workers", type=int, default=MAX_WORKERS, help=f"并发线程数 (默认: {MAX_WORKERS})")
    parser.add_argument("--latest-count", type=int, default=1, help="latest模式加载最近N根K线 (默认: 1)")
    parser.add_argument("--proxy", action="store_true", help="启用代理")
    parser.add_argument("--no-proxy", action="store_true", help="不使用代理")
    args = parser.parse_args()

    # 如果是去重模式，直接执行去重
    if args.mode == "deduplicate":
        storage = RedisKlineStorage()
        if not storage.connect():
            print("Redis连接失败，退出")
            return

        deduplicate_all_contracts(
            storage,
            exchange=args.exchange,
            interval=args.interval,
            workers=args.workers
        )
        return

    print("=" * 60)
    print("       K 线数据预加载工具")
    print("=" * 60)
    print()
    print(f"  模式: {args.mode.upper()}")
    print(f"  交易所: {args.exchange.upper()}")
    print(f"  网络: {'测试网' if args.testnet else '主网'}")
    print(f"  并发: {args.workers} 线程")
    use_proxy = args.proxy or (DEFAULT_USE_PROXY and not args.no_proxy)
    print(f"  代理: {'启用' if use_proxy else '禁用'}")
    print(f"  Redis: {REDIS_HOST}:{REDIS_PORT}")
    print()

    if args.mode == "current":
        print("  当前时间模式说明:")
        print("  - 从Redis扫描所有USDT合约")
        print("  - 从当前时间往前加载:")
        print("    * 1m: 2个月（直接拉取）")
        print("    * 1h: 6个月（直接拉取）")
        print("    * 5m/15m/30m: 从1min聚合生成")
        print("  - 每个合约加载完成后进行连续性检测")
    elif args.mode == "auto":
        print("  自动模式说明:")
        print("  - 从Redis扫描所有USDT合约")
        print("  - 检测每个合约的最早K线时间戳")
        print("  - 从最早时间戳往前加载:")
        print("    * 1m: 2个月（直接拉取）")
        print("    * 1h: 6个月（直接拉取）")
        print("    * 5m/15m/30m: 从1min聚合生成")
    elif args.mode == "latest":
        print("  最近K线预热模式:")
        print(f"  - 拉取全市场USDT永续最近 {args.latest_count} 根 {args.interval} K线")
        print("  - 不扫描Redis历史，不查询合约上线时间")
    else:
        print(f"  手动模式: {args.days} 天, {args.interval}")

    print()
    print("-" * 60)

    # 初始化 Redis
    storage = RedisKlineStorage()
    if not storage.connect():
        print("[错误] Redis 连接失败，退出")
        return 1

    results = {}

    # ==================== 最近K线预热模式 ====================
    if args.mode == "latest":
        if args.exchange in ["binance", "all"]:
            loader = BinanceKlineLoader(testnet=args.testnet, use_proxy=use_proxy)
            if args.symbols:
                symbols = [s.strip().upper() for s in args.symbols.split(",")]
                symbols = [s.replace("-", "").replace("SWAP", "") for s in symbols]
                print(f"[Binance] 使用指定的 {len(symbols)} 个交易对")
            else:
                print("[Binance] 获取交易对列表...")
                symbols = [s for s in loader.get_exchange_info() if s.endswith("USDT")]
                print(f"[Binance] 获取到 {len(symbols)} 个永续合约交易对")

            if symbols:
                results["binance"] = load_latest_exchange_data(
                    loader, storage, symbols, args.interval, args.latest_count, args.workers
                )

        if args.exchange in ["okx", "all"]:
            loader = OKXKlineLoader(testnet=args.testnet, use_proxy=use_proxy)
            if args.symbols:
                symbols = [s.strip().upper() for s in args.symbols.split(",")]
                okx_symbols = []
                for s in symbols:
                    if "-SWAP" in s:
                        okx_symbols.append(s)
                    elif s.endswith("USDT"):
                        base = s[:-4]
                        okx_symbols.append(f"{base}-USDT-SWAP")
                    else:
                        okx_symbols.append(s)
                symbols = okx_symbols
                print(f"[OKX] 使用指定的 {len(symbols)} 个交易对")
            else:
                print("[OKX] 获取交易对列表...")
                symbols = loader.get_exchange_info()
                print(f"[OKX] 获取到 {len(symbols)} 个永续合约交易对")

            if symbols:
                results["okx"] = load_latest_exchange_data(
                    loader, storage, symbols, args.interval, args.latest_count, args.workers
                )

        print()
        print("=" * 60)
        print("       最近K线预热完成")
        print("=" * 60)
        storage.disconnect()
        return 0

    # ==================== 当前时间模式 ====================
    if args.mode == "current":
        print("\n[当前时间模式] 从Redis扫描USDT合约...")

        # 获取所有USDT合约
        contracts = storage.get_all_usdt_contracts()

        if not contracts["okx"] and not contracts["binance"]:
            print("[错误] Redis中没有找到任何USDT合约数据")
            print("[提示] 请先运行 trading_server_full 和 data_recorder 收集实时数据")
            return 1

        print(f"\n[扫描结果]")
        print(f"  OKX: {len(contracts['okx'])} 个合约")
        print(f"  Binance: {len(contracts['binance'])} 个合约")

        # 定义要加载的周期和对应的天数（只拉取1m和1h，其他通过聚合生成）
        intervals_config = [
            ("1m", 60),    # 2个月
            ("1h", 180),   # 6个月
        ]

        # 需要通过1min聚合生成的周期
        aggregate_intervals = ["5m", "15m", "30m"]

        # 加载 OKX 数据
        if contracts["okx"] and args.exchange in ["okx", "all"]:
            loader = OKXKlineLoader(testnet=args.testnet, use_proxy=use_proxy)

            for interval, days in intervals_config:
                print(f"\n{'='*60}")
                print(f"[OKX] 处理 {interval} K线 (从当前时间往前 {days} 天)")
                print(f"{'='*60}")

                total_loaded = 0
                success_count = 0

                # 使用线程池并发处理
                with ThreadPoolExecutor(max_workers=args.workers) as executor:
                    futures = []
                    for i, symbol in enumerate(contracts["okx"], 1):
                        future = executor.submit(
                            process_single_contract,
                            loader, storage, symbol, interval, days, i, len(contracts["okx"])
                        )
                        futures.append(future)

                    # 收集结果
                    for future in as_completed(futures):
                        _, _, loaded, _ = future.result()
                        total_loaded += loaded
                        if loaded > 0:
                            success_count += 1

                print(f"\n[OKX {interval}] 完成: {success_count}/{len(contracts['okx'])} 个合约")
                print(f"  总计加载: {total_loaded} 根K线")

            # 从1min聚合生成其他周期
            if "1m" in [intv for intv, _ in intervals_config]:
                print(f"\n{'='*60}")
                print(f"[OKX] 从1min聚合生成其他周期")
                print(f"{'='*60}")

                for symbol in contracts["okx"]:
                    print(f"\n  处理: {symbol}")
                    results = aggregate_all_intervals(storage, symbol, "okx", aggregate_intervals)
                    total_aggregated = sum(results.values())
                    if total_aggregated > 0:
                        print(f"  ✓ 总计聚合: {total_aggregated} 根K线")

        # 加载 Binance 数据
        if contracts["binance"] and args.exchange in ["binance", "all"]:
            loader = BinanceKlineLoader(testnet=args.testnet, use_proxy=use_proxy)

            for interval, days in intervals_config:
                print(f"\n{'='*60}")
                print(f"[Binance] 处理 {interval} K线 (从当前时间往前 {days} 天)")
                print(f"{'='*60}")

                total_loaded = 0
                success_count = 0

                # 使用线程池并发处理
                with ThreadPoolExecutor(max_workers=args.workers) as executor:
                    futures = []
                    for i, symbol in enumerate(contracts["binance"], 1):
                        future = executor.submit(
                            process_single_contract,
                            loader, storage, symbol, interval, days, i, len(contracts["binance"])
                        )
                        futures.append(future)

                    # 收集结果
                    for future in as_completed(futures):
                        _, _, loaded, _ = future.result()
                        total_loaded += loaded
                        if loaded > 0:
                            success_count += 1

                print(f"\n[Binance {interval}] 完成: {success_count}/{len(contracts['binance'])} 个合约")
                print(f"  总计加载: {total_loaded} 根K线")

            # 从1min聚合生成其他周期
            if "1m" in [intv for intv, _ in intervals_config]:
                print(f"\n{'='*60}")
                print(f"[Binance] 从1min聚合生成其他周期")
                print(f"{'='*60}")

                for symbol in contracts["binance"]:
                    print(f"\n  处理: {symbol}")
                    results = aggregate_all_intervals(storage, symbol, "binance", aggregate_intervals)
                    total_aggregated = sum(results.values())
                    if total_aggregated > 0:
                        print(f"  ✓ 总计聚合: {total_aggregated} 根K线")

        print()
        print("=" * 60)
        print("       当前时间模式加载完成")
        print("=" * 60)
        storage.disconnect()
        return 0

    # ==================== 自动模式 ====================
    if args.mode == "auto":
        print("\n[自动模式] 从Redis扫描USDT合约...")

        # 获取所有USDT合约
        contracts = storage.get_all_usdt_contracts()

        if not contracts["okx"] and not contracts["binance"]:
            print("[错误] Redis中没有找到任何USDT合约数据")
            print("[提示] 请先运行 trading_server_full 和 data_recorder 收集实时数据")
            return 1

        print(f"\n[扫描结果]")
        print(f"  OKX: {len(contracts['okx'])} 个合约")
        print(f"  Binance: {len(contracts['binance'])} 个合约")

        # 定义要加载的周期和对应的天数（只拉取1m和1h，其他通过聚合生成）
        intervals_config = [
            ("1m", 60),    # 2个月
            ("1h", 180),   # 6个月
        ]

        # 需要通过1min聚合生成的周期
        aggregate_intervals = ["5m", "15m", "30m"]

        # 加载 OKX 数据
        if contracts["okx"] and args.exchange in ["okx", "all"]:
            loader = OKXKlineLoader(testnet=args.testnet, use_proxy=use_proxy)

            for interval, days in intervals_config:
                print(f"\n{'='*60}")
                print(f"[OKX] 处理 {interval} K线")
                print(f"{'='*60}")

                total_loaded = 0
                total_filled = 0
                success_count = 0

                for i, symbol in enumerate(contracts["okx"], 1):
                    print(f"\n[{i}/{len(contracts['okx'])}] {symbol}:{interval}")

                    try:
                        # 步骤1: 填补现有数据的间隔
                        filled = fill_gaps_in_existing_data(loader, storage, symbol, interval)
                        total_filled += filled

                        # 步骤2: 从最早时间戳往前加载历史数据
                        loaded = load_historical_before_oldest(
                            loader, storage, symbol, interval, days
                        )
                        total_loaded += loaded

                        if filled > 0 or loaded > 0:
                            success_count += 1
                    except Exception as e:
                        print(f"  ✗ 失败: {e}")

                print(f"\n[OKX {interval}] 完成: {success_count}/{len(contracts['okx'])} 个合约")
                print(f"  填补间隔: {total_filled} 根K线")
                print(f"  历史数据: {total_loaded} 根K线")
                print(f"  总计: {total_filled + total_loaded} 根K线")

            # 从1min聚合生成其他周期
            if "1m" in [intv for intv, _ in intervals_config]:
                print(f"\n{'='*60}")
                print(f"[OKX] 从1min聚合生成其他周期")
                print(f"{'='*60}")

                for symbol in contracts["okx"]:
                    print(f"\n  处理: {symbol}")
                    results = aggregate_all_intervals(storage, symbol, "okx", aggregate_intervals)
                    total_aggregated = sum(results.values())
                    if total_aggregated > 0:
                        print(f"  ✓ 总计聚合: {total_aggregated} 根K线")

        # 加载 Binance 数据
        if contracts["binance"] and args.exchange in ["binance", "all"]:
            loader = BinanceKlineLoader(testnet=args.testnet, use_proxy=use_proxy)

            for interval, days in intervals_config:
                print(f"\n{'='*60}")
                print(f"[Binance] 处理 {interval} K线")
                print(f"{'='*60}")

                total_loaded = 0
                total_filled = 0
                success_count = 0

                for i, symbol in enumerate(contracts["binance"], 1):
                    print(f"\n[{i}/{len(contracts['binance'])}] {symbol}:{interval}")

                    try:
                        # 步骤1: 填补现有数据的间隔
                        filled = fill_gaps_in_existing_data(loader, storage, symbol, interval)
                        total_filled += filled

                        # 步骤2: 从最早时间戳往前加载历史数据
                        loaded = load_historical_before_oldest(
                            loader, storage, symbol, interval, days
                        )
                        total_loaded += loaded

                        if filled > 0 or loaded > 0:
                            success_count += 1
                    except Exception as e:
                        print(f"  ✗ 失败: {e}")

                print(f"\n[Binance {interval}] 完成: {success_count}/{len(contracts['binance'])} 个合约")
                print(f"  填补间隔: {total_filled} 根K线")
                print(f"  历史数据: {total_loaded} 根K线")
                print(f"  总计: {total_filled + total_loaded} 根K线")

            # 从1min聚合生成其他周期
            if "1m" in [intv for intv, _ in intervals_config]:
                print(f"\n{'='*60}")
                print(f"[Binance] 从1min聚合生成其他周期")
                print(f"{'='*60}")

                for symbol in contracts["binance"]:
                    print(f"\n  处理: {symbol}")
                    results = aggregate_all_intervals(storage, symbol, "binance", aggregate_intervals)
                    total_aggregated = sum(results.values())
                    if total_aggregated > 0:
                        print(f"  ✓ 总计聚合: {total_aggregated} 根K线")

        print()
        print("=" * 60)
        print("       自动加载完成")
        print("=" * 60)
        storage.disconnect()
        return 0

    # ==================== 手动模式 ====================

    # 加载 Binance 数据
    if args.exchange in ["binance", "all"]:
        loader = BinanceKlineLoader(testnet=args.testnet, use_proxy=use_proxy)

        if args.symbols:
            symbols = [s.strip().upper() for s in args.symbols.split(",")]
            # 确保是 Binance 格式
            symbols = [s.replace("-", "").replace("SWAP", "") for s in symbols]
            if symbols[0].endswith("USDT"):
                pass  # 已经是 Binance 格式
            print(f"[Binance] 使用指定的 {len(symbols)} 个交易对")
        else:
            print("[Binance] 获取交易对列表...")
            symbols = loader.get_exchange_info()
            print(f"[Binance] 获取到 {len(symbols)} 个永续合约交易对")

        if symbols:
            total, success, failed, elapsed = load_exchange_data(
                loader, storage, symbols, args.interval, args.days, args.workers
            )
            results["binance"] = {
                "total": total,
                "success": success,
                "failed": failed,
                "elapsed": elapsed,
                "symbols": symbols
            }

    # 加载 OKX 数据
    if args.exchange in ["okx", "all"]:
        loader = OKXKlineLoader(testnet=args.testnet, use_proxy=use_proxy)

        if args.symbols:
            symbols = [s.strip().upper() for s in args.symbols.split(",")]
            # 转换为 OKX 格式
            okx_symbols = []
            for s in symbols:
                if "-SWAP" in s:
                    okx_symbols.append(s)
                elif s.endswith("USDT"):
                    base = s[:-4]
                    okx_symbols.append(f"{base}-USDT-SWAP")
                else:
                    okx_symbols.append(s)
            symbols = okx_symbols
            print(f"[OKX] 使用指定的 {len(symbols)} 个交易对")
        else:
            print("[OKX] 获取交易对列表...")
            symbols = loader.get_exchange_info()
            print(f"[OKX] 获取到 {len(symbols)} 个永续合约交易对")

        if symbols:
            total, success, failed, elapsed = load_exchange_data(
                loader, storage, symbols, args.interval, args.days, args.workers
            )
            results["okx"] = {
                "total": total,
                "success": success,
                "failed": failed,
                "elapsed": elapsed,
                "symbols": symbols
            }

    # 统计结果
    print()
    print("=" * 60)
    print("       加载完成")
    print("=" * 60)

    for exchange, data in results.items():
        print()
        print(f"[{exchange.upper()}]")
        print(f"  总耗时: {data['elapsed']:.1f} 秒")
        print(f"  成功: {data['success']}/{len(data['symbols'])} 个交易对")
        print(f"  新增: {data['total']} 条 K 线")
        if data['failed']:
            print(f"  失败: {len(data['failed'])} 个")
            print(f"  失败列表: {data['failed'][:5]}{'...' if len(data['failed']) > 5 else ''}")

    # 显示部分交易对的数据量
    print()
    print("[数据统计]")

    if "binance" in results:
        sample_symbols = ["BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT"]
        print("  Binance:")
        for sym in sample_symbols:
            count = storage.get_kline_count(sym, args.interval, "binance")
            if count > 0:
                print(f"    {sym}: {count} 条")

    if "okx" in results:
        sample_symbols = ["BTC-USDT-SWAP", "ETH-USDT-SWAP", "SOL-USDT-SWAP", "DOGE-USDT-SWAP", "XRP-USDT-SWAP"]
        print("  OKX:")
        for sym in sample_symbols:
            count = storage.get_kline_count(sym, args.interval, "okx")
            if count > 0:
                print(f"    {sym}: {count} 条")

    storage.disconnect()
    print()
    print("[完成] 数据已存储到 Redis")
    return 0


if __name__ == "__main__":
    sys.exit(main())
