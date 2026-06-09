#!/usr/bin/env python3
"""
网格策略 - 基于模块化 C++ 策略基类

使用模块化设计的 C++ StrategyBase：
- MarketDataModule: 行情数据（K线订阅、存储）
- TradingModule: 交易操作（下单、撤单）
- AccountModule: 账户管理（登录、余额、持仓）

使用方法:
    # 先编译 C++ 模块
    cd cpp/build && cmake .. && make strategy_base

    # 从配置文件运行策略
    python3 grid_strategy_cpp.py --config configs/grid_btc_main.json

    # 或使用命令行参数
    python3 grid_strategy_cpp.py --symbol BTC-USDT-SWAP --grid-num 20

@author Sequence Team
@date 2025-12
"""

import sys
import os
from pathlib import Path

# 自动设置 Python 路径，让策略能找到 strategy_base 模块
# 获取脚本所在目录的绝对路径
script_dir = Path(__file__).resolve().parent
build_dir = script_dir / 'build'  # 使用 build 目录中的新编译模块

# 优先使用 build 目录
if build_dir.exists() and str(build_dir) not in sys.path:
    sys.path.insert(0, str(build_dir))

# 备用：使用脚本目录（如果 build 目录不存在）
if str(script_dir) not in sys.path:
    sys.path.insert(0, str(script_dir))

# 同时添加当前工作目录
current_dir = Path.cwd()
if str(current_dir) not in sys.path:
    sys.path.insert(0, str(current_dir))

import signal
import argparse
import time
import json
from typing import List, Dict, Optional
from strategy_logger import setup_strategy_logging

# 导入 C++ 策略基类
try:
    from strategy_base import (
        StrategyBase,
        KlineBar,
        TradeData,
        PositionInfo,
        BalanceInfo,
        OrderInfo
    )
except ImportError as e:
    print("错误: 未找到 strategy_base 模块")
    print(f"导入错误: {e}")
    print(f"当前 Python 路径: {sys.path[:3]}")
    print(f"脚本目录: {script_dir}")
    print(f"当前目录: {current_dir}")
    print()
    print("请检查:")
    print("  1. strategy_base.cpython-*.so 是否存在于 strategies 目录")
    print("  2. 如果不存在，请编译: cd cpp/strategies/build && cmake .. && make")
    sys.exit(1)


class GridStrategy(StrategyBase):
    """
    网格交易策略
    
    继承自模块化 C++ StrategyBase，使用基类提供的：
    - 行情模块：订阅K线、获取历史数据
    - 交易模块：下单、撤单
    - 账户模块：注册、查询余额持仓
    """
    
    def __init__(self,
                 strategy_id: str,
                 symbol: str,
                 grid_num: int,
                 grid_spread: float,
                 order_amount: float,
                 api_key: str = "",
                 secret_key: str = "",
                 passphrase: str = "",
                 is_testnet: bool = True,
                 log_file_path: str = ""):
        """
        初始化网格策略

        Args:
            strategy_id: 策略ID
            symbol: 交易对（如 BTC-USDT-SWAP）
            grid_num: 单边网格数量
            grid_spread: 网格间距比例（如 0.001 = 0.1%）
            order_amount: 单次下单金额（USDT）
            api_key: OKX API Key
            secret_key: OKX Secret Key
            passphrase: OKX Passphrase
            is_testnet: 是否模拟盘
            log_file_path: 日志文件路径（空字符串表示不记录到文件）
        """
        # 初始化基类（7200 根 K 线 = 2 小时 1s 数据）
        super().__init__(strategy_id, max_kline_bars=7200, log_file_path=log_file_path)

        self.symbol = symbol
        self.grid_num = grid_num
        self.grid_spread = grid_spread
        self.order_amount = order_amount

        # 账户信息
        self._api_key = api_key
        self._secret_key = secret_key
        self._passphrase = passphrase
        self._is_testnet = is_testnet

        # 余额就绪标志
        self.balance_ready = False

        # 价格追踪
        self.current_price = 0.0
        self.base_price = 0.0

        # 网格
        self.buy_levels: List[float] = []
        self.sell_levels: List[float] = []
        self.triggered: Dict[float, bool] = {}

        # 统计
        self.buy_count = 0
        self.sell_count = 0

        # 下单冷却（防止频繁下单触发限流）
        self.last_order_time = 0.0
        self.order_cooldown = 1.0  # 两次下单最小间隔（秒）

        # 判断是否为合约
        self.is_swap = "SWAP" in symbol.upper()

        self.log_info(f"策略参数: {symbol} | 网格:{grid_num}x2 | 间距:{grid_spread*100:.3f}% | 金额:{order_amount}U")
    
    # ============================================================
    # 生命周期回调
    # ============================================================
    
    def on_init(self):
        """策略初始化"""
        self.log_info("策略初始化...")
        
        # 1. 注册账户
        if self._api_key and self._secret_key and self._passphrase:
            self.register_account(
                self._api_key, 
                self._secret_key, 
                self._passphrase, 
                self._is_testnet
            )
            time.sleep(1)  # 等待注册完成
        else:
            self.log_info("未提供账户信息，使用服务器默认账户")
        
        # 2. 测试下单功能
        self.log_info("=" * 50)
        self.log_info("开始测试下单功能...")
        
        # 测试买入 1 张（使用 long 双向持仓模式）
        self.log_info(f"测试买入: {self.symbol} buy 1张 (long)")
        buy_order_id = self.send_swap_market_order(self.symbol, "buy", 1, "long")
        self.log_info(f"买入订单已发送, 客户端订单ID: {buy_order_id}")
        
        time.sleep(2)  # 等待回报
        
        # 测试卖出 1 张平多（使用 long 双向持仓模式）
        self.log_info(f"测试卖出平多: {self.symbol} sell 1张 (long)")
        sell_order_id = self.send_swap_market_order(self.symbol, "sell", 1, "long")
        self.log_info(f"卖出订单已发送, 客户端订单ID: {sell_order_id}")
        
        time.sleep(2)  # 等待回报
        
        self.log_info("下单测试完成")
        self.log_info("=" * 50)
        
        # 3. 订阅 K 线
        self.subscribe_kline(self.symbol, "1s")
        
        time.sleep(1)
        self.log_info("初始化完成，等待行情数据...")
    
    def on_stop(self):
        """策略停止"""
        self.log_info("策略停止...")
        self.unsubscribe_kline(self.symbol, "1s")
        self.print_grid_summary()
    
    # ============================================================
    # 账户回调
    # ============================================================
    
    def on_register_report(self, success: bool, error_msg: str):
        """账户注册回报"""
        if success:
            self.log_info("✓ 账户注册成功")
            # 主动刷新账户余额（重要！）
            self.refresh_account()
            # 不在这里等待，而是在 on_balance_update 回调中处理
        else:
            self.log_error(f"✗ 账户注册失败: {error_msg}")

    def on_balance_update(self, balance: BalanceInfo):
        """余额更新回调"""
        if balance.currency == "USDT" and not self.balance_ready:
            self.balance_ready = True
            self.log_info(f"✓ 余额数据已就绪")
            self.log_info(f"  USDT可用: {balance.available:.2f}")
            self.log_info(f"  USDT冻结: {balance.frozen:.2f}")
            self.log_info(f"  USDT总计: {balance.total:.2f}")
    
    def on_position_update(self, position: PositionInfo):
        """持仓更新回调"""
        if position.symbol == self.symbol and position.quantity != 0:
            self.log_info(f"[持仓更新] {position.symbol} {position.pos_side}: "
                         f"{position.quantity}张 @ {position.avg_price:.2f} "
                         f"盈亏: {position.unrealized_pnl:.2f}")
    
    # ============================================================
    # K线回调
    # ============================================================
    
    def on_kline(self, symbol: str, interval: str, bar: KlineBar):
        """K线回调"""
        if symbol != self.symbol:
            return
        
        # 更新当前价格
        self.current_price = bar.close
        
        # 初始化网格
        if self.base_price == 0:
            self.base_price = bar.close
            self.init_grids()
            return
        
        # 检查网格触发
        self.check_grid_triggers()
    
    # ============================================================
    # 订单回报
    # ============================================================
    
    def on_order_report(self, report: dict):
        """订单回报"""
        status = report.get("status", "")
        side = report.get("side", "")
        
        if status == "filled":
            if side == "buy":
                self.buy_count += 1
            elif side == "sell":
                self.sell_count += 1
            
            self.log_info(f"[成交] {side.upper()} | 买入:{self.buy_count} 卖出:{self.sell_count}")
    
    # ============================================================
    # 网格逻辑
    # ============================================================
    
    def init_grids(self):
        """初始化网格"""
        self.buy_levels = []
        self.sell_levels = []
        self.triggered = {}
        
        # 买入网格（低价）
        for i in range(1, self.grid_num + 1):
            level = self.base_price * (1 - self.grid_spread * i)
            self.buy_levels.append(level)
            self.triggered[level] = False
        
        # 卖出网格（高价）
        for i in range(1, self.grid_num + 1):
            level = self.base_price * (1 + self.grid_spread * i)
            self.sell_levels.append(level)
            self.triggered[level] = False
        
        self.buy_levels.sort(reverse=True)
        self.sell_levels.sort()
        
        self.log_info(f"网格初始化: 基准={self.base_price:.2f} | "
                     f"买入:{self.buy_levels[-1]:.2f}~{self.buy_levels[0]:.2f} | "
                     f"卖出:{self.sell_levels[0]:.2f}~{self.sell_levels[-1]:.2f}")
    
    def check_grid_triggers(self):
        """检查网格触发"""
        # 检查买入网格
        for level in self.buy_levels:
            if self.triggered[level]:
                continue
            
            if self.current_price <= level:
                self.trigger_buy(level)
                break
        
        # 检查卖出网格
        for level in self.sell_levels:
            if self.triggered[level]:
                continue
            
            if self.current_price >= level:
                self.trigger_sell(level)
                break
        
        # 重置触发状态
        for level in self.buy_levels:
            if self.triggered[level] and self.current_price > level * 1.002:
                self.triggered[level] = False
        
        for level in self.sell_levels:
            if self.triggered[level] and self.current_price < level * 0.998:
                self.triggered[level] = False
    
    def trigger_buy(self, level: float):
        """触发买入（开多）"""
        # 冷却检查：防止频繁下单触发限流
        current_time = time.time()
        if current_time - self.last_order_time < self.order_cooldown:
            return
        
        self.triggered[level] = True
        self.last_order_time = current_time
        
        # 合约：计算张数（1张=0.01BTC）
        contracts = int(self.order_amount / (self.current_price * 0.01))
        if contracts < 1:
            contracts = 1
        
        self.log_info(f"[触发] 买入开多 {contracts}张 @ {self.current_price:.2f}")
        # 双向持仓模式：买入开多用 "long"
        self.send_swap_market_order(self.symbol, "buy", contracts, "long")
    
    def trigger_sell(self, level: float):
        """触发卖出（开空）"""
        # 冷却检查：防止频繁下单触发限流
        current_time = time.time()
        if current_time - self.last_order_time < self.order_cooldown:
            return
        
        self.triggered[level] = True
        self.last_order_time = current_time
        
        # 合约：计算张数
        contracts = int(self.order_amount / (self.current_price * 0.01))
        if contracts < 1:
            contracts = 1
        
        self.log_info(f"[触发] 卖出开空 {contracts}张 @ {self.current_price:.2f}")
        # 双向持仓模式：卖出开空用 "short"
        self.send_swap_market_order(self.symbol, "sell", contracts, "short")
    
    def print_grid_summary(self):
        """打印网格统计"""
        print("\n" + "=" * 50)
        print("          网格策略统计")
        print("=" * 50)
        print(f"  交易对:     {self.symbol}")
        print(f"  基准价格:   {self.base_price:.2f}")
        print(f"  当前价格:   {self.current_price:.2f}")
        print(f"  买入成交:   {self.buy_count} 笔")
        print(f"  卖出成交:   {self.sell_count} 笔")
        print(f"  K线数量:    {self.get_kline_count(self.symbol, '1s')} 根")
        
        # 显示账户信息
        usdt = self.get_usdt_available()
        equity = self.get_total_equity()
        print(f"  USDT可用:   {usdt:.2f}")
        print(f"  总权益:     {equity:.2f}")
        
        # 显示持仓
        positions = self.get_active_positions()
        if positions:
            print("  持仓:")
            for pos in positions:
                print(f"    {pos.symbol}: {pos.quantity}张 盈亏:{pos.unrealized_pnl:.2f}")
        
        print("=" * 50)


# ============================================================
# 配置加载
# ============================================================

def load_config(config_path: str) -> Optional[Dict]:
    """从 JSON 文件加载配置"""
    try:
        with open(config_path, 'r', encoding='utf-8') as f:
            config = json.load(f)
        return config
    except FileNotFoundError:
        print(f"错误: 配置文件不存在: {config_path}")
        return None
    except json.JSONDecodeError as e:
        print(f"错误: 配置文件格式错误: {e}")
        return None
    except Exception as e:
        print(f"错误: 加载配置文件失败: {e}")
        return None


# ============================================================
# 主程序
# ============================================================

def main():
    parser = argparse.ArgumentParser(description='网格策略（模块化C++基类版）')

    # 配置文件参数（优先级最高）
    parser.add_argument('--config', type=str,
                       help='配置文件路径 (JSON格式)')

    # 策略参数（可选，用于覆盖配置文件）
    parser.add_argument('--strategy-id', type=str,
                       help='策略ID')
    parser.add_argument('--symbol', type=str,
                       help='交易对')
    parser.add_argument('--grid-num', type=int,
                       help='单边网格数量')
    parser.add_argument('--grid-spread', type=float,
                       help='网格间距 (0.001 = 0.1%%)')
    parser.add_argument('--order-amount', type=float,
                       help='单次下单金额 USDT')

    # 账户参数（可选，用于覆盖配置文件）
    parser.add_argument('--api-key', type=str,
                       help='OKX API Key')
    parser.add_argument('--secret-key', type=str,
                       help='OKX Secret Key')
    parser.add_argument('--passphrase', type=str,
                       help='OKX Passphrase')
    parser.add_argument('--testnet', action='store_true',
                       help='使用测试网')
    parser.add_argument('--live', action='store_true',
                       help='使用实盘')

    args = parser.parse_args()

    # 加载配置
    config = {}
    if args.config:
        loaded_config = load_config(args.config)
        if loaded_config is None:
            sys.exit(1)
        config = loaded_config

    # 从配置文件或命令行参数获取值（命令行参数优先）
    strategy_id = args.strategy_id or config.get('strategy_id', 'grid_cpp')

    # ★ 设置日志（在所有输出之前，尽早初始化）
    log_dir = os.path.join(script_dir, 'log')
    logger = setup_strategy_logging(exchange='okx', strategy_id=strategy_id, log_dir=log_dir)

    # 现在所有的print都会被记录
    if args.config:
        print(f"✓ 已加载配置文件: {args.config}")
    print(f"✓ 日志文件: {logger.log_file}")
    print()

    # 获取策略参数
    params = config.get('params', {})
    symbol = args.symbol or params.get('symbol', 'BTC-USDT-SWAP')
    grid_num = args.grid_num or params.get('grid_num', 20)
    grid_spread = args.grid_spread or params.get('grid_spread', 0.001)
    order_amount = args.order_amount or params.get('order_amount', 100.0)

    # 获取账户参数
    api_key = args.api_key or config.get('api_key', '')
    secret_key = args.secret_key or config.get('secret_key', '')
    passphrase = args.passphrase or config.get('passphrase', '')

    # 确定是否使用测试网
    if args.live:
        is_testnet = False
    elif args.testnet:
        is_testnet = True
    else:
        is_testnet = config.get('is_testnet', True)

    # 验证必要参数
    if not api_key or not secret_key or not passphrase:
        print("错误: 缺少 API 密钥信息")
        print("请通过以下方式之一提供:")
        print("  1. 使用 --config 指定配置文件")
        print("  2. 使用 --api-key, --secret-key, --passphrase 参数")
        sys.exit(1)

    print()
    print("╔" + "═" * 50 + "╗")
    print("║" + "  网格策略 (模块化 C++ 基类版)".center(44) + "║")
    print("╚" + "═" * 50 + "╝")
    print()
    print("策略配置:")
    print(f"  策略ID:     {strategy_id}")
    print(f"  交易对:     {symbol}")
    print(f"  网格数量:   {grid_num}")
    print(f"  网格间距:   {grid_spread * 100:.3f}%")
    print(f"  下单金额:   {order_amount} USDT")
    print(f"  交易环境:   {'测试网' if is_testnet else '实盘'}")
    print(f"  API Key:    {api_key[:10]}...")
    print()
    print("模块化设计:")
    print("  - MarketDataModule: 行情数据（K线订阅、存储）")
    print("  - TradingModule: 交易操作（下单、撤单）")
    print("  - AccountModule: 账户管理（登录、余额、持仓）")
    print()

    # 创建策略
    strategy = GridStrategy(
        strategy_id=strategy_id,
        symbol=symbol,
        grid_num=grid_num,
        grid_spread=grid_spread,
        order_amount=order_amount,
        api_key=api_key,
        secret_key=secret_key,
        passphrase=passphrase,
        is_testnet=is_testnet,
        log_file_path=logger.log_file  # 传递日志文件路径给 C++ 基类
    )
    
    # 信号处理
    def signal_handler(signum, frame):
        print("\n收到停止信号...")
        strategy.stop()
        logger.close()  # 关闭日志文件

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # 运行策略
    try:
        strategy.run()
    finally:
        logger.close()  # 确保日志文件被关闭


if __name__ == "__main__":
    main()
