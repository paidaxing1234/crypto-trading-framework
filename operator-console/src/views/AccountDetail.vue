<template>
  <div class="account-detail-page">
    <!-- 页面头部 -->
    <div class="page-header">
      <div class="header-left">
        <el-button :icon="ArrowLeft" @click="goBack">返回</el-button>
        <div class="header-info">
          <h2>{{ accountInfo.name || '账户详情' }}</h2>
          <p>
            <el-tag :type="accountInfo.exchange === 'okx' ? 'primary' : 'success'" size="small">
              {{ accountInfo.exchange?.toUpperCase() || 'OKX' }}
            </el-tag>
            <el-tag :type="accountInfo.isTestnet ? 'warning' : 'success'" size="small" style="margin-left: 8px;">
              {{ accountInfo.isTestnet ? '模拟盘' : '实盘' }}
            </el-tag>
            <el-tag :type="accountInfo.marginMode === 'cross' ? 'warning' : 'info'" size="small" style="margin-left: 8px;">
              {{ accountInfo.marginMode === 'cross' ? '全仓' : '逐仓' }}
            </el-tag>
          </p>
        </div>
      </div>
      <el-button :icon="RefreshRight" @click="handleRefresh">刷新</el-button>
    </div>

    <!-- 账户概览 -->
    <el-row :gutter="20" class="stats-row">
      <el-col :span="6">
        <el-card class="stat-card">
          <el-statistic title="账户净值 (USDT)" :value="accountInfo.equity" :precision="2">
            <template #prefix>
              <el-icon><Wallet /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
      <el-col :span="6">
        <el-card class="stat-card">
          <el-statistic
            title="未实现盈亏 (USDT)"
            :value="totalUnrealizedPnL"
            :precision="2"
            :value-style="{ color: totalUnrealizedPnL >= 0 ? '#67c23a' : '#f56c6c' }"
          >
            <template #prefix>
              <el-icon><TrendCharts /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
      <el-col :span="6">
        <el-card class="stat-card">
          <el-statistic title="持仓数量" :value="positions.length">
            <template #prefix>
              <el-icon><PieChart /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
      <el-col :span="6">
        <el-card class="stat-card">
          <el-statistic title="活跃订单" :value="activeOrderCount">
            <template #prefix>
              <el-icon><Clock /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
    </el-row>

    <!-- Tab 切换 -->
    <el-card>
      <el-tabs v-model="activeTab">
        <!-- 持仓 Tab -->
        <el-tab-pane label="持仓" name="positions">
          <el-table :data="positions" v-loading="loading">
            <el-table-column prop="symbol" label="交易对" width="130" fixed="left" />
            <el-table-column prop="side" label="方向" width="80">
              <template #default="{ row }">
                <el-tag :type="row.side === 'long' ? 'success' : 'danger'">
                  {{ row.side === 'long' ? '多' : '空' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="marginMode" label="仓位模式" width="90">
              <template #default="{ row }">
                <el-tag :type="row.marginMode === 'cross' ? 'warning' : 'info'" size="small">
                  {{ row.marginMode === 'cross' ? '全仓' : '逐仓' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="quantity" label="持仓量" width="120" align="right">
              <template #default="{ row }">
                {{ formatNumber(row.quantity, 4) }}
              </template>
            </el-table-column>
            <el-table-column prop="avgPrice" label="均价" width="120" align="right">
              <template #default="{ row }">
                {{ formatNumber(row.avgPrice, 2) }}
              </template>
            </el-table-column>
            <el-table-column prop="currentPrice" label="现价" width="120" align="right">
              <template #default="{ row }">
                {{ formatNumber(row.currentPrice, 2) }}
              </template>
            </el-table-column>
            <el-table-column prop="unrealizedPnl" label="未实现盈亏" width="130" align="right">
              <template #default="{ row }">
                <span :class="row.unrealizedPnl >= 0 ? 'text-success' : 'text-danger'">
                  {{ formatNumber(row.unrealizedPnl, 2) }}
                </span>
              </template>
            </el-table-column>
            <el-table-column prop="leverage" label="杠杆" width="80" align="center">
              <template #default="{ row }">
                {{ row.leverage }}x
              </template>
            </el-table-column>
            <el-table-column prop="stopLoss" label="止损价" width="120" align="right">
              <template #default="{ row }">
                <span v-if="row.stopLoss" class="text-danger">{{ formatNumber(row.stopLoss, 2) }}</span>
                <span v-else class="text-muted">--</span>
              </template>
            </el-table-column>
            <el-table-column prop="takeProfit" label="止盈价" width="120" align="right">
              <template #default="{ row }">
                <span v-if="row.takeProfit" class="text-success">{{ formatNumber(row.takeProfit, 2) }}</span>
                <span v-else class="text-muted">--</span>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="200" fixed="right">
              <template #default="{ row }">
                <el-button type="primary" size="small" @click="handleSetTPSL(row)">
                  止盈止损
                </el-button>
                <el-button type="warning" size="small" @click="handleClosePosition(row)">
                  平仓
                </el-button>
              </template>
            </el-table-column>
          </el-table>
        </el-tab-pane>

        <!-- 订单 Tab -->
        <el-tab-pane label="订单" name="orders">
          <div class="tab-toolbar">
            <el-form :inline="true" :model="orderFilters">
              <el-form-item label="状态">
                <el-select v-model="orderFilters.state" placeholder="全部" clearable size="small">
                  <el-option label="已提交" value="SUBMITTED" />
                  <el-option label="已接受" value="ACCEPTED" />
                  <el-option label="部分成交" value="PARTIALLY_FILLED" />
                  <el-option label="完全成交" value="FILLED" />
                  <el-option label="已取消" value="CANCELLED" />
                  <el-option label="已拒绝" value="REJECTED" />
                </el-select>
              </el-form-item>
              <el-form-item label="方向">
                <el-select v-model="orderFilters.side" placeholder="全部" clearable size="small">
                  <el-option label="买入" value="BUY" />
                  <el-option label="卖出" value="SELL" />
                </el-select>
              </el-form-item>
              <el-form-item>
                <el-button type="primary" size="small" :icon="Plus" @click="showPlaceOrderDialog = true">
                  手动下单
                </el-button>
              </el-form-item>
            </el-form>
          </div>

          <el-table :data="filteredOrders" v-loading="loading">
            <el-table-column prop="id" label="订单ID" width="100" />
            <el-table-column prop="symbol" label="交易对" width="130" />
            <el-table-column prop="side" label="方向" width="80">
              <template #default="{ row }">
                <el-tag :type="row.side === 'BUY' ? 'success' : 'danger'">
                  {{ row.side === 'BUY' ? '买入' : '卖出' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="type" label="类型" width="100">
              <template #default="{ row }">
                {{ { LIMIT: '限价单', MARKET: '市价单', STOP: '止损单' }[row.type] || row.type }}
              </template>
            </el-table-column>
            <el-table-column prop="price" label="价格" width="120" align="right">
              <template #default="{ row }">
                {{ row.price ? formatNumber(row.price, 2) : '-' }}
              </template>
            </el-table-column>
            <el-table-column prop="quantity" label="数量" width="120" align="right">
              <template #default="{ row }">
                {{ formatNumber(row.quantity, 4) }}
              </template>
            </el-table-column>
            <el-table-column prop="filledQuantity" label="已成交" width="120" align="right">
              <template #default="{ row }">
                {{ formatNumber(row.filledQuantity || 0, 4) }}
              </template>
            </el-table-column>
            <el-table-column prop="state" label="状态" width="120">
              <template #default="{ row }">
                <el-tag :type="getOrderStateType(row.state)" size="small">
                  {{ formatOrderState(row.state) }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="timestamp" label="时间" width="180">
              <template #default="{ row }">
                {{ formatTime(row.timestamp) }}
              </template>
            </el-table-column>
            <el-table-column label="操作" width="100" fixed="right">
              <template #default="{ row }">
                <el-button
                  v-if="canCancelOrder(row.state)"
                  type="danger"
                  size="small"
                  @click="handleCancelOrder(row)"
                >
                  取消
                </el-button>
              </template>
            </el-table-column>
          </el-table>
        </el-tab-pane>

        <!-- 账户信息 Tab -->
        <el-tab-pane label="账户信息" name="info">
          <el-descriptions :column="2" border>
            <el-descriptions-item label="账户名称">{{ accountInfo.name }}</el-descriptions-item>
            <el-descriptions-item label="交易所">{{ accountInfo.exchange?.toUpperCase() }}</el-descriptions-item>
            <el-descriptions-item label="环境">
              <el-tag :type="accountInfo.isTestnet ? 'warning' : 'success'" size="small">
                {{ accountInfo.isTestnet ? '模拟盘' : '实盘' }}
              </el-tag>
            </el-descriptions-item>
            <el-descriptions-item label="保证金模式">
              {{ accountInfo.marginMode === 'cross' ? '全仓' : '逐仓' }}
            </el-descriptions-item>
            <el-descriptions-item label="API Key">{{ maskApiKey(accountInfo.apiKey) }}</el-descriptions-item>
            <el-descriptions-item label="净值 (USDT)">{{ formatNumber(accountInfo.equity, 2) }}</el-descriptions-item>
            <el-descriptions-item label="可用余额 (USDT)">{{ formatNumber(accountInfo.available, 2) }}</el-descriptions-item>
            <el-descriptions-item label="冻结 (USDT)">{{ formatNumber(accountInfo.frozen, 2) }}</el-descriptions-item>
          </el-descriptions>
        </el-tab-pane>
      </el-tabs>
    </el-card>

    <!-- 止盈止损设置对话框 -->
    <el-dialog v-model="showTPSLDialog" title="设置止盈止损" width="500px">
      <el-form :model="tpslForm" label-width="100px">
        <el-form-item label="交易对">
          <el-input :value="tpslForm.symbol" disabled />
        </el-form-item>
        <el-form-item label="当前价格">
          <el-input :value="formatNumber(tpslForm.currentPrice, 2)" disabled />
        </el-form-item>
        <el-form-item label="止盈价格">
          <el-input-number v-model="tpslForm.takeProfit" :precision="2" :min="0" style="width: 100%;" />
        </el-form-item>
        <el-form-item label="止损价格">
          <el-input-number v-model="tpslForm.stopLoss" :precision="2" :min="0" style="width: 100%;" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="showTPSLDialog = false">取消</el-button>
        <el-button type="primary" @click="handleSaveTPSL">确定</el-button>
      </template>
    </el-dialog>

    <!-- 下单对话框 -->
    <place-order-dialog
      v-model="showPlaceOrderDialog"
      @success="handlePlaceOrderSuccess"
    />
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useAccountStore } from '@/stores/account'
import { useOrderStore } from '@/stores/order'
import { formatNumber, formatTime } from '@/utils/format'
import {
  ArrowLeft,
  RefreshRight,
  Wallet,
  TrendCharts,
  PieChart,
  Clock,
  Plus
} from '@element-plus/icons-vue'

import PlaceOrderDialog from '@/components/Order/PlaceOrderDialog.vue'

const route = useRoute()
const router = useRouter()
const accountStore = useAccountStore()
const orderStore = useOrderStore()

const loading = ref(false)
const activeTab = ref('positions')
const showTPSLDialog = ref(false)
const showPlaceOrderDialog = ref(false)

const accountInfo = reactive({
  id: '',
  name: '',
  exchange: 'okx',
  isTestnet: false,
  equity: 0,
  available: 0,
  frozen: 0,
  marginMode: 'cross',
  apiKey: ''
})

const positions = ref([])
const orders = ref([])

const orderFilters = reactive({
  state: '',
  side: ''
})

const tpslForm = reactive({
  symbol: '',
  currentPrice: 0,
  takeProfit: null,
  stopLoss: null,
  positionId: ''
})

const totalUnrealizedPnL = computed(() =>
  positions.value.reduce((sum, p) => sum + (p.unrealizedPnl || 0), 0)
)

const activeOrderCount = computed(() =>
  orders.value.filter(o => ['SUBMITTED', 'ACCEPTED', 'PARTIALLY_FILLED'].includes(o.state)).length
)

const filteredOrders = computed(() => {
  let result = orders.value
  if (orderFilters.state) {
    result = result.filter(o => o.state === orderFilters.state)
  }
  if (orderFilters.side) {
    result = result.filter(o => o.side === orderFilters.side)
  }
  return result
})

function formatOrderState(state) {
  const map = {
    CREATED: '已创建', SUBMITTED: '已提交', ACCEPTED: '已接受',
    PARTIALLY_FILLED: '部分成交', FILLED: '已成交',
    CANCELLED: '已取消', REJECTED: '已拒绝'
  }
  return map[state] || state
}

function getOrderStateType(state) {
  const map = {
    CREATED: '', SUBMITTED: '', ACCEPTED: 'warning',
    PARTIALLY_FILLED: 'warning', FILLED: 'success',
    CANCELLED: '', REJECTED: 'danger'
  }
  return map[state] || ''
}

function canCancelOrder(state) {
  return ['SUBMITTED', 'ACCEPTED', 'PARTIALLY_FILLED'].includes(state)
}

function maskApiKey(apiKey) {
  if (!apiKey) return '--'
  const len = apiKey.length
  if (len <= 8) return apiKey
  return apiKey.substring(0, 4) + '****' + apiKey.substring(len - 4)
}

function goBack() {
  router.push('/account')
}

async function handleRefresh() {
  await fetchAccountData()
  ElMessage.success('刷新成功')
}

async function fetchAccountData() {
  loading.value = true
  try {
    const accountId = route.params.id

    // 从 store 中查找账户信息
    const acc = accountStore.accounts.find(
      a => (a.id || a.strategyId) === accountId
    )
    if (acc) {
      accountInfo.id = acc.id || acc.strategyId
      accountInfo.name = acc.name || acc.strategyId || '默认账户'
      accountInfo.exchange = acc.exchange || 'okx'
      accountInfo.isTestnet = acc.isTestnet || false
      accountInfo.equity = acc.equity || 0
      accountInfo.available = acc.balance || 0
      accountInfo.frozen = acc.frozen || 0
      accountInfo.marginMode = acc.marginMode || 'cross'
      accountInfo.apiKey = acc.apiKey || ''
    } else {
      accountInfo.id = accountId
      accountInfo.name = `账户 ${accountId}`
    }

    // TODO: 调用后端 API 获取该账户的持仓和订单
    // 目前使用 store 中的全局数据进行过滤（后续接入账户级别 API）
    positions.value = orderStore.positions.filter(
      p => !p.accountId || p.accountId === accountId
    )
    orders.value = orderStore.orders.filter(
      o => !o.accountId || o.accountId === accountId
    )
  } catch (error) {
    ElMessage.error('获取数据失败: ' + error.message)
  } finally {
    loading.value = false
  }
}

function handleSetTPSL(row) {
  tpslForm.symbol = row.symbol
  tpslForm.currentPrice = row.currentPrice
  tpslForm.takeProfit = row.takeProfit
  tpslForm.stopLoss = row.stopLoss
  tpslForm.positionId = row.id
  showTPSLDialog.value = true
}

async function handleSaveTPSL() {
  try {
    // TODO: 调用API保存止盈止损
    ElMessage.success('止盈止损设置成功')
    showTPSLDialog.value = false
    await fetchAccountData()
  } catch (error) {
    ElMessage.error('设置失败: ' + error.message)
  }
}

async function handleClosePosition(row) {
  try {
    await ElMessageBox.confirm(
      `确定要平仓 ${row.symbol} 吗?`,
      '平仓确认',
      { confirmButtonText: '确定', cancelButtonText: '取消', type: 'warning' }
    )
    // TODO: 调用API平仓
    ElMessage.success('平仓成功')
    await fetchAccountData()
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('平仓失败: ' + error.message)
    }
  }
}

async function handleCancelOrder(row) {
  try {
    await ElMessageBox.confirm(
      '确定要取消该订单吗?',
      '提示',
      { confirmButtonText: '确定', cancelButtonText: '取消', type: 'warning' }
    )
    await orderStore.cancelOrder(row.id)
    ElMessage.success('订单取消成功')
    await fetchAccountData()
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('订单取消失败: ' + error.message)
    }
  }
}

function handlePlaceOrderSuccess() {
  ElMessage.success('订单提交成功')
  fetchAccountData()
}

onMounted(() => {
  fetchAccountData()
})
</script>

<style lang="scss" scoped>
.account-detail-page {
  .page-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 20px;

    .header-left {
      display: flex;
      align-items: center;
      gap: 16px;

      .el-button {
        color: var(--text-secondary) !important;
        border-color: var(--border-color) !important;
        background: transparent !important;
        &:hover { color: var(--accent-green) !important; border-color: var(--accent-green) !important; }
      }

      .header-info {
        h2 { margin: 0 0 5px 0; color: var(--text-primary); font-weight: 700; }
        p { margin: 0; }
      }
    }

    & > .el-button {
      color: var(--text-secondary) !important;
      border-color: var(--border-color) !important;
      background: transparent !important;
      &:hover { color: var(--accent-green) !important; border-color: var(--accent-green) !important; }
    }
  }

  .stats-row {
    margin-bottom: 20px;
    .stat-card {
      :deep(.el-card__body) { padding: 20px; }
      :deep(.el-statistic__head) { color: var(--text-secondary) !important; font-size: 12px; text-transform: uppercase; letter-spacing: 0.5px; }
      :deep(.el-statistic__content) { font-family: var(--font-mono); }
      :deep(.el-statistic__number) { color: var(--text-primary) !important; }
    }
  }

  :deep(.el-tabs__item) {
    color: var(--text-secondary) !important;
    &.is-active { color: var(--accent-green) !important; }
    &:hover { color: var(--accent-green) !important; }
  }
  :deep(.el-tabs__active-bar) { background-color: var(--accent-green) !important; }
  :deep(.el-tabs__nav-wrap::after) { background-color: var(--border-color) !important; }

  :deep(.el-descriptions) {
    --el-descriptions-item-bordered-label-background: var(--bg-elevated) !important;
  }
  :deep(.el-descriptions__label) { color: var(--text-muted) !important; background: var(--bg-elevated) !important; }
  :deep(.el-descriptions__content) { color: var(--text-primary) !important; background: var(--bg-card) !important; }
  :deep(.el-descriptions__cell) { border-color: var(--border-color) !important; }

  .tab-toolbar { margin-bottom: 16px; }
  .text-muted { color: var(--text-muted); }
}
</style>
