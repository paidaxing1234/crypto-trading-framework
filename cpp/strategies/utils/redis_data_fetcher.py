#!/usr/bin/env python3
"""
Redis 数据拉取器

功能：
1. 从服务器 Redis 拉取行情数据
2. 存储到本地 CSV 文件

使用方法：
    python3 redis_data_fetcher.py --redis-host 192.168.1.100 --output ./data

依赖：
    pip install redis

@author Sequence Team
@date 2025-12
"""

import redis
import json
import time
import argparse
import os
import csv
from datetime import datetime
from typing import Optional, List, Dict


class RedisDataFetcher:
    """Redis 数据拉取器"""
    
    def __init__(self, host: str, port: int, password: str = "", db: int = 0):
        self.host = host
        self.port = port
        self.password = password
        self.db = db
        self.client: Optional[redis.Redis] = None
        
    def connect(self) -> bool:
        """连接到 Redis"""
        try:
            self.client = redis.Redis(
                host=self.host,
                port=self.port,
                password=self.password if self.password else None,
                db=self.db,
                decode_responses=True
            )
            self.client.ping()
            print(f"[Redis] 连接成功: {self.host}:{self.port}")
            return True
        except redis.ConnectionError as e:
            print(f"[Redis] 连接失败: {e}")
            return False
    
    def disconnect(self):
        """断开连接"""
        if self.client:
            self.client.close()
            self.client = None
    
    def get_trades(self, symbol: str, limit: int = 10000) -> List[Dict]:
        """获取 trades 数据"""
        if not self.client:
            return []
        
        key = f"trades:{symbol}"
        try:
            raw_data = self.client.lrange(key, 0, limit - 1)
            return [json.loads(item) for item in raw_data]
        except Exception as e:
            print(f"[错误] 获取 trades 失败: {e}")
            return []
    
    def get_klines(self, symbol: str, interval: str, limit: int = 10000) -> List[Dict]:
        """获取 K 线数据"""
        if not self.client:
            return []
        
        key = f"kline:{symbol}:{interval}"
        try:
            # 获取最新 N 条（按时间升序）
            raw_data = self.client.zrange(key, -limit, -1)
            return [json.loads(item) for item in raw_data]
        except Exception as e:
            print(f"[错误] 获取 K线 失败: {e}")
            return []
    
    def get_all_symbols(self) -> Dict[str, Dict[str, int]]:
        """获取所有有数据的交易对"""
        if not self.client:
            return {}
        
        result = {"trades": {}, "klines": {}}
        
        try:
            # 扫描 trades
            cursor = 0
            while True:
                cursor, keys = self.client.scan(cursor, match="trades:*", count=100)
                for key in keys:
                    symbol = key.replace("trades:", "")
                    count = self.client.llen(key)
                    result["trades"][symbol] = count
                if cursor == 0:
                    break
            
            # 扫描 K线
            cursor = 0
            while True:
                cursor, keys = self.client.scan(cursor, match="kline:*", count=100)
                for key in keys:
                    parts = key.split(":", 2)
                    if len(parts) >= 3:
                        symbol_interval = f"{parts[1]}:{parts[2]}"
                        count = self.client.zcard(key)
                        result["klines"][symbol_interval] = count
                if cursor == 0:
                    break
                    
        except Exception as e:
            print(f"[错误] 扫描失败: {e}")
        
        return result


class CsvStorage:
    """CSV 文件存储"""
    
    def __init__(self, output_dir: str):
        self.output_dir = output_dir
        os.makedirs(output_dir, exist_ok=True)
        os.makedirs(os.path.join(output_dir, "trades"), exist_ok=True)
        os.makedirs(os.path.join(output_dir, "klines"), exist_ok=True)
        print(f"[存储] 输出目录: {output_dir}")
    
    def save_trades(self, symbol: str, trades: List[Dict]) -> int:
        """保存 trades 到 CSV"""
        if not trades:
            return 0
        
        # 文件名: trades/BTC-USDT_20251219.csv
        date_str = datetime.now().strftime("%Y%m%d")
        filename = os.path.join(self.output_dir, "trades", f"{symbol}_{date_str}.csv")
        
        # 读取已有数据的时间戳（避免重复）
        existing_keys = set()
        if os.path.exists(filename):
            with open(filename, 'r', newline='') as f:
                reader = csv.DictReader(f)
                for row in reader:
                    # 用 timestamp + price + size 作为唯一键
                    key = f"{row.get('timestamp')}_{row.get('price')}_{row.get('size')}"
                    existing_keys.add(key)
        
        # 写入新数据
        file_exists = os.path.exists(filename)
        new_count = 0
        
        with open(filename, 'a', newline='') as f:
            fieldnames = ['timestamp', 'symbol', 'price', 'size', 'side']
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            
            if not file_exists:
                writer.writeheader()
            
            for trade in trades:
                key = f"{trade.get('timestamp')}_{trade.get('price')}_{trade.get('size')}"
                if key not in existing_keys:
                    writer.writerow({
                        'timestamp': trade.get('timestamp', 0),
                        'symbol': trade.get('symbol', symbol),
                        'price': trade.get('price', 0),
                        'size': trade.get('size', 0),
                        'side': trade.get('side', '')
                    })
                    new_count += 1
        
        return new_count
    
    def save_klines(self, symbol: str, interval: str, klines: List[Dict]) -> int:
        """保存 K线 到 CSV"""
        if not klines:
            return 0
        
        # 文件名: klines/BTC-USDT_1m_20251219.csv
        date_str = datetime.now().strftime("%Y%m%d")
        filename = os.path.join(self.output_dir, "klines", f"{symbol}_{interval}_{date_str}.csv")
        
        # 读取已有数据的时间戳
        existing_ts = set()
        if os.path.exists(filename):
            with open(filename, 'r', newline='') as f:
                reader = csv.DictReader(f)
                for row in reader:
                    existing_ts.add(int(row.get('timestamp', 0)))
        
        # 写入新数据
        file_exists = os.path.exists(filename)
        new_count = 0
        
        with open(filename, 'a', newline='') as f:
            fieldnames = ['timestamp', 'symbol', 'interval', 'open', 'high', 'low', 'close', 'volume']
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            
            if not file_exists:
                writer.writeheader()
            
            for kline in klines:
                ts = kline.get('timestamp', 0)
                if ts not in existing_ts:
                    writer.writerow({
                        'timestamp': ts,
                        'symbol': kline.get('symbol', symbol),
                        'interval': interval,
                        'open': kline.get('open', 0),
                        'high': kline.get('high', 0),
                        'low': kline.get('low', 0),
                        'close': kline.get('close', 0),
                        'volume': kline.get('volume', 0)
                    })
                    new_count += 1
        
        return new_count


def fetch_and_store(fetcher: RedisDataFetcher, storage: CsvStorage,
                   symbols: List[str], intervals: List[str]):
    """从 Redis 拉取数据并存储到 CSV"""
    print(f"\n[拉取] {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
    
    total_trades = 0
    total_klines = 0
    
    # 拉取 trades
    for symbol in symbols:
        trades = fetcher.get_trades(symbol, limit=10000)
        if trades:
            saved = storage.save_trades(symbol, trades)
            total_trades += saved
            print(f"  [trades] {symbol}: 获取 {len(trades)} 条, 新增 {saved} 条")
    
    # 拉取 K线
    for symbol in symbols:
        for interval in intervals:
            klines = fetcher.get_klines(symbol, interval, limit=10000)
            if klines:
                saved = storage.save_klines(symbol, interval, klines)
                total_klines += saved
                print(f"  [kline] {symbol}:{interval}: 获取 {len(klines)} 条, 新增 {saved} 条")
    
    print(f"\n[完成] 新增 Trades: {total_trades} 条, K线: {total_klines} 条")


def main():
    parser = argparse.ArgumentParser(description='Redis 数据拉取器 (CSV)')
    
    # Redis 配置
    parser.add_argument('--redis-host', type=str, default='127.0.0.1',
                        help='Redis 主机')
    parser.add_argument('--redis-port', type=int, default=6379,
                        help='Redis 端口')
    parser.add_argument('--redis-pass', type=str, default='',
                        help='Redis 密码')
    
    # 本地存储
    parser.add_argument('--output', type=str, default='./market_data',
                        help='输出目录')
    
    # 数据范围
    parser.add_argument('--symbols', type=str, 
                        default='BTC-USDT,ETH-USDT,SOL-USDT',
                        help='要拉取的交易对，逗号分隔')
    parser.add_argument('--intervals', type=str,
                        default='1s,1m,5m,1H',
                        help='要拉取的K线周期，逗号分隔')
    
    # 模式
    parser.add_argument('--daemon', action='store_true',
                        help='守护进程模式，定时拉取')
    parser.add_argument('--interval-hours', type=float, default=1.0,
                        help='守护进程模式下的拉取间隔（小时）')
    parser.add_argument('--list', action='store_true',
                        help='列出 Redis 中所有数据')
    
    args = parser.parse_args()
    
    print("╔" + "═" * 48 + "╗")
    print("║        Redis 数据拉取器 (CSV)                    ║")
    print("╚" + "═" * 48 + "╝\n")
    
    # 解析交易对和周期
    symbols = [s.strip() for s in args.symbols.split(',') if s.strip()]
    intervals = [i.strip() for i in args.intervals.split(',') if i.strip()]
    
    # 连接 Redis
    fetcher = RedisDataFetcher(args.redis_host, args.redis_port, args.redis_pass)
    if not fetcher.connect():
        return
    
    # 列出数据
    if args.list:
        print("\n[Redis 数据统计]\n")
        all_data = fetcher.get_all_symbols()
        
        print("Trades:")
        for symbol, count in sorted(all_data["trades"].items()):
            print(f"  {symbol}: {count} 条")
        
        print("\nK线:")
        for key, count in sorted(all_data["klines"].items()):
            print(f"  {key}: {count} 条")
        
        fetcher.disconnect()
        return
    
    # 创建存储
    storage = CsvStorage(args.output)
    
    print(f"[配置]")
    print(f"  Redis: {args.redis_host}:{args.redis_port}")
    print(f"  输出目录: {args.output}")
    print(f"  交易对: {symbols}")
    print(f"  K线周期: {intervals}")
    
    if args.daemon:
        print(f"\n[守护进程模式] 每 {args.interval_hours} 小时拉取一次")
        print("  按 Ctrl+C 停止\n")
        
        try:
            while True:
                fetch_and_store(fetcher, storage, symbols, intervals)
                print(f"\n[等待] 下次拉取: {args.interval_hours} 小时后\n")
                time.sleep(args.interval_hours * 3600)
        except KeyboardInterrupt:
            print("\n[停止] 已退出")
    else:
        # 单次拉取
        fetch_and_store(fetcher, storage, symbols, intervals)
    
    # 清理
    fetcher.disconnect()


if __name__ == "__main__":
    main()

