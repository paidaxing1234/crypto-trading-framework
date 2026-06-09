/**
 * 模拟数据服务
 * 用于在后端未完成时进行前端开发和测试
 */

import { strategyStorage, accountStorage, orderStorage, positionStorage } from '@/utils/storage'

// 初始化策略数据
const initialStrategies = [
  {
    id: 1,
    name: '网格交易策略A',
    type: 'grid',
    account: 'OKX账户1',
    status: 'running',
    pnl: 1250.50,
    returnRate: 12.5,
    trades: 145,
    runTime: '3天12小时'
  },
  {
    id: 2,
    name: '趋势跟踪策略B',
    type: 'trend',
    account: 'OKX账户2',
    status: 'running',
    pnl: 680.30,
    returnRate: 6.8,
    trades: 89,
    runTime: '2天8小时'
  },
  {
    id: 3,
    name: '套利策略C',
    type: 'arbitrage',
    account: 'OKX账户1',
    status: 'stopped',
    pnl: -120.40,
    returnRate: -1.2,
    trades: 56,
    runTime: '1天6小时'
  },
  {
    id: 4,
    name: '做市策略D',
    type: 'market_making',
    account: 'OKX账户3',
    status: 'pending',
    pnl: 0,
    returnRate: 0,
    trades: 0,
    runTime: '-'
  },
  {
    id: 5,
    name: '均值回归策略E',
    type: 'custom',
    account: 'OKX账户2',
    status: 'error',
    pnl: -50.20,
    returnRate: -0.5,
    trades: 12,
    runTime: '5小时'
  }
]

// 从本地存储加载或使用初始数据
export let mockStrategies = strategyStorage.load(initialStrategies)

// 初始化账户数据
const initialAccounts = [
  {
    id: 1,
    name: 'OKX账户1',
    apiKey: 'abc123def456ghi789jkl012mno345pqr',
    balance: 15000,
    availableBalance: 12000,
    frozenBalance: 3000,
    equity: 16250,
    unrealizedPnl: 1250,
    realizedPnl: 500,
    returnRate: 8.33,
    status: 'active',
    accountType: 'unified',
    lastSyncTime: new Date(Date.now() - 5 * 60 * 1000),
    createdAt: new Date('2024-01-01')
  },
  {
    id: 2,
    name: 'OKX账户2',
    apiKey: 'xyz789abc123def456ghi789jkl012mno',
    balance: 8000,
    availableBalance: 7200,
    frozenBalance: 800,
    equity: 8680,
    unrealizedPnl: 680,
    realizedPnl: 320,
    returnRate: 12.5,
    status: 'active',
    accountType: 'unified',
    lastSyncTime: new Date(Date.now() - 10 * 60 * 1000),
    createdAt: new Date('2024-01-15')
  },
  {
    id: 3,
    name: 'OKX账户3',
    apiKey: 'pqr345stu678vwx901yza234bcd567efg',
    balance: 5000,
    availableBalance: 5000,
    frozenBalance: 0,
    equity: 5000,
    unrealizedPnl: 0,
    realizedPnl: 0,
    returnRate: 0,
    status: 'inactive',
    accountType: 'unified',
    lastSyncTime: new Date(Date.now() - 60 * 60 * 1000),
    createdAt: new Date('2024-02-01')
  }
]

// 从本地存储加载或使用初始数据
export let mockAccounts = accountStorage.load(initialAccounts)

// 初始化订单数据
const initialOrders = [
  {
    id: 1001,
    exchangeOrderId: '123456789',
    symbol: 'BTC-USDT-SWAP',
    side: 'BUY',
    type: 'LIMIT',
    price: 42500,
    quantity: 0.1,
    filledQuantity: 0.1,
    filledPrice: 42500,
    state: 'FILLED',
    fee: 4.25,
    timestamp: new Date(Date.now() - 2 * 60 * 60 * 1000),
    updateTime: new Date(Date.now() - 2 * 60 * 60 * 1000),
    trades: [
      {
        tradeId: 'T1001',
        price: 42500,
        quantity: 0.1,
        fee: 4.25,
        timestamp: new Date(Date.now() - 2 * 60 * 60 * 1000)
      }
    ]
  },
  {
    id: 1002,
    exchangeOrderId: '123456790',
    symbol: 'ETH-USDT-SWAP',
    side: 'SELL',
    type: 'LIMIT',
    price: 2250,
    quantity: 1.0,
    filledQuantity: 0.5,
    filledPrice: 2250,
    state: 'PARTIALLY_FILLED',
    fee: 1.125,
    timestamp: new Date(Date.now() - 1 * 60 * 60 * 1000),
    updateTime: new Date(Date.now() - 30 * 60 * 1000),
    trades: [
      {
        tradeId: 'T1002',
        price: 2250,
        quantity: 0.5,
        fee: 1.125,
        timestamp: new Date(Date.now() - 30 * 60 * 1000)
      }
    ]
  },
  {
    id: 1003,
    exchangeOrderId: '123456791',
    symbol: 'SOL-USDT-SWAP',
    side: 'BUY',
    type: 'MARKET',
    price: null,
    quantity: 10,
    filledQuantity: 10,
    filledPrice: 98.5,
    state: 'FILLED',
    fee: 0.985,
    timestamp: new Date(Date.now() - 30 * 60 * 1000),
    updateTime: new Date(Date.now() - 30 * 60 * 1000)
  }
]

// 从本地存储加载或使用初始数据
export let mockOrders = orderStorage.load(initialOrders)

// 初始化持仓数据
const initialPositions = [
  {
    id: 1,
    symbol: 'BTC-USDT-SWAP',
    side: 'long',
    quantity: 0.5,
    avgPrice: 42000,
    currentPrice: 42500,
    notionalValue: 21250,
    unrealizedPnl: 250,
    returnRate: 1.19,
    leverage: 5,
    liquidationPrice: 38000
  },
  {
    id: 2,
    symbol: 'ETH-USDT-SWAP',
    side: 'long',
    quantity: 5,
    avgPrice: 2200,
    currentPrice: 2250,
    notionalValue: 11250,
    unrealizedPnl: 250,
    returnRate: 2.27,
    leverage: 3,
    liquidationPrice: 1900
  },
  {
    id: 3,
    symbol: 'SOL-USDT-SWAP',
    side: 'short',
    quantity: 50,
    avgPrice: 100,
    currentPrice: 98,
    notionalValue: 4900,
    unrealizedPnl: 100,
    returnRate: 2.0,
    leverage: 2,
    liquidationPrice: 115
  }
]

// 从本地存储加载或使用初始数据
export let mockPositions = positionStorage.load(initialPositions)

// 保存数据到本地存储
export function saveMockData() {
  strategyStorage.save(mockStrategies)
  accountStorage.save(mockAccounts)
  orderStorage.save(mockOrders)
  positionStorage.save(mockPositions)
}

// 模拟API响应包装
export function mockApiResponse(data, delay = 500) {
  return new Promise(resolve => {
    setTimeout(() => {
      resolve({
        code: 200,
        message: 'success',
        data: data
      })
    }, delay)
  })
}

