#!/usr/bin/env python3
"""
================================================================================
                        策略基类 API 完整参考
================================================================================

【编译方法】
    cd cpp/build && cmake .. && make strategy_base

================================================================================
一、数据结构
================================================================================

KlineBar (K线):
    .timestamp  : int64  - 时间戳（毫秒）
    .open       : float  - 开盘价
    .high       : float  - 最高价
    .low        : float  - 最低价
    .close      : float  - 收盘价
    .volume     : float  - 成交量

TradeData (逐笔成交):
    .timestamp  : int64  - 时间戳（毫秒）
    .trade_id   : str    - 成交ID
    .price      : float  - 成交价格
    .quantity   : float  - 成交数量
    .side       : str    - "buy" / "sell"

PositionInfo (持仓):
    .symbol            : str    - 交易对
    .pos_side          : str    - "net" / "long" / "short"
    .quantity          : float  - 持仓数量（张）
    .avg_price         : float  - 持仓均价
    .mark_price        : float  - 标记价格
    .unrealized_pnl    : float  - 未实现盈亏
    .realized_pnl      : float  - 已实现盈亏
    .margin            : float  - 保证金
    .leverage          : float  - 杠杆倍数
    .liquidation_price : float  - 强平价格

BalanceInfo (余额):
    .currency   : str    - 币种 ("USDT", "BTC"...)
    .available  : float  - 可用余额
    .frozen     : float  - 冻结余额
    .total      : float  - 总余额
    .usd_value  : float  - USD估值

OrderInfo (订单):
    .client_order_id   : str   - 客户端订单ID
    .exchange_order_id : str   - 交易所订单ID
    .symbol            : str   - 交易对
    .side              : str   - "buy" / "sell"
    .order_type        : str   - "market" / "limit"
    .pos_side          : str   - "net" / "long" / "short"
    .price             : float - 价格
    .quantity          : int   - 数量（张）
    .filled_quantity   : int   - 已成交数量
    .filled_price      : float - 成交均价
    .error_msg         : str   - 错误信息

ScheduledTask (定时任务):
    .task_name         : str   - 任务名称
    .interval_ms       : int64 - 执行间隔（毫秒）
    .next_run_time_ms  : int64 - 下次执行时间（毫秒时间戳）
    .last_run_time_ms  : int64 - 上次执行时间（毫秒时间戳）
    .enabled           : bool  - 是否启用
    .run_count         : int   - 执行次数

================================================================================
二、构造函数
================================================================================

StrategyBase(strategy_id, max_kline_bars=7200, max_trades=10000, 
             max_orderbook_snapshots=1000, max_funding_rate_records=100)
    strategy_id              : str - 策略ID
    max_kline_bars           : int - K线存储数量（默认7200=2小时1sK线）
    max_trades               : int - Trades存储数量（默认10000条）
    max_orderbook_snapshots  : int - OrderBook存储数量（默认1000个快照）
    max_funding_rate_records : int - FundingRate存储数量（默认100条）

================================================================================
三、账户模块
================================================================================

register_account(api_key, secret_key, passphrase, is_testnet=True)
    api_key    : str  - OKX API Key
    secret_key : str  - OKX Secret Key  
    passphrase : str  - OKX Passphrase
    is_testnet : bool - True=模拟盘, False=实盘
    返回: bool

unregister_account()  -> bool
is_account_registered() -> bool

get_usdt_available()  -> float   # USDT可用余额
get_total_equity()    -> float   # 总权益（USD）
get_all_balances()    -> List[BalanceInfo]

get_all_positions()    -> List[PositionInfo]  # 所有持仓
get_active_positions() -> List[PositionInfo]  # 有效持仓(qty!=0)
get_position(symbol, pos_side="net") -> PositionInfo | None

refresh_account()   -> bool  # 刷新账户信息
refresh_positions() -> bool  # 刷新持仓信息

================================================================================
四、行情模块
================================================================================

【订阅】
subscribe_kline(symbol, interval) -> bool
    symbol   : str - "BTC-USDT-SWAP", "ETH-USDT-SWAP"...
    interval : str - "1s", "1m", "3m", "5m", "15m", "30m", "1H", "4H", "1D"

unsubscribe_kline(symbol, interval) -> bool
subscribe_trades(symbol) -> bool
unsubscribe_trades(symbol) -> bool

subscribe_orderbook(symbol, channel="books5") -> bool
    channel: "books5"(默认), "books", "bbo-tbt", "books-l2-tbt", "books50-l2-tbt", "books-elp"
unsubscribe_orderbook(symbol, channel="books5") -> bool

subscribe_funding_rate(symbol) -> bool  # 仅永续合约
unsubscribe_funding_rate(symbol) -> bool

【查询】
get_klines(symbol, interval)           -> List[KlineBar]
get_closes(symbol, interval)           -> List[float]
get_opens(symbol, interval)            -> List[float]
get_highs(symbol, interval)            -> List[float]
get_lows(symbol, interval)             -> List[float]
get_volumes(symbol, interval)          -> List[float]
get_recent_klines(symbol, interval, n) -> List[KlineBar]
get_last_kline(symbol, interval)       -> KlineBar | None
get_kline_count(symbol, interval)      -> int

get_trades(symbol)                     -> List[TradeData]
get_recent_trades(symbol, n)            -> List[TradeData]
get_trades_by_time(symbol, time_ms)    -> List[TradeData]  # 最近N毫秒内
get_last_trade(symbol)                  -> TradeData | None
get_trade_count(symbol)                 -> int

get_orderbooks(symbol, channel="books5")              -> List[OrderBookSnapshot]
get_recent_orderbooks(symbol, n, channel="books5")   -> List[OrderBookSnapshot]
get_orderbooks_by_time(symbol, time_ms, channel="books5") -> List[OrderBookSnapshot]
get_last_orderbook(symbol, channel="books5")          -> OrderBookSnapshot | None
get_orderbook_count(symbol, channel="books5")         -> int

get_funding_rates(symbol)               -> List[FundingRateData]
get_recent_funding_rates(symbol, n)    -> List[FundingRateData]
get_funding_rates_by_time(symbol, time_ms) -> List[FundingRateData]
get_last_funding_rate(symbol)          -> FundingRateData | None
get_funding_rate_count(symbol)         -> int

================================================================================
五、交易模块
================================================================================

【下单】
send_swap_market_order(symbol, side, quantity, pos_side="net") -> str
    symbol   : str - 交易对 "BTC-USDT-SWAP"
    side     : str - "buy" / "sell"
    quantity : int - 张数
    pos_side : str - "net"(默认), "long", "short"
    返回: 客户端订单ID

send_swap_limit_order(symbol, side, quantity, price, pos_side="net") -> str
    price : float - 限价

send_swap_market_order_with_tp_sl(symbol, side, quantity, 
                                   tp_trigger_px="", tp_ord_px="",
                                   sl_trigger_px="", sl_ord_px="",
                                   pos_side="net", tag="") -> str
    发送合约市价订单（带止盈止损）
    tp_trigger_px: str - 止盈触发价（可选，空字符串表示不设置）
    tp_ord_px    : str - 止盈委托价（可选，"-1"表示市价，空字符串表示不设置）
    sl_trigger_px: str - 止损触发价（可选，空字符串表示不设置）
    sl_ord_px    : str - 止损委托价（可选，"-1"表示市价，空字符串表示不设置）
    tag          : str - 订单标签（可选，1-16位）
    返回: 客户端订单ID

send_swap_limit_order_with_tp_sl(symbol, side, quantity, price,
                                   tp_trigger_px="", tp_ord_px="",
                                   sl_trigger_px="", sl_ord_px="",
                                   pos_side="net", tag="") -> str
    发送合约限价订单（带止盈止损）
    price        : float - 限价
    其他参数同上

send_swap_advanced_order(symbol, side, quantity, price, ord_type,
                          pos_side="net", tag="") -> str
    发送高级订单类型
    ord_type: str - 订单类型: "post_only"(只做maker), "fok"(全部成交或取消), "ioc"(立即成交并取消剩余)
    返回: 客户端订单ID

send_batch_orders(orders) -> List[str]
    批量下单（最多20个订单）
    orders: List[dict] - 订单列表，每个订单是一个字典，包含:
        - symbol: str - 交易对
        - side: str - "buy" / "sell"
        - order_type: str - "market" / "limit" / "post_only" / "fok" / "ioc"
        - quantity: int - 数量（张）
        - price: float - 价格（限价单必填，市价单为0）
        - pos_side: str - 持仓方向（可选，默认"net"）
        - tag: str - 订单标签（可选）
        - tp_trigger_px: str - 止盈触发价（可选）
        - tp_ord_px: str - 止盈委托价（可选，"-1"表示市价）
        - sl_trigger_px: str - 止损触发价（可选）
        - sl_ord_px: str - 止损委托价（可选，"-1"表示市价）
    返回: List[str] - 订单ID列表（与输入订单顺序对应）

【撤单】
cancel_order(symbol, client_order_id) -> bool
cancel_all_orders(symbol="") -> bool  # 空=撤销全部

【查询】
get_active_orders()   -> List[OrderInfo]
pending_order_count() -> int

================================================================================
六、定时任务模块
================================================================================

schedule_task(task_name, interval, start_time="") -> bool
    task_name  : str - 任务名称（在 on_scheduled_task 回调中返回）
    interval   : str - 执行间隔:
        "30s", "60s"        - 秒
        "1m", "5m", "30m"   - 分钟
        "1h", "4h"          - 小时
        "1d"                - 天
        "1w"                - 周
    start_time : str - 首次执行时间，格式 "HH:MM"（如 "14:00"）
        空字符串或"now"表示立即开始
        如果指定时间已过，会自动计算下一个执行时间

unschedule_task(task_name) -> bool    # 取消任务
pause_task(task_name)      -> bool    # 暂停任务
resume_task(task_name)     -> bool    # 恢复任务

get_scheduled_tasks()          -> List[ScheduledTask]  # 所有任务
get_task_info(task_name)       -> ScheduledTask | None

================================================================================
七、回调函数（需重写）
================================================================================

on_init()                                    # 初始化
on_stop()                                    # 停止
on_tick()                                    # 每次循环（约100us）

on_kline(symbol, interval, bar: KlineBar)    # K线回调
on_trade(symbol, trade: TradeData)           # 逐笔成交回调
on_orderbook(symbol, snapshot: OrderBookSnapshot)  # 深度数据回调
on_funding_rate(symbol, fr: FundingRateData)       # 资金费率回调

on_order_report(report: dict)                # 订单回报
    report["status"]: "accepted", "rejected", "filled", 
                      "partially_filled", "cancelled", "live", "failed"
    report["symbol"], report["side"], report["client_order_id"]
    report["filled_quantity"], report["filled_price"], report["error_msg"]

on_register_report(success: bool, error_msg: str)  # 账户注册回报
on_position_update(position: PositionInfo)         # 持仓更新
on_balance_update(balance: BalanceInfo)            # 余额更新
on_scheduled_task(task_name: str)                  # 定时任务回调

================================================================================
八、日志与属性
================================================================================

log_info(msg)   # 输出信息
log_error(msg)  # 输出错误

【只读属性】
.strategy_id  : str
.is_running   : bool
.kline_count  : int
.order_count  : int
.report_count : int

================================================================================
"""

import sys
import signal
import time

try:
    from strategy_base import (
        StrategyBase, KlineBar, TradeData, OrderBookSnapshot, FundingRateData,
        PositionInfo, BalanceInfo, OrderInfo, ScheduledTask
    )
except ImportError:
    print("错误: 未找到 strategy_base 模块，请先编译:")
    print("  cd cpp/build && cmake .. && make strategy_base")
    sys.exit(1)


class ExampleStrategy(StrategyBase):
    """示例策略 - 展示所有接口的实际调用方式"""
    
    def __init__(self, strategy_id: str, symbol: str, 
                 max_kline_bars: int = 7200,
                 max_trades: int = 10000,
                 max_orderbook_snapshots: int = 1000,
                 max_funding_rate_records: int = 100):
        """
        构造函数
        
        Args:
            strategy_id: 策略唯一标识符，用于区分不同策略实例
            symbol: 默认的交易对，如 "BTC-USDT-SWAP"（一个变量而已，比如下文订阅k线可以用self.symbol来调用，方便全局修改）
            max_kline_bars: 本地K线最大存储数量，默认7200（2h的1sK线数据）（如果订阅会本地保存历史数据，下同）
            max_trades: 本地逐笔成交最大存储数量，默认10000条
            max_orderbook_snapshots: 本地深度快照最大存储数量，默认1000个
            max_funding_rate_records: 本地资金费率最大存储数量，默认100条
        """
        super().__init__(strategy_id, 
                        max_kline_bars=max_kline_bars,      # K线存储上限
                        max_trades=max_trades,               # Trades存储上限
                        max_orderbook_snapshots=max_orderbook_snapshots,  # OrderBook存储上限
                        max_funding_rate_records=max_funding_rate_records)  # FundingRate存储上限
        self.symbol = symbol
        self.kline_count_local = 0
    
    # ======================== 生命周期 ========================
    
    def on_init(self):
        """
        策略初始化
        
        触发时机: 策略启动时，在连接服务器成功后自动调用一次
        用途: 在此方法中完成账户注册、订阅行情、注册定时任务等初始化工作
        """
        # 1. 注册账户
        # register_account: 向服务器注册API密钥，用于后续下单和查询账户信息
        # 返回: bool，表示注册请求是否成功发送（实际注册结果通过 on_register_report 回调返回）
        self.register_account(
            api_key="your-api-key",        # OKX API Key
            secret_key="your-secret-key",  # OKX Secret Key
            passphrase="your-passphrase",  # OKX Passphrase
            is_testnet=True                # True=模拟盘, False=实盘
        )
        time.sleep(1)  # 等待注册请求处理
        
        # 2. 订阅K线数据
        # subscribe_kline: 订阅指定交易对和周期的K线数据
        #   symbol: 交易对，如 "BTC-USDT-SWAP", "ETH-USDT-SWAP" 等
        #   interval: K线周期，可选值: "1s", "1m", "3m", "5m", "15m", "30m", "1H", "4H", "1D"
        #   返回: bool，表示订阅请求是否成功发送
        #   注意: 订阅成功后，新的K线数据会通过 on_kline 回调函数推送
        #   注意: 服务器只推送已完结的K线（confirm=1），未完结的不推送
        #   注意：已经收到的k线会自动存储到内存，可可通过查询接口获取历史数据，示例在 query_example函数中
        self.subscribe_kline(self.symbol, "1s")   # 订阅1秒K线
        self.subscribe_kline(self.symbol, "1m")   # 订阅1分钟K线（可同时订阅多个周期）
        self.subscribe_kline("ETH-USDT-SWAP", "1s")  # 可订阅多个交易对
        
        # 3. 订阅逐笔成交数据
        # subscribe_trades: 订阅指定交易对的逐笔成交数据
        #   symbol: 交易对，如 "BTC-USDT-SWAP"
        #   返回: bool，表示订阅请求是否成功发送
        #   注意: 订阅成功后，每笔成交会通过 on_trade 回调函数推送
        #   注意: 成交数据会自动存储到内存缓冲区，可通过查询接口获取历史数据，示例在 query_example函数中
        self.subscribe_trades(self.symbol)
        
        # 4. 订阅深度数据（OrderBook） 【测试中】
        # subscribe_orderbook: 订阅指定交易对的订单簿深度数据
        #   symbol: 交易对，如 "BTC-USDT-SWAP"
        #   channel: 深度频道类型，可选值:
        #     - "books5" (默认): 5档深度，推送频率100ms，适合一般交易策略
        #     - "books": 400档深度，推送频率100ms，适合需要完整深度的策略
        #     - "bbo-tbt": 1档最优买卖价，推送频率10ms，适合高频策略
        #     - "books-l2-tbt": 400档深度，推送频率10ms，适合高频策略
        #     - "books50-l2-tbt": 50档深度，推送频率10ms
        #     - "books-elp": 增强限价单深度
        #   返回: bool，表示订阅请求是否成功发送
        #   注意: 订阅成功后，深度快照会通过 on_orderbook 回调函数推送
        #   注意: 深度数据会自动存储到内存缓冲区，可通过查询接口获取历史快照，示例在 query_example函数中
        self.subscribe_orderbook(self.symbol, "books5")  # 订阅5档深度（推荐）
        self.subscribe_orderbook(self.symbol, "books")   # 订阅400档深度
        self.subscribe_orderbook(self.symbol, "bbo-tbt") # 订阅1档最优价（高频）
        
        # 5. 订阅资金费率数据 【测试中】
        # subscribe_funding_rate: 订阅永续合约的资金费率数据
        #   symbol: 交易对，必须是永续合约（如 "BTC-USDT-SWAP"），现货不支持
        #   返回: bool，表示订阅请求是否成功发送
        #   注意: 资金费率推送频率为30-90秒一次（OKX服务器决定）
        #   注意: 订阅成功后，资金费率更新会通过 on_funding_rate 回调函数推送
        #   注意: 资金费率数据会自动存储到内存缓冲区，可通过查询接口获取历史数据，示例在 query_example函数中
        self.subscribe_funding_rate(self.symbol)
        
        # 6. 注册定时任务
        # schedule_task: 注册一个定时函数
        #   task_name: 函数名称
        #   interval: 执行间隔，格式:
        #     - "30s", "60s": 秒级间隔
        #     - "1m", "5m", "30m": 分钟级间隔
        #     - "1h", "4h": 小时级间隔
        #     - "1d": 天级间隔
        #     - "1w": 周级间隔
        #   start_time: 首次执行时间，格式 "HH:MM"（如 "14:00"）
        #     - 空字符串或 "now": 立即开始执行
        #     - 如果指定时间已过，会自动计算下一个执行时间
        #   返回: bool，表示任务是否注册成功
        #   注意: 任务注册成功后，会在指定时间通过 on_scheduled_task 回调函数执行
        #   注意: 可以注册多个任务，通过 task_name 区分
        self.schedule_task("function_name1", "10s")  # 每10秒执行一次（立即开始）
        self.schedule_task("function_name2", "1d", "14:00") # 每天14:00执行（用于日度调仓等）
        self.schedule_task("function_name3", "1w", "09:00")  # 每周执行（用于周度报告等）
        
        # log_info: 输出信息日志
        #   msg: str，要输出的日志信息
        #   注意: 日志会显示策略ID前缀，格式: [strategy_id] msg
        self.log_info(f"策略初始化完成，订阅 {self.symbol}")
    
    def on_stop(self):
        """
        策略停止回调
        
        触发时机: 策略停止时（调用 stop() 或收到 SIGINT/SIGTERM 信号）自动调用一次
        用途: 在此方法中完成取消订阅、取消定时任务、打印统计信息等清理工作
        """
        # 取消订阅K线
        # unsubscribe_kline: 取消订阅指定交易对和周期的K线数据
        #   symbol: 交易对，必须与订阅时一致
        #   interval: K线周期，必须与订阅时一致
        #   返回: bool，表示取消订阅请求是否成功发送
        self.unsubscribe_kline(self.symbol, "1s")
        
        # 取消订阅逐笔成交
        # unsubscribe_trades: 取消订阅指定交易对的逐笔成交数据
        #   symbol: 交易对，必须与订阅时一致
        #   返回: bool，表示取消订阅请求是否成功发送
        self.unsubscribe_trades(self.symbol)
        
        # 取消订阅深度数据
        # unsubscribe_orderbook: 取消订阅指定交易对和频道的深度数据
        #   symbol: 交易对，必须与订阅时一致
        #   channel: 深度频道类型，必须与订阅时一致（默认 "books5"）
        #   返回: bool，表示取消订阅请求是否成功发送
        self.unsubscribe_orderbook(self.symbol, "books5")
        
        # 取消订阅资金费率
        # unsubscribe_funding_rate: 取消订阅指定交易对的资金费率数据
        #   symbol: 交易对，必须与订阅时一致
        #   返回: bool，表示取消订阅请求是否成功发送
        self.unsubscribe_funding_rate(self.symbol)
        
        # 取消定时任务
        # unschedule_task: 取消已注册的定时任务
        #   task_name: 任务名称，必须与注册时一致
        #   返回: bool，表示任务是否取消成功
        self.unschedule_task("check_position")
        self.unschedule_task("daily_rebalance")
        self.unschedule_task("weekly_report")
        
        # 获取所有定时任务
        # get_scheduled_tasks: 获取所有已注册的定时任务列表
        #   返回: List[ScheduledTask]，包含所有任务的信息（名称、执行次数、下次执行时间等）
        for task in self.get_scheduled_tasks():
            self.log_info(f"任务 {task.function_name} 共执行 {task.run_count} 次")
        
        # 打印统计信息（示例，未必需要，可以当接口示例看看）
        # get_kline_count: 获取指定交易对和周期的K线存储数量
        #   symbol: 交易对
        #   interval: K线周期
        #   返回: int，当前存储的K线数量
        self.log_info(f"K线数量: {self.get_kline_count(self.symbol, '1s')}")
        
        # get_trade_count: 获取指定交易对的成交数据存储数量
        #   symbol: 交易对
        #   返回: int，当前存储的成交数量
        self.log_info(f"成交数量: {self.get_trade_count(self.symbol)}")
        
        # get_orderbook_count: 获取指定交易对和频道的深度快照存储数量
        #   symbol: 交易对
        #   channel: 深度频道类型（默认 "books5"）
        #   返回: int，当前存储的快照数量
        self.log_info(f"深度快照数: {self.get_orderbook_count(self.symbol, 'books5')}")
        
        # get_funding_rate_count: 获取指定交易对的资金费率记录存储数量
        #   symbol: 交易对
        #   返回: int，当前存储的记录数量
        self.log_info(f"资金费率记录数: {self.get_funding_rate_count(self.symbol)}")
        
        # get_usdt_available: 获取USDT可用余额
        #   返回: float，USDT可用余额（单位：USDT）
        self.log_info(f"USDT余额: {self.get_usdt_available():.2f}")
        
        # 获取有效持仓
        # get_active_positions: 获取所有有效持仓（数量不为0的持仓）
        #   返回: List[PositionInfo]，包含所有有效持仓的信息
        #   PositionInfo 包含: symbol, pos_side, quantity, avg_price, unrealized_pnl 等
        for pos in self.get_active_positions():
            self.log_info(f"持仓: {pos.symbol} {pos.quantity}张 盈亏:{pos.unrealized_pnl:.2f}")

     # ======================== 定时任务方法 ========================
    
    def function_name1(self):
        """
        定时任务示例，检查持仓（每10秒执行）
        函数名要和初始化中的函数名一致
        """
        # get_active_positions: 获取所有有效持仓（数量不为0的持仓）
        #   返回: List[PositionInfo]，包含所有有效持仓的信息
        positions = self.get_active_positions()
        if positions:
            for pos in positions:
                self.log_info(f"[定时检查] {pos.symbol} 持仓:{pos.quantity}张 盈亏:{pos.unrealized_pnl:.2f}")
        else:
            self.log_info("[定时检查] 无持仓")
    
    def function_name2(self):
       # do nothing, just for example
        return
       
    def function_name3(self):
        # do nothing, just for example
        return
    
    # ======================== 行情回调 ========================
    
    def on_kline(self, symbol: str, interval: str, bar: KlineBar):
        """
        K线数据回调
        
        触发时机: 每当收到一条新的已完结K线数据时自动调用（同步执行）
        参数:
            symbol: 交易对，如 "BTC-USDT-SWAP"
            interval: K线周期，如 "1s", "1m" 等
            bar: KlineBar 对象，包含:
                - timestamp: 时间戳（毫秒）
                - open: 开盘价
                - high: 最高价
                - low: 最低价
                - close: 收盘价
                - volume: 成交量
        注意: 服务器只推送已完结的K线（confirm=1），未完结的不推送
        注意: 回调是同步执行的，避免在此方法中执行耗时操作
        """

        # 示例处理，
        if symbol != self.symbol: # 如果交易对不是默认的交易对，则不处理
            return
        
        self.kline_count_local += 1
        
        # 每30根K线打印一次
        if self.kline_count_local % 30 == 0:
            # get_closes: 获取指定交易对和周期的收盘价数组
            #   symbol: 交易对
            #   interval: K线周期
            #   返回: List[float]，按时间顺序排列的收盘价数组
            closes = self.get_closes(symbol, interval)
            self.log_info(f"[K线] {symbol} 收盘:{bar.close:.2f} 存储:{len(closes)}根")
    
    def on_trade(self, symbol: str, trade: TradeData):
        """
        逐笔成交数据回调
        
        触发时机: 每当收到一笔新的成交数据时自动调用（同步执行）
        参数:
            symbol: 交易对，如 "BTC-USDT-SWAP"
            trade: TradeData 对象，包含:
                - timestamp: 时间戳（毫秒）
                - trade_id: 成交ID
                - price: 成交价格
                - quantity: 成交数量
                - side: 买卖方向（"buy" 或 "sell"）
        注意: 成交数据会自动存储到内存缓冲区，可通过查询接口获取历史数据
        注意: 回调是同步执行的，避免在此方法中执行耗时操作
        """

        self.log_info(f"[Trade] {symbol} {trade.side} {trade.quantity} @ {trade.price}")
    
    def on_orderbook(self, symbol: str, snapshot: OrderBookSnapshot):
        """
        深度数据回调
        
        触发时机: 每当收到一个新的深度快照时自动调用（同步执行）
        参数:
            symbol: 交易对，如 "BTC-USDT-SWAP"
            snapshot: OrderBookSnapshot 对象，包含:
                - timestamp: 时间戳（毫秒）
                - bids: 买盘列表 [(price, size), ...]，按价格从高到低排序
                - asks: 卖盘列表 [(price, size), ...]，按价格从低到高排序
                - best_bid_price: 最优买价
                - best_bid_size: 最优买量
                - best_ask_price: 最优卖价
                - best_ask_size: 最优卖量
                - mid_price: 中间价（(best_bid + best_ask) / 2）
                - spread: 价差（best_ask - best_bid）
        注意: 深度快照会自动存储到内存缓冲区，可通过查询接口获取历史快照
        注意: 推送频率取决于订阅的频道类型（books5: 100ms, bbo-tbt: 10ms 等）
        注意: 回调是同步执行的，避免在此方法中执行耗时操作
        """

        self.log_info(f"[OrderBook] {symbol} 买价:{snapshot.best_bid_price:.2f} "
                     f"卖价:{snapshot.best_ask_price:.2f} 中间价:{snapshot.mid_price:.2f}")
    
    def on_funding_rate(self, symbol: str, fr: FundingRateData):
        """
        资金费率数据回调
        
        触发时机: 每当收到新的资金费率数据时自动调用（同步执行）
        参数:
            symbol: 交易对，必须是永续合约（如 "BTC-USDT-SWAP"）
            fr: FundingRateData 对象，包含:
                - timestamp: 数据更新时间（毫秒）
                - funding_rate: 当前资金费率（小数形式，如 0.0001 表示 0.01%）
                - next_funding_rate: 下一期预测资金费率
                - funding_time: 资金费时间（毫秒时间戳）
                - next_funding_time: 下一期资金费时间（毫秒时间戳）
                - min_funding_rate: 资金费率下限
                - max_funding_rate: 资金费率上限
                - premium: 溢价指数
                - sett_state: 结算状态（"processing" 或 "settled"）
        注意: 资金费率推送频率为30-90秒一次（由OKX服务器决定）
        注意: 资金费率数据会自动存储到内存缓冲区，可通过查询接口获取历史数据
        注意: 回调是同步执行的，避免在此方法中执行耗时操作
        """

        self.log_info(f"[FundingRate] {symbol} 费率:{fr.funding_rate:.6f} "
                     f"下一期:{fr.next_funding_rate:.6f}")
    
    # ======================== 订单回调 ========================
    
    def on_order_report(self, report: dict):
        """
        订单回报回调
        
        触发时机: 每当订单状态发生变化时自动调用（同步执行）
        参数:
            report: dict，包含订单回报信息:
                - status: 订单状态，可能的值:
                    "accepted": 订单已接受（已提交到交易所）
                    "rejected": 订单被拒绝（下单失败）
                    "filled": 订单已完全成交
                    "partially_filled": 订单部分成交
                    "cancelled": 订单已取消
                    "live": 订单仍在挂单中（未成交）
                    "failed": 订单失败
                - symbol: 交易对，如 "BTC-USDT-SWAP"
                - side: 买卖方向，"buy" 或 "sell"
                - client_order_id: 客户端订单ID（下单时返回的ID）
                - exchange_order_id: 交易所订单ID
                - filled_quantity: 已成交数量（张）
                - filled_price: 成交均价
                - error_msg: 错误信息（如果订单失败）
        注意: 此回调会收到订单的所有状态变化（接受、成交、取消等）
        注意: 回调是同步执行的，避免在此方法中执行耗时操作
        """

        status = report.get("status", "")
        symbol = report.get("symbol", "")
        side = report.get("side", "")
        
        if status == "filled":
            self.log_info(f"[成交] {symbol} {side} {report.get('filled_quantity')}张")
        elif status == "rejected":
            self.log_error(f"[拒绝] {symbol} {side} 原因:{report.get('error_msg')}")
    
    # ======================== 账户回调 ========================
    
    def on_register_report(self, success: bool, error_msg: str):
        """
        账户注册回报回调
        
        触发时机: 账户注册请求处理完成后自动调用（同步执行）
        参数:
            success: bool，True表示注册成功，False表示注册失败
            error_msg: str，如果注册失败，包含错误信息；如果成功，为空字符串
        注意: 注册成功后，可以调用 get_usdt_available() 等接口查询账户信息
        注意: 注册失败时，需要检查 API Key、Secret Key、Passphrase 是否正确
        注意: 回调是同步执行的，避免在此方法中执行耗时操作
        """

        if success:
            # get_usdt_available: 获取USDT可用余额
            #   返回: float，USDT可用余额（单位：USDT）
            self.log_info(f"账户注册成功，USDT:{self.get_usdt_available():.2f}")
        else:
            self.log_error(f"账户注册失败: {error_msg}")
    
    def on_position_update(self, position: PositionInfo):
        """
        持仓更新回调
        
        触发时机: 每当持仓发生变化时自动调用（同步执行）
        参数:
            position: PositionInfo 对象，包含持仓信息:
                - symbol: 交易对，如 "BTC-USDT-SWAP"
                - pos_side: 持仓方向，"net"（净持仓）、"long"（多仓）、"short"（空仓）
                - quantity: 持仓数量（张），正数表示多仓，负数表示空仓，0表示无持仓
                - avg_price: 持仓均价
                - mark_price: 标记价格
                - unrealized_pnl: 未实现盈亏（USD）
                - realized_pnl: 已实现盈亏（USD）
                - margin: 保证金
                - leverage: 杠杆倍数
                - liquidation_price: 强平价格
        注意: 持仓变化包括开仓、平仓、部分平仓、追加保证金等
        注意: quantity=0 表示无持仓，此时可能仍会收到回调（持仓清零）
        注意: 回调是同步执行的，避免在此方法中执行耗时操作
        """

        if position.quantity != 0:
            self.log_info(f"[持仓] {position.symbol} {position.quantity}张 盈亏:{position.unrealized_pnl:.2f}")
    
    def on_balance_update(self, balance: BalanceInfo):
        """
        余额更新回调
        
        触发时机: 每当账户余额发生变化时自动调用（同步执行）
        参数:
            balance: BalanceInfo 对象，包含余额信息:
                - currency: 币种，如 "USDT", "BTC" 等
                - available: 可用余额
                - frozen: 冻结余额（下单冻结的资金）
                - total: 总余额（available + frozen）
                - usd_value: USD估值
        注意: 余额变化包括充值、提现、下单冻结、成交释放、资金费用等
        注意: 可能同时收到多个币种的余额更新（如 USDT 和 BTC）
        注意: 回调是同步执行的，避免在此方法中执行耗时操作
        """

        if balance.currency == "USDT":
            self.log_info(f"[余额] USDT 可用:{balance.available:.2f}")
    
    # ======================== 交易示例 ========================
    
    def place_order_example(self):
        """
        下单示例（在需要时调用）
        
        用途: 演示如何使用下单接口，实际使用时可在策略逻辑中调用
        """
        
        # 市价买入
        # send_swap_market_order: 发送永续合约市价单
        #   symbol: 交易对，如 "BTC-USDT-SWAP"
        #   side: 买卖方向，"buy"（买入）或 "sell"（卖出）
        #   quantity: 数量（张），必须是整数，最小为1张
        #   pos_side: 持仓方向，"net"（净持仓，默认）、"long"（多仓）、"short"（空仓）
        #   返回: str，客户端订单ID（用于后续撤单、查询等）
        #   注意: 订单提交后，结果通过 on_order_report 回调返回
        order_id = self.send_swap_market_order(self.symbol, "buy", 1, "net")
        self.log_info(f"买入订单: {order_id}")
        
        # 市价卖出
        #   注意: pos_side 参数可以省略，默认使用 "net"
        order_id = self.send_swap_market_order(self.symbol, "sell", 1)
        self.log_info(f"卖出订单: {order_id}")
        
        # 限价单
        # send_swap_limit_order: 发送永续合约限价单
        #   symbol: 交易对
        #   side: 买卖方向，"buy" 或 "sell"
        #   quantity: 数量（张），必须是整数
        #   price: 限价（float），必须大于0
        #   pos_side: 持仓方向（默认 "net"）
        #   返回: str，客户端订单ID
        order_id = self.send_swap_limit_order(self.symbol, "buy", 1, 80000.0)
        self.log_info(f"限价订单: {order_id}")
        
        # 市价单（带止盈止损）
        # send_swap_market_order_with_tp_sl: 发送合约市价订单（带止盈止损）
        #   symbol: 交易对
        #   side: 买卖方向，"buy" 或 "sell"
        #   quantity: 数量（张），必须是整数
        #   tp_trigger_px: 止盈触发价（str，可选，空字符串表示不设置）
        #   tp_ord_px: 止盈委托价（str，可选，"-1"表示市价）
        #   sl_trigger_px: 止损触发价（str，可选，空字符串表示不设置）
        #   sl_ord_px: 止损委托价（str，可选，"-1"表示市价）
        #   tag: 订单标签（str，可选，1-16位）
        order_id = self.send_swap_market_order_with_tp_sl(
            self.symbol,            # 交易对
            "buy",                  # 买卖方向
            1,                      # 数量（张）
            tp_trigger_px="90000",  # 止盈触发价
            tp_ord_px="-1",         # 市价止盈
            sl_trigger_px="80000",  # 止损触发价
            sl_ord_px="-1",         # 市价止损
            tag="with_tp_sl"        # 订单标签
        )
        
        # 限价单（带止盈止损）
        # send_swap_limit_order_with_tp_sl: 发送合约限价订单（带止盈止损）
        #   参数同上，额外需要 price 参数
        order_id = self.send_swap_limit_order_with_tp_sl(
            self.symbol, "sell", 1, 85000.0,
            tp_trigger_px="84000",
            tp_ord_px="-1",
            sl_trigger_px="86000",
            sl_ord_px="-1",
            tag="limit_tp_sl"
        )
        
        # 高级订单类型
        # send_swap_advanced_order: 发送高级订单类型
        #   ord_type: 订单类型
        #     - "post_only": 只做maker单（如果会吃掉深度，则全部撤销）
        #     - "fok": 全部成交或立即取消（如果无法全部成交，则全部撤销）
        #     - "ioc": 立即成交并取消剩余（立即按委托价撮合，剩余数量取消）
        # order_id = self.send_swap_advanced_order(
        #     self.symbol, "sell", 1, 85000.0,
        #     ord_type="post_only",  # 只做maker
        #     tag="post_only"
        # )
        
        # 批量下单
        # send_batch_orders: 批量下单（最多20个订单）
        #   orders: List[dict]，每个订单是一个字典
        #   返回: List[str]，订单ID列表（与输入顺序对应）
        # orders = [
        #     {
        #         "symbol": self.symbol,
        #         "side": "buy",
        #         "order_type": "limit",
        #         "quantity": 1,
        #         "price": 80000.0,
        #         "pos_side": "net",
        #         "tag": "batch_1"
        #     },
        #     {
        #         "symbol": self.symbol,
        #         "side": "sell",
        #         "order_type": "limit",
        #         "quantity": 1,
        #         "price": 85000.0,
        #         "pos_side": "net",
        #         "tag": "batch_2",
        #         "tp_trigger_px": "84000",  # 带止盈止损
        #         "tp_ord_px": "-1",
        #         "sl_trigger_px": "86000",
        #         "sl_ord_px": "-1"
        #     }
        # ]
        # order_ids = self.send_batch_orders(orders)
        # self.log_info(f"批量下单: {len(order_ids)} 个订单")
        
        # 撤单
        # cancel_order: 撤销指定订单
        #   symbol: 交易对，必须与下单时一致
        #   client_order_id: 客户端订单ID（下单时返回的ID）
        #   返回: bool，表示撤单请求是否成功发送
        #   注意: 撤单结果通过 on_order_report 回调返回（status="cancelled"）
        self.cancel_order(self.symbol, order_id)
        
        # 撤销所有订单
        # cancel_all_orders: 撤销所有订单或指定交易对的所有订单
        #   symbol: 交易对，如果为空字符串则撤销所有交易对的所有订单
        #   返回: bool，表示撤单请求是否成功发送
        #   注意: 批量撤单结果通过多个 on_order_report 回调返回
        self.cancel_all_orders()
    
    def query_example(self):
        """
        查询示例（在需要时调用）
        
        用途: 演示如何使用各种查询接口，实际使用时可在策略逻辑中调用
        """
        # ========== K线数据查询 ==========
        
        # get_klines: 获取所有K线数据
        #   symbol: 交易对
        #   interval: K线周期
        #   返回: List[KlineBar]，按时间顺序排列的K线数组
        klines = self.get_klines(self.symbol, "1s")
        
        # get_closes: 获取收盘价数组
        #   返回: List[float]，按时间顺序排列的收盘价数组
        closes = self.get_closes(self.symbol, "1s")
        
        # get_opens: 获取开盘价数组
        #   返回: List[float]，按时间顺序排列的开盘价数组
        opens = self.get_opens(self.symbol, "1s")
        
        # get_highs: 获取最高价数组
        #   返回: List[float]，按时间顺序排列的最高价数组
        highs = self.get_highs(self.symbol, "1s")
        
        # get_lows: 获取最低价数组
        #   返回: List[float]，按时间顺序排列的最低价数组
        lows = self.get_lows(self.symbol, "1s")
        
        # get_volumes: 获取成交量数组
        #   返回: List[float]，按时间顺序排列的成交量数组
        volumes = self.get_volumes(self.symbol, "1s")
        
        # get_recent_klines: 获取最近N根K线
        #   symbol: 交易对
        #   interval: K线周期
        #   n: 数量（int），获取最近N根K线
        #   返回: List[KlineBar]，按时间顺序排列的K线数组（最多N根）
        recent = self.get_recent_klines(self.symbol, "1s", 20)
        
        # get_last_kline: 获取最后一根K线
        #   symbol: 交易对
        #   interval: K线周期
        #   返回: KlineBar | None，如果无数据返回 None
        last = self.get_last_kline(self.symbol, "1s")
        
        # ========== Trades数据查询 ==========
        
        # get_trades: 获取所有成交数据
        #   symbol: 交易对
        #   返回: List[TradeData]，按时间顺序排列的成交数组
        trades = self.get_trades(self.symbol)
        
        # get_recent_trades: 获取最近N条成交
        #   symbol: 交易对
        #   n: 数量（int），获取最近N条成交
        #   返回: List[TradeData]，按时间顺序排列的成交数组（最多N条）
        recent_trades = self.get_recent_trades(self.symbol, 100)
        
        # get_trades_by_time: 获取最近N毫秒内的成交
        #   symbol: 交易对
        #   time_ms: 时间范围（毫秒），如 60000 表示最近1分钟
        #   返回: List[TradeData]，时间戳在指定范围内的成交数组
        trades_1min = self.get_trades_by_time(self.symbol, 60000)  # 最近1分钟
        
        # get_last_trade: 获取最后一条成交
        #   symbol: 交易对
        #   返回: TradeData | None，如果无数据返回 None
        last_trade = self.get_last_trade(self.symbol)
        
        # get_trade_count: 获取成交数量
        #   symbol: 交易对
        #   返回: int，当前存储的成交数量
        trade_count = self.get_trade_count(self.symbol)
        
        # ========== OrderBook数据查询 ==========
        
        # get_orderbooks: 获取所有深度快照
        #   symbol: 交易对
        #   channel: 深度频道类型（默认 "books5"），必须与订阅时一致
        #   返回: List[OrderBookSnapshot]，按时间顺序排列的快照数组
        orderbooks = self.get_orderbooks(self.symbol, "books5")
        
        # get_recent_orderbooks: 获取最近N个快照
        #   symbol: 交易对
        #   n: 数量（int），获取最近N个快照
        #   channel: 深度频道类型（默认 "books5"）
        #   返回: List[OrderBookSnapshot]，按时间顺序排列的快照数组（最多N个）
        recent_ob = self.get_recent_orderbooks(self.symbol, 10, "books5")
        
        # get_orderbooks_by_time: 获取最近N毫秒内的快照
        #   symbol: 交易对
        #   time_ms: 时间范围（毫秒），如 60000 表示最近1分钟
        #   channel: 深度频道类型（默认 "books5"）
        #   返回: List[OrderBookSnapshot]，时间戳在指定范围内的快照数组
        ob_1min = self.get_orderbooks_by_time(self.symbol, 60000, "books5")
        
        # get_last_orderbook: 获取最后一个快照
        #   symbol: 交易对
        #   channel: 深度频道类型（默认 "books5"）
        #   返回: OrderBookSnapshot | None，如果无数据返回 None
        last_ob = self.get_last_orderbook(self.symbol, "books5")
        
        # get_orderbook_count: 获取快照数量
        #   symbol: 交易对
        #   channel: 深度频道类型（默认 "books5"）
        #   返回: int，当前存储的快照数量
        ob_count = self.get_orderbook_count(self.symbol, "books5")
        
        # ========== FundingRate数据查询 ==========
        
        # get_funding_rates: 获取所有资金费率数据
        #   symbol: 交易对（必须是永续合约）
        #   返回: List[FundingRateData]，按时间顺序排列的记录数组
        funding_rates = self.get_funding_rates(self.symbol)
        
        # get_recent_funding_rates: 获取最近N条记录
        #   symbol: 交易对
        #   n: 数量（int），获取最近N条记录
        #   返回: List[FundingRateData]，按时间顺序排列的记录数组（最多N条）
        recent_fr = self.get_recent_funding_rates(self.symbol, 10)
        
        # get_funding_rates_by_time: 获取最近N毫秒内的记录
        #   symbol: 交易对
        #   time_ms: 时间范围（毫秒），如 3600000 表示最近1小时
        #   返回: List[FundingRateData]，时间戳在指定范围内的记录数组
        fr_1hour = self.get_funding_rates_by_time(self.symbol, 3600000)  # 最近1小时
        
        # get_last_funding_rate: 获取最后一条记录
        #   symbol: 交易对
        #   返回: FundingRateData | None，如果无数据返回 None
        last_fr = self.get_last_funding_rate(self.symbol)
        
        # get_funding_rate_count: 获取记录数量
        #   symbol: 交易对
        #   返回: int，当前存储的记录数量
        fr_count = self.get_funding_rate_count(self.symbol)
        
        # ========== 账户信息查询 ==========
        
        # get_usdt_available: 获取USDT可用余额
        #   返回: float，USDT可用余额（单位：USDT）
        usdt = self.get_usdt_available()
        
        # get_total_equity: 获取总权益（USD）
        #   返回: float，账户总权益（单位：USD）
        equity = self.get_total_equity()
        
        # get_all_balances: 获取所有币种余额
        #   返回: List[BalanceInfo]，包含所有币种的余额信息
        balances = self.get_all_balances()
        
        # ========== 持仓信息查询 ==========
        
        # get_all_positions: 获取所有持仓（包括数量为0的持仓）
        #   返回: List[PositionInfo]，包含所有持仓的信息
        positions = self.get_all_positions()
        
        # get_active_positions: 获取有效持仓（数量不为0的持仓）
        #   返回: List[PositionInfo]，只包含有效持仓的信息
        active = self.get_active_positions()
        
        # get_position: 获取指定交易对的持仓
        #   symbol: 交易对
        #   pos_side: 持仓方向，"net"（默认）、"long"、"short"
        #   返回: PositionInfo | None，如果无持仓返回 None
        pos = self.get_position(self.symbol, "net")
        
        # ========== 订单信息查询 ==========
        
        # get_active_orders: 获取所有活跃订单（未成交的订单）
        #   返回: List[OrderInfo]，包含所有活跃订单的信息
        orders = self.get_active_orders()
        
        # pending_order_count: 获取活跃订单数量
        #   返回: int，当前活跃订单的数量
        pending = self.pending_order_count()
        
        # ========== 定时任务查询 ==========
        
        # get_scheduled_tasks: 获取所有已注册的定时任务
        #   返回: List[ScheduledTask]，包含所有任务的信息
        tasks = self.get_scheduled_tasks()
        
        # get_task_info: 获取指定任务的信息
        #   task_name: 任务名称
        #   返回: ScheduledTask | None，如果任务不存在返回 None
        task_info = self.get_task_info("check_position")


def main():
    print("=" * 60)
    print("  示例策略 - 策略基类 API 完整参考")
    print("=" * 60)
    
    strategy = ExampleStrategy("example", "BTC-USDT-SWAP")
    
    signal.signal(signal.SIGINT, lambda s, f: strategy.stop())
    signal.signal(signal.SIGTERM, lambda s, f: strategy.stop())
    
    strategy.run()


if __name__ == "__main__":
    main()
