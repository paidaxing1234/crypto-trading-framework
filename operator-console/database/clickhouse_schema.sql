-- ============================================
-- ClickHouse 数据库设计
-- 实盘交易管理系统
-- ============================================

-- 创建数据库
CREATE DATABASE IF NOT EXISTS trading_system;

USE trading_system;

-- ============================================
-- 1. 用户表
-- ============================================
CREATE TABLE IF NOT EXISTS users (
    id UInt64,
    username String,
    name String,
    password_hash String,
    email String,
    role Enum8('super_admin' = 1, 'viewer' = 2),
    status Enum8('active' = 1, 'inactive' = 2),
    last_login DateTime,
    created_at DateTime,
    updated_at DateTime
) ENGINE = ReplacingMergeTree(updated_at)
ORDER BY id
SETTINGS index_granularity = 8192;

-- ============================================
-- 2. 策略表
-- ============================================
CREATE TABLE IF NOT EXISTS strategies (
    id UInt64,
    name String,
    type String,
    account_id UInt64,
    account_name String,
    symbol String,
    python_file String,
    parameters String,  -- JSON格式参数
    status Enum8('running' = 1, 'stopped' = 2, 'pending' = 3, 'error' = 4),
    description String,
    created_by UInt64,
    created_at DateTime,
    updated_at DateTime
) ENGINE = ReplacingMergeTree(updated_at)
ORDER BY (id, created_at)
SETTINGS index_granularity = 8192;

-- ============================================
-- 3. 账户表
-- ============================================
CREATE TABLE IF NOT EXISTS accounts (
    id UInt64,
    name String,
    exchange String DEFAULT 'OKX',
    api_key String,
    account_type Enum8('unified' = 1, 'single' = 2, 'multi' = 3),
    is_demo UInt8,
    status Enum8('active' = 1, 'inactive' = 2),
    created_by UInt64,
    created_at DateTime,
    updated_at DateTime
) ENGINE = ReplacingMergeTree(updated_at)
ORDER BY id
SETTINGS index_granularity = 8192;

-- ============================================
-- 4. 账户快照表（时序数据）
-- ============================================
CREATE TABLE IF NOT EXISTS account_snapshots (
    timestamp DateTime,
    account_id UInt64,
    account_name String,
    balance Decimal(18, 8),
    available_balance Decimal(18, 8),
    frozen_balance Decimal(18, 8),
    equity Decimal(18, 8),
    unrealized_pnl Decimal(18, 8),
    realized_pnl Decimal(18, 8),
    margin_ratio Decimal(10, 4),
    date Date DEFAULT toDate(timestamp)
) ENGINE = MergeTree()
PARTITION BY toYYYYMM(date)
ORDER BY (account_id, timestamp)
SETTINGS index_granularity = 8192;

-- ============================================
-- 5. 订单表
-- ============================================
CREATE TABLE IF NOT EXISTS orders (
    id UInt64,
    exchange_order_id String,
    account_id UInt64,
    strategy_id UInt64,
    symbol String,
    side Enum8('BUY' = 1, 'SELL' = 2),
    type Enum8('LIMIT' = 1, 'MARKET' = 2, 'STOP' = 3),
    price Decimal(18, 8),
    quantity Decimal(18, 8),
    filled_quantity Decimal(18, 8),
    filled_price Decimal(18, 8),
    state Enum8('CREATED' = 1, 'SUBMITTED' = 2, 'ACCEPTED' = 3, 
                'PARTIALLY_FILLED' = 4, 'FILLED' = 5, 
                'CANCELLED' = 6, 'REJECTED' = 7),
    fee Decimal(18, 8),
    note String,
    created_at DateTime,
    updated_at DateTime,
    date Date DEFAULT toDate(created_at)
) ENGINE = ReplacingMergeTree(updated_at)
PARTITION BY toYYYYMM(date)
ORDER BY (symbol, created_at, id)
SETTINGS index_granularity = 8192;

-- ============================================
-- 6. 成交记录表
-- ============================================
CREATE TABLE IF NOT EXISTS trades (
    id UInt64,
    trade_id String,
    order_id UInt64,
    exchange_order_id String,
    account_id UInt64,
    strategy_id UInt64,
    symbol String,
    side Enum8('BUY' = 1, 'SELL' = 2),
    price Decimal(18, 8),
    quantity Decimal(18, 8),
    fee Decimal(18, 8),
    fee_currency String,
    timestamp DateTime,
    date Date DEFAULT toDate(timestamp)
) ENGINE = MergeTree()
PARTITION BY toYYYYMM(date)
ORDER BY (symbol, timestamp, id)
SETTINGS index_granularity = 8192;

-- ============================================
-- 7. 持仓表
-- ============================================
CREATE TABLE IF NOT EXISTS positions (
    timestamp DateTime,
    account_id UInt64,
    symbol String,
    side Enum8('long' = 1, 'short' = 2),
    quantity Decimal(18, 8),
    avg_price Decimal(18, 8),
    current_price Decimal(18, 8),
    notional_value Decimal(18, 8),
    unrealized_pnl Decimal(18, 8),
    realized_pnl Decimal(18, 8),
    leverage UInt8,
    liquidation_price Decimal(18, 8),
    date Date DEFAULT toDate(timestamp)
) ENGINE = ReplacingMergeTree(timestamp)
PARTITION BY toYYYYMM(date)
ORDER BY (account_id, symbol, timestamp)
SETTINGS index_granularity = 8192;

-- ============================================
-- 8. 策略性能表（时序数据）
-- ============================================
CREATE TABLE IF NOT EXISTS strategy_performance (
    timestamp DateTime,
    strategy_id UInt64,
    strategy_name String,
    pnl Decimal(18, 8),
    return_rate Decimal(10, 4),
    trades_count UInt32,
    win_count UInt32,
    loss_count UInt32,
    win_rate Decimal(10, 4),
    max_drawdown Decimal(10, 4),
    sharpe_ratio Decimal(10, 4),
    date Date DEFAULT toDate(timestamp)
) ENGINE = MergeTree()
PARTITION BY toYYYYMM(date)
ORDER BY (strategy_id, timestamp)
SETTINGS index_granularity = 8192;

-- ============================================
-- 9. 系统日志表
-- ============================================
CREATE TABLE IF NOT EXISTS system_logs (
    timestamp DateTime,
    level Enum8('DEBUG' = 1, 'INFO' = 2, 'WARNING' = 3, 'ERROR' = 4),
    user_id UInt64,
    action String,
    resource_type String,
    resource_id UInt64,
    ip_address String,
    user_agent String,
    message String,
    details String,
    date Date DEFAULT toDate(timestamp)
) ENGINE = MergeTree()
PARTITION BY toYYYYMM(date)
ORDER BY (timestamp, level)
TTL date + INTERVAL 90 DAY  -- 90天后自动删除
SETTINGS index_granularity = 8192;

-- ============================================
-- 物化视图：实时统计
-- ============================================

-- 账户实时汇总
CREATE MATERIALIZED VIEW IF NOT EXISTS account_summary_mv
ENGINE = AggregatingMergeTree()
PARTITION BY toYYYYMM(date)
ORDER BY (account_id, date)
AS SELECT
    account_id,
    toDate(timestamp) as date,
    argMax(equity, timestamp) as latest_equity,
    sum(unrealized_pnl) as total_unrealized_pnl,
    sum(realized_pnl) as total_realized_pnl,
    count() as snapshot_count
FROM account_snapshots
GROUP BY account_id, date;

-- 策略每日统计
CREATE MATERIALIZED VIEW IF NOT EXISTS strategy_daily_summary_mv
ENGINE = SummingMergeTree()
PARTITION BY toYYYYMM(date)
ORDER BY (strategy_id, date)
AS SELECT
    strategy_id,
    toDate(timestamp) as date,
    sum(pnl) as total_pnl,
    avg(return_rate) as avg_return_rate,
    sum(trades_count) as total_trades,
    sum(win_count) as total_wins,
    sum(loss_count) as total_losses
FROM strategy_performance
GROUP BY strategy_id, date;

-- 订单统计
CREATE MATERIALIZED VIEW IF NOT EXISTS order_statistics_mv
ENGINE = SummingMergeTree()
PARTITION BY toYYYYMM(date)
ORDER BY (account_id, symbol, date)
AS SELECT
    account_id,
    symbol,
    toDate(created_at) as date,
    countIf(state = 'FILLED') as filled_count,
    countIf(state = 'CANCELLED') as cancelled_count,
    countIf(state = 'REJECTED') as rejected_count,
    sum(filled_quantity * filled_price) as total_volume,
    sum(fee) as total_fee
FROM orders
WHERE state IN ('FILLED', 'CANCELLED', 'REJECTED')
GROUP BY account_id, symbol, date;

-- ============================================
-- 常用查询示例
-- ============================================

-- 查询账户净值曲线（最近30天）
-- SELECT 
--     timestamp,
--     equity,
--     unrealized_pnl,
--     realized_pnl
-- FROM account_snapshots
-- WHERE account_id = 1
--   AND timestamp >= now() - INTERVAL 30 DAY
-- ORDER BY timestamp;

-- 查询策略性能排行
-- SELECT 
--     strategy_id,
--     strategy_name,
--     sum(pnl) as total_pnl,
--     avg(return_rate) as avg_return_rate,
--     sum(trades_count) as total_trades
-- FROM strategy_performance
-- WHERE date >= today() - 30
-- GROUP BY strategy_id, strategy_name
-- ORDER BY total_pnl DESC
-- LIMIT 10;

-- 查询订单成交统计（按交易对）
-- SELECT 
--     symbol,
--     count() as order_count,
--     sum(filled_quantity) as total_quantity,
--     sum(filled_quantity * filled_price) as total_volume,
--     sum(fee) as total_fee
-- FROM orders
-- WHERE state = 'FILLED'
--   AND date >= today() - 7
-- GROUP BY symbol
-- ORDER BY total_volume DESC;

-- 查询实时持仓
-- SELECT 
--     account_id,
--     symbol,
--     side,
--     quantity,
--     avg_price,
--     current_price,
--     unrealized_pnl,
--     leverage
-- FROM positions
-- WHERE timestamp = (SELECT max(timestamp) FROM positions);

