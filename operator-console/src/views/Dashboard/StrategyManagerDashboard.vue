<template>
  <div class="strategy-manager-dashboard">
    <el-row :gutter="20">
      <el-col :span="24">
        <el-alert
          title="策略管理员模式"
          type="info"
          :closable="false"
          show-icon
        >
          您当前以策略管理员身份登录，可以查看和操作（启动/停止）被分配的策略
        </el-alert>
      </el-col>
    </el-row>

    <!-- 账号选择器（只显示与分配策略关联的账户） -->
    <el-row :gutter="20" style="margin-top: 20px;">
      <el-col :span="24">
        <el-card>
          <template #header>
            <span>选择要查看的账号</span>
          </template>
          <el-radio-group v-model="selectedAccountId" @change="handleAccountChange">
            <el-radio-button
              v-for="acc in relatedAccounts"
              :key="acc.id"
              :label="acc.id"
            >
              {{ acc.name }}
            </el-radio-button>
          </el-radio-group>
          <div v-if="relatedAccounts.length === 0" style="color: #909399; padding: 10px 0;">
            暂无关联账户
          </div>
        </el-card>
      </el-col>
    </el-row>

    <!-- 选中账号的统计 -->
    <el-row :gutter="20" class="stats-row" v-if="selectedAccount">
      <el-col :xs="24" :sm="12" :md="8">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">账户净值 (USDT)</span>
            <el-icon color="#409eff"><Wallet /></el-icon>
          </div>
          <div class="stat-value">{{ formatNumber(selectedAccount.equity || 0, 2) }}</div>
        </div>
      </el-col>

      <el-col :xs="24" :sm="12" :md="8">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">未实现盈亏 (USDT)</span>
            <el-icon color="#67c23a"><TrendCharts /></el-icon>
          </div>
          <div class="stat-value" :class="selectedAccount.unrealizedPnl >= 0 ? 'text-success' : 'text-danger'">
            {{ formatNumber(selectedAccount.unrealizedPnl || 0, 2) }}
          </div>
        </div>
      </el-col>

      <el-col :xs="24" :sm="12" :md="8">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">收益率</span>
            <el-icon color="#e6a23c"><DataLine /></el-icon>
          </div>
          <div class="stat-value" :class="selectedAccount.returnRate >= 0 ? 'text-success' : 'text-danger'">
            {{ formatPercent((selectedAccount.returnRate || 0) / 100) }}
          </div>
        </div>
      </el-col>
    </el-row>

    <!-- 当前持仓（表格） -->
    <el-row :gutter="20" class="charts-row" v-if="selectedAccountId">
      <el-col :span="24">
        <el-card>
          <template #header>
            <div class="card-header">
              <span>当前持仓</span>
              <el-button :icon="Refresh" size="small" circle @click="loadPositions" :loading="positionsLoading" />
            </div>
          </template>
          <el-table :data="positions" v-loading="positionsLoading" size="small" stripe>
            <el-table-column label="品种" prop="symbol" min-width="140">
              <template #default="{ row }">
                {{ row.symbol || row.instId || '--' }}
              </template>
            </el-table-column>
            <el-table-column label="方向" width="80">
              <template #default="{ row }">
                <span v-if="getPositionSide(row)" :style="{ color: getPositionSide(row) === 'LONG' ? '#67c23a' : '#f56c6c', fontWeight: 600 }">
                  {{ getPositionSide(row) }}
                </span>
                <span v-else>--</span>
              </template>
            </el-table-column>
            <el-table-column label="数量" width="120">
              <template #default="{ row }">
                {{ row.positionAmt || row.pos || '--' }}
              </template>
            </el-table-column>
            <el-table-column label="开仓均价" width="130" align="right">
              <template #default="{ row }">
                {{ formatPrice(row.entryPrice || row.avgPx) }}
              </template>
            </el-table-column>
            <el-table-column label="标记价格" width="130" align="right">
              <template #default="{ row }">
                {{ formatPrice(row.markPrice || row.markPx) }}
              </template>
            </el-table-column>
            <el-table-column label="名义价值 (USDT)" width="150" align="right">
              <template #default="{ row }">
                {{ formatNumber(Math.abs(parseFloat(row.notional || row.notionalUsd || 0)), 2) }}
              </template>
            </el-table-column>
            <el-table-column label="未实现盈亏" width="140" align="right">
              <template #default="{ row }">
                <span :class="parseFloat(row.unRealizedProfit || row.upl || 0) >= 0 ? 'text-success' : 'text-danger'">
                  {{ formatNumber(parseFloat(row.unRealizedProfit || row.upl || 0), 2) }}
                </span>
              </template>
            </el-table-column>
            <el-table-column label="杠杆" width="80" align="center">
              <template #default="{ row }">
                {{ row.leverage || row.lever || '--' }}x
              </template>
            </el-table-column>
          </el-table>
          <div v-if="positions.length === 0 && !positionsLoading" style="padding: 20px; text-align: center; color: #909399;">
            暂无持仓
          </div>
        </el-card>
      </el-col>
    </el-row>

    <!-- 最近订单（不依赖账户选择，始终显示） -->
    <el-row :gutter="20" class="charts-row">
      <el-col :span="24">
        <el-card>
          <template #header>
            <div class="card-header">
              <span>最近订单（只读）</span>
              <el-button :icon="Refresh" circle size="small" @click="loadRecentOrders" :loading="ordersLoading" />
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
                <el-tag size="small" type="info" disable-transitions>
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
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import { useStrategyStore } from '@/stores/strategy'
import { useAccountStore } from '@/stores/account'
import { formatNumber, formatPercent } from '@/utils/format'
import { strategyApi } from '@/api/strategy'
import { Wallet, TrendCharts, DataLine, Refresh } from '@element-plus/icons-vue'

const strategyStore = useStrategyStore()
const accountStore = useAccountStore()

const selectedAccountId = ref(null)
const positions = ref([])
const positionsLoading = ref(false)
const recentOrders = ref([])
const ordersLoading = ref(false)

// 从策略列表中提取关联的 account_id（去重）
const relatedAccounts = computed(() => {
  const strategies = strategyStore.strategies || []
  const accountMap = new Map()
  for (const s of strategies) {
    const accId = s.account_id || s.account || ''
    if (accId && !accountMap.has(accId)) {
      // 尝试从 accountStore 找到对应的账户信息
      const storeAcc = (accountStore.accounts || []).find(a => a.id === accId)
      accountMap.set(accId, {
        id: accId,
        name: accId,
        equity: storeAcc?.equity || 0,
        unrealizedPnl: storeAcc?.unrealizedPnl || 0,
        returnRate: storeAcc?.returnRate || 0
      })
    }
  }
  return Array.from(accountMap.values())
})

const selectedAccount = computed(() =>
  relatedAccounts.value.find(a => a.id === selectedAccountId.value)
)

// 初始化选中第一个账号
watch(relatedAccounts, (newAccounts) => {
  if (newAccounts.length > 0 && !selectedAccountId.value) {
    selectedAccountId.value = newAccounts[0].id
  }
}, { immediate: true })

function formatPrice(price) {
  if (!price) return '--'
  const p = parseFloat(price)
  if (isNaN(p)) return '--'
  if (p >= 1000) return p.toFixed(2)
  if (p >= 1) return p.toFixed(4)
  return p.toFixed(6)
}

function getPositionSide(row) {
  if (row.positionSide && row.positionSide !== 'BOTH') return row.positionSide
  if (row.posSide) return row.posSide === 'long' ? 'LONG' : row.posSide === 'short' ? 'SHORT' : ''
  const amt = parseFloat(row.positionAmt || row.pos || 0)
  if (amt > 0) return 'LONG'
  if (amt < 0) return 'SHORT'
  return ''
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

async function loadPositions() {
  if (!selectedAccountId.value) return
  positionsLoading.value = true
  try {
    const res = await strategyApi.getAccountPositions(selectedAccountId.value)
    positions.value = res.data || []
  } finally {
    positionsLoading.value = false
  }
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

function handleAccountChange() {
  positions.value = []
  loadPositions()
}

// 选中账户变化时加载持仓
watch(selectedAccountId, (newVal) => {
  if (newVal) {
    loadPositions()
  }
})

let refreshTimer = null

onMounted(() => {
  // 立即加载订单（不依赖账户选择）
  loadRecentOrders()
  // 定时刷新持仓（每10秒）和订单（每30秒）
  let tickCount = 0
  refreshTimer = setInterval(() => {
    tickCount++
    if (selectedAccountId.value) loadPositions()
    if (tickCount % 3 === 0) loadRecentOrders() // 每30秒刷新订单
  }, 10000)
})

onUnmounted(() => {
  if (refreshTimer) clearInterval(refreshTimer)
})
</script>

<style lang="scss" scoped>
.strategy-manager-dashboard {
  .el-alert {
    margin-bottom: 24px;
    background: rgba(59, 130, 246, 0.06) !important;
    border: 1px solid rgba(59, 130, 246, 0.15) !important;
    color: var(--text-primary) !important;
    border-radius: var(--radius) !important;
    backdrop-filter: blur(8px);

    :deep(.el-alert__title) { color: var(--accent-blue) !important; font-weight: 600; }
    :deep(.el-alert__description) { color: var(--text-secondary) !important; }
    :deep(.el-alert__icon) { color: var(--accent-blue) !important; }
  }

  .stats-row { margin: 24px 0; }

  .stat-card {
    background: var(--bg-card);
    border-radius: var(--radius);
    padding: 24px;
    border: 1px solid var(--border-color);
    transition: all 0.4s cubic-bezier(0.22, 1, 0.36, 1);
    position: relative;
    overflow: hidden;

    &::before {
      content: '';
      position: absolute;
      top: 0; left: 0; right: 0;
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
      .stat-label { font-size: 11px; color: var(--text-muted); text-transform: uppercase; letter-spacing: 1px; font-weight: 600; }
      .el-icon { opacity: 0.5; font-size: 20px; }
    }

    .stat-value {
      font-size: 28px;
      font-weight: 800;
      font-family: var(--font-mono);
      color: var(--text-primary);
      letter-spacing: -1px;
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
      &:hover { color: var(--accent-green) !important; border-color: var(--accent-green) !important; }
    }
  }

  :deep(.el-radio-button__inner) {
    background: var(--bg-elevated) !important;
    border-color: var(--border-color) !important;
    color: var(--text-secondary) !important;
    border-radius: var(--radius-sm) !important;
    font-weight: 500;
  }
  :deep(.el-radio-button__original-radio:checked + .el-radio-button__inner) {
    background: rgba(52, 211, 153, 0.12) !important;
    border-color: var(--accent-green) !important;
    color: var(--accent-green) !important;
    box-shadow: -1px 0 0 0 var(--accent-green) !important;
  }

  :deep(.order-row-error) {
    background-color: rgba(239, 68, 68, 0.05) !important;
  }
}
</style>
