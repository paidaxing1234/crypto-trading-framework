<template>
  <div class="account-positions-page">
    <!-- 页面头部 -->
    <div class="page-header">
      <div class="header-left">
        <el-button :icon="ArrowLeft" @click="goBack">返回</el-button>
        <div class="header-info">
          <h2>{{ accountInfo.name || '账户' }} - 持仓管理</h2>
          <p>
            <el-tag :type="accountInfo.exchange === 'okx' ? 'primary' : 'success'" size="small">
              {{ accountInfo.exchange?.toUpperCase() || 'OKX' }}
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
              <el-icon><List /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>

      <el-col :span="6">
        <el-card class="stat-card">
          <el-statistic title="挂单数量" :value="pendingOrders.length">
            <template #prefix>
              <el-icon><Clock /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
    </el-row>

    <!-- 持仓列表 -->
    <el-card>
      <template #header>
        <div class="card-header">
          <span>持仓列表</span>
        </div>
      </template>

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
    </el-card>

    <!-- 止盈止损挂单 -->
    <el-card style="margin-top: 20px;">
      <template #header>
        <div class="card-header">
          <span>止盈止损挂单</span>
        </div>
      </template>

      <el-table :data="pendingOrders" v-loading="loading">
        <el-table-column prop="symbol" label="交易对" width="130" />
        <el-table-column prop="type" label="类型" width="100">
          <template #default="{ row }">
            <el-tag :type="row.type === 'stop_loss' ? 'danger' : 'success'" size="small">
              {{ row.type === 'stop_loss' ? '止损' : '止盈' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="side" label="方向" width="80">
          <template #default="{ row }">
            <el-tag :type="row.side === 'buy' ? 'success' : 'danger'" size="small">
              {{ row.side === 'buy' ? '买入' : '卖出' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="triggerPrice" label="触发价" width="120" align="right">
          <template #default="{ row }">
            {{ formatNumber(row.triggerPrice, 2) }}
          </template>
        </el-table-column>
        <el-table-column prop="quantity" label="数量" width="120" align="right">
          <template #default="{ row }">
            {{ formatNumber(row.quantity, 4) }}
          </template>
        </el-table-column>
        <el-table-column prop="createTime" label="创建时间" width="180" />
        <el-table-column label="操作" width="100">
          <template #default="{ row }">
            <el-button type="danger" size="small" @click="handleCancelOrder(row)">
              撤销
            </el-button>
          </template>
        </el-table-column>
      </el-table>
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
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { formatNumber } from '@/utils/format'
import {
  ArrowLeft,
  RefreshRight,
  Wallet,
  TrendCharts,
  List,
  Clock
} from '@element-plus/icons-vue'

const route = useRoute()
const router = useRouter()

const loading = ref(false)
const showTPSLDialog = ref(false)

const accountInfo = reactive({
  id: '',
  name: '',
  exchange: 'okx',
  equity: 0,
  marginMode: 'cross'
})

const positions = ref([])
const pendingOrders = ref([])

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
    const accountId = route.query.accountId
    // TODO: 调用API获取账户数据
    // 模拟数据
    accountInfo.id = accountId
    accountInfo.name = `账户 ${accountId}`
    accountInfo.equity = 50000
    accountInfo.marginMode = 'cross'

    positions.value = [
      {
        id: '1',
        symbol: 'BTC-USDT',
        side: 'long',
        marginMode: 'cross',
        quantity: 0.5,
        avgPrice: 42000,
        currentPrice: 43500,
        unrealizedPnl: 750,
        leverage: 10,
        stopLoss: 40000,
        takeProfit: 48000
      },
      {
        id: '2',
        symbol: 'ETH-USDT',
        side: 'short',
        marginMode: 'isolated',
        quantity: 5,
        avgPrice: 2300,
        currentPrice: 2250,
        unrealizedPnl: 250,
        leverage: 5,
        stopLoss: null,
        takeProfit: null
      }
    ]

    pendingOrders.value = [
      {
        id: '1',
        symbol: 'BTC-USDT',
        type: 'stop_loss',
        side: 'sell',
        triggerPrice: 40000,
        quantity: 0.5,
        createTime: '2024-01-15 10:30:00'
      },
      {
        id: '2',
        symbol: 'BTC-USDT',
        type: 'take_profit',
        side: 'sell',
        triggerPrice: 48000,
        quantity: 0.5,
        createTime: '2024-01-15 10:30:00'
      }
    ]
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
      {
        confirmButtonText: '确定',
        cancelButtonText: '取消',
        type: 'warning'
      }
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
      `确定要撤销该挂单吗?`,
      '撤销确认',
      {
        confirmButtonText: '确定',
        cancelButtonText: '取消',
        type: 'warning'
      }
    )
    // TODO: 调用API撤销挂单
    ElMessage.success('撤销成功')
    await fetchAccountData()
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('撤销失败: ' + error.message)
    }
  }
}

onMounted(() => {
  fetchAccountData()
})
</script>

<style lang="scss" scoped>
.account-positions-page {
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

  .card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--text-primary);
  }

  .text-muted { color: var(--text-muted); }
}
</style>
