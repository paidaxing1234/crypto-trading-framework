<template>
  <div class="super-admin-dashboard">
    <el-row :gutter="20">
      <el-col :span="24">
        <h3>超级管理员视图 - 全局概览</h3>
      </el-col>
    </el-row>

    <!-- 实时行情 -->
    <el-card class="market-card">
      <template #header>
        <div class="card-header">
          <span>实时行情</span>
          <el-tag v-if="marketConnected" type="success" size="small">已连接</el-tag>
          <el-tag v-else type="danger" size="small">未连接</el-tag>
        </div>
      </template>

      <!-- OKX 行情 -->
      <div class="exchange-row">
        <div class="exchange-label">OKX</div>
        <div class="price-row">
          <div v-for="coin in displayCoins" :key="'okx-' + coin" class="price-item">
            <div class="symbol">{{ coin }}-USDT</div>
            <div class="price" :class="priceDirection[coin + '-USDT']">
              {{ formatPrice(marketPrices[coin + '-USDT']) }}
            </div>
          </div>
        </div>
      </div>

      <!-- Binance 行情 -->
      <div class="exchange-row">
        <div class="exchange-label">Binance</div>
        <div class="price-row">
          <div v-for="coin in displayCoins" :key="'binance-' + coin" class="price-item">
            <div class="symbol">{{ coin }}USDT</div>
            <div class="price" :class="priceDirection[coin + 'USDT']">
              {{ formatPrice(marketPrices[coin + 'USDT']) }}
            </div>
          </div>
        </div>
      </div>
    </el-card>

    <!-- 全局统计 -->
    <el-row :gutter="20" class="stats-row">
      <el-col :xs="24" :sm="12" :md="6">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">总资产 (USDT)</span>
            <el-icon color="#409eff"><Wallet /></el-icon>
          </div>
          <div class="stat-value">{{ formatNumber(totalEquity, 2) }}</div>
          <div class="stat-change" :class="equityChange >= 0 ? 'text-success' : 'text-danger'">
            <el-icon>
              <component :is="equityChange >= 0 ? 'TopRight' : 'BottomRight'" />
            </el-icon>
            {{ formatPercent(equityChange / 100) }}
          </div>
        </div>
      </el-col>
      
      <el-col :xs="24" :sm="12" :md="6">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">总未实现盈亏 (USDT)</span>
            <el-icon color="#67c23a"><TrendCharts /></el-icon>
          </div>
          <div class="stat-value" :class="totalPnL >= 0 ? 'text-success' : 'text-danger'">
            {{ formatNumber(totalPnL, 2) }}
          </div>
        </div>
      </el-col>
      
      <el-col :xs="24" :sm="12" :md="6">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">运行策略</span>
            <el-icon color="#e6a23c"><SetUp /></el-icon>
          </div>
          <div class="stat-value">{{ runningStrategies }}</div>
          <div class="stat-change">
            总策略: {{ totalStrategies }}
          </div>
        </div>
      </el-col>
      
      <el-col :xs="24" :sm="12" :md="6">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">活跃账户</span>
            <el-icon color="#f56c6c"><User /></el-icon>
          </div>
          <div class="stat-value">{{ activeAccounts }}</div>
          <div class="stat-change">
            总账户: {{ totalAccounts }}
          </div>
        </div>
      </el-col>
    </el-row>
    

    <!-- 最近订单 -->
    <el-row :gutter="20" class="charts-row">
      <el-col :span="24">
        <el-card>
          <template #header>
            <div class="card-header">
              <span>最近订单</span>
              <div style="display: flex; align-items: center; gap: 10px;">
                <el-button :icon="Refresh" circle size="small" @click="loadRecentOrders" :loading="ordersLoading" />
              </div>
            </div>
          </template>
          <el-table
            :data="recentOrders"
            v-loading="ordersLoading"
            size="small"
            max-height="400"
            :row-class-name="orderRowClass"
            style="width: 100%"
          >
            <el-table-column label="时间" prop="timestamp" width="180" />
            <el-table-column label="策略" prop="strategy" min-width="200">
              <template #default="{ row }">
                <el-tag size="small" :type="getStrategyTagType(row.strategy)" disable-transitions>
                  {{ row.strategy }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="品种" prop="symbol" width="140" />
            <el-table-column label="方向" prop="side" width="80">
              <template #default="{ row }">
                <span v-if="row.side" :style="{ color: row.side.toLowerCase() === 'buy' ? '#67c23a' : '#f56c6c', fontWeight: 600 }">
                  {{ row.side.toUpperCase() }}
                </span>
              </template>
            </el-table-column>
            <el-table-column label="数量" prop="quantity" width="120" />
            <el-table-column label="状态" prop="status" width="150">
              <template #default="{ row }">
                <el-tag
                  size="small"
                  :type="getStatusTagType(row.status)"
                  disable-transitions
                >{{ row.status }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="详情" prop="detail" min-width="250" show-overflow-tooltip />
          </el-table>
          <div v-if="recentOrders.length === 0 && !ordersLoading" style="padding: 30px; text-align: center; color: #909399;">
            今日暂无订单记录
          </div>
        </el-card>
      </el-col>
    </el-row>
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted, onUnmounted } from 'vue'
import { useAccountStore } from '@/stores/account'
import { useStrategyStore } from '@/stores/strategy'
import { useOrderStore } from '@/stores/order'
import { formatNumber, formatPercent } from '@/utils/format'
import { Wallet, TrendCharts, SetUp, User, Refresh } from '@element-plus/icons-vue'
import { wsClient } from '@/services/WebSocketClient'
import { strategyApi } from '@/api/strategy'

const accountStore = useAccountStore()
const strategyStore = useStrategyStore()
const orderStore = useOrderStore()

// 实时行情
const marketPrices = reactive({})
const priceDirection = reactive({})
const marketConnected = ref(false)
const displayCoins = ['BTC', 'ETH', 'SOL', 'XRP', 'DOGE']

function formatPrice(price) {
  if (!price) return '--'
  if (price >= 1000) return price.toFixed(2)
  if (price >= 1) return price.toFixed(4)
  return price.toFixed(6)
}

function handleTrade(event) {
  const { data } = event
  if (data && data.symbol && data.price) {
    const oldPrice = marketPrices[data.symbol]
    const newPrice = parseFloat(data.price)
    if (oldPrice !== undefined) {
      priceDirection[data.symbol] = newPrice > oldPrice ? 'up' : newPrice < oldPrice ? 'down' : ''
    }
    marketPrices[data.symbol] = newPrice
    marketConnected.value = true
  }
}

function handleSnapshot(event) {
  const { data } = event
  if (data && data.trades) {
    for (const trade of data.trades) {
      if (trade.symbol && trade.price) {
        marketPrices[trade.symbol] = parseFloat(trade.price)
      }
    }
    marketConnected.value = true
  }
}


// 统计数据
const totalEquity = computed(() => accountStore.totalEquity || 0)
const equityChange = computed(() => 5.2) // Mock数据
const totalPnL = computed(() => accountStore.totalPnL || 0)
const runningStrategies = computed(() => strategyStore.runningStrategies?.length || 0)
const totalStrategies = computed(() => strategyStore.strategies?.length || 0)
const activeAccounts = computed(() => accountStore.activeAccounts?.length || 0)
const totalAccounts = computed(() => accountStore.accounts?.length || 0)

// 最近订单
const recentOrders = ref([])
const ordersLoading = ref(false)

// 策略颜色映射
const strategyColors = {}
const colorTypes = ['', 'success', 'warning', 'danger', 'info']
let colorIdx = 0

function getStrategyTagType(strategy) {
  if (!strategyColors[strategy]) {
    strategyColors[strategy] = colorTypes[colorIdx % colorTypes.length]
    colorIdx++
  }
  return strategyColors[strategy]
}

function getStatusTagType(status) {
  if (!status) return 'info'
  if (status === 'ACCEPTED' || status === 'FILLED') return 'success'
  if (status === 'REJECTED' || status === 'RISK_REJECTED') return 'danger'
  if (status === 'RECEIVED' || status === 'BATCH_RECEIVED') return ''
  return 'info'
}

function orderRowClass({ row }) {
  if (row.status === 'REJECTED' || row.status === 'RISK_REJECTED') return 'order-row-error'
  return ''
}

async function loadRecentOrders() {
  ordersLoading.value = true
  try {
    const res = await strategyApi.getRecentOrders(100)
    recentOrders.value = res.data || []
  } finally {
    ordersLoading.value = false
  }
}

let orderRefreshTimer = null

onMounted(() => {
  wsClient.on('trade', handleTrade)
  wsClient.on('ticker', handleTrade)
  wsClient.on('snapshot', handleSnapshot)
  marketConnected.value = wsClient.connected
  loadRecentOrders()
  orderRefreshTimer = setInterval(loadRecentOrders, 30000)
})

onUnmounted(() => {
  wsClient.off('trade', handleTrade)
  wsClient.off('ticker', handleTrade)
  wsClient.off('snapshot', handleSnapshot)
  if (orderRefreshTimer) clearInterval(orderRefreshTimer)
})
</script>

<style lang="scss" scoped>
.super-admin-dashboard {
  h3 {
    margin: 0 0 24px 0;
    color: var(--text-primary);
    font-weight: 800;
    font-size: 20px;
    letter-spacing: -0.5px;
    animation: fadeInUp 0.4s cubic-bezier(0.22, 1, 0.36, 1);
  }

  .market-card {
    margin-bottom: 24px;
    overflow: hidden;

    .card-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      color: var(--text-primary);
      font-weight: 600;
    }

    .exchange-row {
      display: flex;
      align-items: center;
      margin-bottom: 16px;
      &:last-of-type { margin-bottom: 0; }

      .exchange-label {
        width: 80px;
        font-weight: 700;
        flex-shrink: 0;
        color: var(--text-muted);
        font-family: var(--font-mono);
        font-size: 11px;
        text-transform: uppercase;
        letter-spacing: 1.5px;
      }

      .price-row {
        display: flex;
        flex: 1;
        gap: 10px;
      }
    }

    .price-item {
      flex: 1;
      text-align: center;
      padding: 14px 10px;
      background: var(--bg-elevated);
      border-radius: var(--radius-sm);
      border: 1px solid var(--border-color);
      transition: all 0.3s ease;

      &:hover {
        border-color: var(--border-glow);
        box-shadow: var(--shadow-glow);
        transform: translateY(-2px);
      }

      .symbol {
        font-size: 10px;
        color: var(--text-muted);
        margin-bottom: 8px;
        font-family: var(--font-mono);
        letter-spacing: 1px;
        text-transform: uppercase;
      }

      .price {
        font-size: 18px;
        font-weight: 700;
        font-family: var(--font-mono);
        color: var(--text-primary);
        transition: color 0.3s;
        letter-spacing: -0.5px;

        &.up { color: var(--accent-green); animation: price-flash-up 0.6s; }
        &.down { color: var(--accent-red); animation: price-flash-down 0.6s; }
      }
    }
  }

  .stats-row {
    margin-bottom: 24px;
  }

  .stat-card {
    background: var(--bg-card);
    border-radius: var(--radius);
    padding: 24px;
    border: 1px solid var(--border-color);
    transition: all 0.4s cubic-bezier(0.22, 1, 0.36, 1);
    animation: fadeInUp 0.5s cubic-bezier(0.22, 1, 0.36, 1) both;
    position: relative;
    overflow: hidden;

    &::before {
      content: '';
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 3px;
      background: linear-gradient(90deg, var(--accent-green), var(--accent-cyan));
      opacity: 0;
      transition: opacity 0.4s;
    }

    &:hover {
      border-color: var(--border-glow);
      box-shadow: var(--shadow-glow), var(--shadow-md);
      transform: translateY(-4px);
      &::before { opacity: 1; }
    }

    .stat-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 16px;

      .stat-label {
        font-size: 11px;
        color: var(--text-muted);
        text-transform: uppercase;
        letter-spacing: 1px;
        font-weight: 600;
      }
      .el-icon { opacity: 0.5; font-size: 20px; }
    }

    .stat-value {
      font-size: 32px;
      font-weight: 800;
      font-family: var(--font-mono);
      color: var(--text-primary);
      margin-bottom: 10px;
      letter-spacing: -1px;
      line-height: 1;
    }

    .stat-change {
      font-size: 12px;
      font-family: var(--font-mono);
      display: flex;
      align-items: center;
      gap: 4px;
      color: var(--text-muted);
      font-weight: 500;

      &.text-success { color: var(--accent-green); }
      &.text-danger { color: var(--accent-red); }
    }
  }

  .charts-row { margin-bottom: 24px; }

  .card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--text-primary);
    font-weight: 600;

    .el-button {
      color: var(--text-secondary) !important;
      border-color: var(--border-color) !important;
      background: transparent !important;
      &:hover {
        color: var(--accent-green) !important;
        border-color: var(--accent-green) !important;
      }
    }
  }

  :deep(.order-row-error) {
    background-color: rgba(239, 68, 68, 0.05) !important;
  }
}
</style>

