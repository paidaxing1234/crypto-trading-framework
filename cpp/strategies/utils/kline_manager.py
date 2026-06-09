#!/usr/bin/env python3
"""
K线数据管理器 - 基于 NumPy 的高性能存储

特点:
- 纯内存存储，读写极快（纳秒级）
- 自动滑动窗口，保持固定长度
- 按 symbol 索引，支持多币种
- 提供简洁的存储和读取接口

使用示例:
    manager = KlineManager(max_bars=100, interval="1s")
    
    # 存储
    manager.update("BTC-USDT", timestamp, open, high, low, close, volume)
    
    # 读取
    data = manager.get_all("BTC-USDT")
    closes = manager.get_closes("BTC-USDT")

@author Sequence Team
@date 2025-12
"""

import numpy as np
from typing import Dict, Optional, List, Tuple


# K线数据的 dtype 定义
KLINE_DTYPE = np.dtype([
    ('timestamp', 'i8'),   # 毫秒时间戳
    ('open', 'f8'),
    ('high', 'f8'),
    ('low', 'f8'),
    ('close', 'f8'),
    ('volume', 'f8'),
])


class KlineBuffer:
    """
    单个币种的K线缓冲区
    
    使用 NumPy 结构化数组实现环形缓冲区
    """
    
    def __init__(self, max_bars: int = 100):
        self.max_bars = max_bars
        self.data = np.zeros(max_bars, dtype=KLINE_DTYPE)
        self.size = 0           # 当前存储的K线数量
        self.head = 0           # 环形缓冲区头部索引
        
    def update(self, timestamp: int, open_: float, high: float, 
               low: float, close: float, volume: float) -> bool:
        """
        更新K线数据
        
        - 如果 timestamp 与最后一根相同，更新最后一根
        - 否则追加新的一根
        
        Returns:
            bool: True 表示追加了新K线，False 表示更新了现有K线
        """
        if self.size == 0:
            # 第一根K线
            self.data[0] = (timestamp, open_, high, low, close, volume)
            self.size = 1
            return True
        
        # 获取最后一根K线的索引
        last_idx = (self.head + self.size - 1) % self.max_bars
        
        if self.data[last_idx]['timestamp'] == timestamp:
            # 更新当前K线
            self.data[last_idx] = (timestamp, open_, high, low, close, volume)
            return False
        else:
            # 追加新K线
            if self.size < self.max_bars:
                new_idx = (self.head + self.size) % self.max_bars
                self.data[new_idx] = (timestamp, open_, high, low, close, volume)
                self.size += 1
            else:
                # 缓冲区已满，覆盖最旧的
                self.data[self.head] = (timestamp, open_, high, low, close, volume)
                self.head = (self.head + 1) % self.max_bars
            return True
    
    def get_all(self) -> np.ndarray:
        """获取所有K线数据（按时间顺序）"""
        if self.size == 0:
            return np.array([], dtype=KLINE_DTYPE)
        
        if self.size <= self.max_bars and self.head == 0:
            return self.data[:self.size].copy()
        else:
            result = np.empty(self.size, dtype=KLINE_DTYPE)
            first_part = self.max_bars - self.head
            if first_part >= self.size:
                result[:] = self.data[self.head:self.head + self.size]
            else:
                result[:first_part] = self.data[self.head:]
                result[first_part:] = self.data[:self.size - first_part]
            return result
    
    def get_closes(self) -> np.ndarray:
        """获取收盘价数组"""
        return self.get_all()['close']
    
    def get_timestamps(self) -> np.ndarray:
        """获取时间戳数组"""
        return self.get_all()['timestamp']
    
    def get_last(self) -> Optional[np.void]:
        """获取最后一根K线"""
        if self.size == 0:
            return None
        last_idx = (self.head + self.size - 1) % self.max_bars
        return self.data[last_idx]
    
    def __len__(self) -> int:
        return self.size
    
    def is_continuous(self, interval_ms: int) -> Tuple[bool, List[int]]:
        """检查数据是否连续"""
        if self.size <= 1:
            return True, []
        
        data = self.get_all()
        timestamps = data['timestamp']
        
        missing = []
        for i in range(1, len(timestamps)):
            expected = timestamps[i-1] + interval_ms
            if timestamps[i] != expected:
                t = expected
                while t < timestamps[i]:
                    missing.append(t)
                    t += interval_ms
        
        return len(missing) == 0, missing


class KlineManager:
    """
    多币种K线数据管理器
    
    使用示例:
        manager = KlineManager(max_bars=100, interval="1s")
        
        # 存储K线
        manager.update("BTC-USDT", timestamp, open, high, low, close, volume)
        
        # 读取数据
        all_data = manager.get_all("BTC-USDT")
        closes = manager.get_closes("BTC-USDT")
    """
    
    # 支持的K线间隔及其毫秒数
    INTERVALS = {
        "1s": 1000,
        "1m": 60000,
        "3m": 180000,
        "5m": 300000,
        "15m": 900000,
        "30m": 1800000,
        "1H": 3600000,
        "4H": 14400000,
        "1D": 86400000,
    }
    
    def __init__(self, max_bars: int = 100, interval: str = "1m"):
        """
        初始化K线管理器
        
        Args:
            max_bars: 每个币种保留的最大K线数量
            interval: K线周期 ("1s", "1m", "5m", "1H", etc.)
        """
        self.max_bars = max_bars
        self.interval = interval
        self.interval_ms = self.INTERVALS.get(interval, 60000)
        self._buffers: Dict[str, KlineBuffer] = {}
        
    def _get_or_create_buffer(self, symbol: str) -> KlineBuffer:
        """获取或创建币种的缓冲区"""
        if symbol not in self._buffers:
            self._buffers[symbol] = KlineBuffer(self.max_bars)
        return self._buffers[symbol]
    
    # ==================== 存储接口 ====================
    
    def update(self, symbol: str, timestamp: int, 
               open_: float, high: float, low: float, 
               close: float, volume: float) -> bool:
        """更新单根K线"""
        buffer = self._get_or_create_buffer(symbol)
        return buffer.update(timestamp, open_, high, low, close, volume)
    
    def update_from_dict(self, symbol: str, kline: dict) -> bool:
        """从字典更新K线（兼容实盘推送格式）"""
        return self.update(
            symbol,
            int(kline.get('timestamp', 0)),
            float(kline.get('open', 0)),
            float(kline.get('high', 0)),
            float(kline.get('low', 0)),
            float(kline.get('close', 0)),
            float(kline.get('volume', 0))
        )
    
    # ==================== 读取接口 ====================
    
    def get_all(self, symbol: str) -> Optional[np.ndarray]:
        """获取某币种的所有K线数据"""
        if symbol not in self._buffers:
            return None
        return self._buffers[symbol].get_all()
    
    def get_closes(self, symbol: str) -> Optional[np.ndarray]:
        """获取收盘价数组"""
        if symbol not in self._buffers:
            return None
        return self._buffers[symbol].get_closes()
    
    def get_last(self, symbol: str) -> Optional[np.void]:
        """获取最后一根K线"""
        if symbol not in self._buffers:
            return None
        return self._buffers[symbol].get_last()
    
    def get_bar_count(self, symbol: str) -> int:
        """获取某币种当前存储的K线数量"""
        if symbol not in self._buffers:
            return 0
        return len(self._buffers[symbol])
    
    def get_symbols(self) -> List[str]:
        """获取所有已存储的币种列表"""
        return list(self._buffers.keys())
    
    # ==================== 校验接口 ====================
    
    def is_continuous(self, symbol: str) -> Tuple[bool, List[int]]:
        """检查某币种的K线数据是否连续"""
        if symbol not in self._buffers:
            return True, []
        return self._buffers[symbol].is_continuous(self.interval_ms)
    
    def check_all_continuous(self) -> Dict[str, Tuple[bool, List[int]]]:
        """检查所有币种的数据连续性"""
        result = {}
        for symbol in self._buffers:
            result[symbol] = self.is_continuous(symbol)
        return result
    
    # ==================== 统计接口 ====================
    
    def get_stats(self) -> dict:
        """获取管理器统计信息"""
        total_bars = sum(len(buf) for buf in self._buffers.values())
        memory_bytes = total_bars * KLINE_DTYPE.itemsize
        
        return {
            'symbol_count': len(self._buffers),
            'total_bars': total_bars,
            'max_bars_per_symbol': self.max_bars,
            'interval': self.interval,
            'interval_ms': self.interval_ms,
            'memory_bytes': memory_bytes,
            'memory_kb': memory_bytes / 1024,
        }
    
    def __repr__(self) -> str:
        stats = self.get_stats()
        return (f"KlineManager(symbols={stats['symbol_count']}, "
                f"total_bars={stats['total_bars']}, "
                f"interval={self.interval}, "
                f"memory={stats['memory_kb']:.2f}KB)")

