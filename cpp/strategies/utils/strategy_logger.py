"""
策略日志工具类
用于将策略运行日志同时输出到终端和文件
"""

import sys
import os
from datetime import datetime
from typing import Optional


class StrategyLogger:
    """策略日志记录器"""

    def __init__(self, exchange: str, strategy_id: str, log_dir: str = "./log"):
        """
        初始化日志记录器

        Args:
            exchange: 交易所名称（如 okx, binance）
            strategy_id: 策略ID
            log_dir: 日志目录路径
        """
        self.exchange = exchange.lower()
        self.strategy_id = strategy_id
        self.log_dir = log_dir

        # 确保日志目录存在
        os.makedirs(log_dir, exist_ok=True)

        # 日志文件路径：交易所名_策略ID.txt
        self.log_file = os.path.join(log_dir, f"{self.exchange}_{strategy_id}.txt")

        # 打开日志文件（追加模式）
        self.file_handle = open(self.log_file, 'a', encoding='utf-8', buffering=1)

        # 写入分隔符和启动时间
        self._write_separator()
        self._write_startup_info()

    def _write_separator(self):
        """写入分隔符"""
        separator = "\n" + "=" * 80 + "\n"
        self.file_handle.write(separator)
        self.file_handle.flush()

    def _write_startup_info(self):
        """写入启动信息"""
        now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        startup_msg = f"策略启动时间: {now} | 交易所: {self.exchange} | 策略ID: {self.strategy_id}\n"
        self.file_handle.write(startup_msg)
        self.file_handle.write("=" * 80 + "\n")
        self.file_handle.flush()

    def log(self, message: str, level: str = "INFO"):
        """
        记录日志

        Args:
            message: 日志消息
            level: 日志级别（INFO, WARNING, ERROR）
        """
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        log_line = f"[{timestamp}] [{level}] {message}\n"

        # 同时写入文件和终端
        self.file_handle.write(log_line)
        self.file_handle.flush()
        print(message, flush=True)

    def info(self, message: str):
        """记录INFO级别日志"""
        self.log(message, "INFO")

    def warning(self, message: str):
        """记录WARNING级别日志"""
        self.log(message, "WARNING")

    def error(self, message: str):
        """记录ERROR级别日志"""
        self.log(message, "ERROR")

    def close(self):
        """关闭日志文件"""
        if self.file_handle and not self.file_handle.closed:
            self._write_separator()
            shutdown_msg = f"策略停止时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n"
            self.file_handle.write(shutdown_msg)
            self.file_handle.write("=" * 80 + "\n\n")
            self.file_handle.flush()
            self.file_handle.close()

    def __del__(self):
        """析构函数，确保文件被关闭"""
        self.close()


class TeeOutput:
    """
    将输出同时重定向到终端和文件
    用于捕获 print() 和其他标准输出
    """

    def __init__(self, file_handle, original_stream):
        self.file_handle = file_handle
        self.original_stream = original_stream

    def write(self, message):
        """写入消息"""
        if message and message.strip():  # 忽略空行
            # 写入文件
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            self.file_handle.write(f"[{timestamp}] {message}")
            self.file_handle.flush()

        # 写入原始流（终端）
        self.original_stream.write(message)
        self.original_stream.flush()

    def flush(self):
        """刷新缓冲区"""
        self.file_handle.flush()
        self.original_stream.flush()


def setup_strategy_logging(exchange: str, strategy_id: str, log_dir: str = "./log") -> StrategyLogger:
    """
    设置策略日志

    Args:
        exchange: 交易所名称
        strategy_id: 策略ID
        log_dir: 日志目录

    Returns:
        StrategyLogger实例
    """
    logger = StrategyLogger(exchange, strategy_id, log_dir)

    # 重定向 stdout 和 stderr 到日志文件
    sys.stdout = TeeOutput(logger.file_handle, sys.__stdout__)
    sys.stderr = TeeOutput(logger.file_handle, sys.__stderr__)

    return logger
