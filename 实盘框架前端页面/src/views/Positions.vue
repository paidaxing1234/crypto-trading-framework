<template>
  <div class="positions-page">
    <!-- 页面头部 -->
    <div class="page-header">
      <div>
        <h2>持仓管理</h2>
        <p>查看所有持仓信息</p>
      </div>
    </div>
    
    <!-- 持仓统计 -->
    <el-row :gutter="20" class="stats-row">
      <el-col :span="6">
        <el-card class="stat-card">
          <el-statistic title="持仓市值 (USDT)" :value="totalPositionValue" :precision="2">
            <template #prefix>
              <el-icon><PieChart /></el-icon>
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
          <el-statistic title="盈利/亏损" :value="`${profitablePositions}/${losingPositions}`">
            <template #prefix>
              <el-icon><DataAnalysis /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
    </el-row>
    
    <!-- 持仓分布图 -->
    <el-row :gutter="20" class="charts-row">
      <el-col :span="12">
        <el-card>
          <template #header>
            <span>持仓分布</span>
          </template>
          <position-distribution-chart :positions="positions" />
        </el-card>
      </el-col>
      
      <el-col :span="12">
        <el-card>
          <template #header>
            <span>盈亏分布</span>
          </template>
          <pnl-distribution-chart :positions="positions" />
        </el-card>
      </el-col>
    </el-row>
    
    <!-- 持仓列表 -->
    <el-card>
      <template #header>
        <div class="card-header">
          <span>持仓列表</span>
          <el-button :icon="RefreshRight" @click="handleRefresh">
            刷新
          </el-button>
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
        
        <el-table-column prop="quantity" label="数量" width="120" align="right">
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
        
        <el-table-column prop="notionalValue" label="市值 (USDT)" width="130" align="right">
          <template #default="{ row }">
            {{ formatNumber(row.notionalValue, 2) }}
          </template>
        </el-table-column>
        
        <el-table-column prop="unrealizedPnl" label="未实现盈亏" width="130" align="right">
          <template #default="{ row }">
            <span :class="row.unrealizedPnl >= 0 ? 'text-success' : 'text-danger'">
              {{ formatNumber(row.unrealizedPnl, 2) }}
            </span>
          </template>
        </el-table-column>
        
        <el-table-column prop="returnRate" label="收益率" width="100" align="right">
          <template #default="{ row }">
            <span :class="row.returnRate >= 0 ? 'text-success' : 'text-danger'">
              {{ formatPercent(row.returnRate / 100) }}
            </span>
          </template>
        </el-table-column>
        
        <el-table-column prop="leverage" label="杠杆" width="80" align="center">
          <template #default="{ row }">
            {{ row.leverage }}x
          </template>
        </el-table-column>
        
        <el-table-column prop="liquidationPrice" label="强平价" width="120" align="right">
          <template #default="{ row }">
            {{ formatNumber(row.liquidationPrice, 2) }}
          </template>
        </el-table-column>
        
        <el-table-column label="操作" width="150" fixed="right">
          <template #default="{ row }">
            <Permission :permission="'position:close'">
              <el-button type="warning" size="small" @click="handleClosePosition(row)">
                平仓
              </el-button>
            </Permission>
            <el-button type="primary" size="small" @click="handleViewDetail(row)">
              详情
            </el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useOrderStore } from '@/stores/order'
import { formatNumber, formatPercent } from '@/utils/format'
import {
  PieChart,
  TrendCharts,
  List,
  DataAnalysis,
  RefreshRight
} from '@element-plus/icons-vue'

import PositionDistributionChart from '@/components/Charts/PositionDistributionChart.vue'
import PnlDistributionChart from '@/components/Charts/PnlDistributionChart.vue'

const orderStore = useOrderStore()

const loading = computed(() => orderStore.loading)
const positions = computed(() => orderStore.positions)
const totalPositionValue = computed(() => orderStore.totalPositionValue)
const totalUnrealizedPnL = computed(() => orderStore.totalUnrealizedPnL)

const profitablePositions = computed(() =>
  positions.value.filter(p => p.unrealizedPnl > 0).length
)

const losingPositions = computed(() =>
  positions.value.filter(p => p.unrealizedPnl < 0).length
)

async function handleRefresh() {
  try {
    await orderStore.fetchPositions()
    ElMessage.success('刷新成功')
  } catch (error) {
    ElMessage.error('刷新失败: ' + error.message)
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
    
    // TODO: 实现平仓功能
    ElMessage.info('平仓功能开发中...')
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('平仓失败: ' + error.message)
    }
  }
}

function handleViewDetail(row) {
  // TODO: 显示持仓详情
  console.log('查看持仓详情:', row)
  ElMessage.info('持仓详情功能开发中...')
}

onMounted(() => {
  orderStore.fetchPositions()
})
</script>

<style lang="scss" scoped>
.positions-page {
  .page-header {
    margin-bottom: 20px;
    h2 { margin: 0 0 5px 0; color: var(--text-primary); font-weight: 700; }
    p { margin: 0; color: var(--text-secondary); font-size: 13px; }
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

  .charts-row {
    margin-bottom: 20px;
    .el-card {
      height: 350px;
      :deep(.el-card__body) { height: calc(100% - 57px); }
    }
  }

  .card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--text-primary);
  }
}
</style>

