<template>
  <div class="viewer-dashboard">
    <el-row :gutter="20">
      <el-col :span="24">
        <el-alert
          title="观摩者模式"
          type="info"
          :closable="false"
          show-icon
        >
          您当前以观摩者身份登录，只能查看数据，无法执行交易操作
        </el-alert>
      </el-col>
    </el-row>
    
    <!-- 账号选择器 -->
    <el-row :gutter="20" style="margin-top: 20px;">
      <el-col :span="24">
        <el-card>
          <template #header>
            <span>选择要查看的账号</span>
          </template>
          <el-radio-group v-model="selectedAccountId" @change="handleAccountChange">
            <el-radio-button 
              v-for="account in accounts" 
              :key="account.id" 
              :label="account.id"
            >
              {{ account.name }}
            </el-radio-button>
          </el-radio-group>
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
    
    <!-- 净值曲线 -->
    <el-row :gutter="20" class="charts-row" v-if="selectedAccount">
      <el-col :span="24">
        <el-card>
          <template #header>
            <span>{{ selectedAccount.name }} - 净值曲线</span>
          </template>
          <equity-chart :account-id="selectedAccountId" height="300px" />
        </el-card>
      </el-col>
    </el-row>
    
    <!-- 持仓和订单（只读） -->
    <el-row :gutter="20" class="charts-row" v-if="selectedAccount">
      <el-col :span="12">
        <el-card>
          <template #header>
            <span>当前持仓</span>
          </template>
          <position-distribution-chart :positions="positions" />
        </el-card>
      </el-col>
      
      <el-col :span="12">
        <el-card>
          <template #header>
            <span>最近订单（只读）</span>
          </template>
          <el-table :data="recentOrders" max-height="300" size="small">
            <el-table-column prop="symbol" label="交易对" width="120" />
            <el-table-column prop="side" label="方向" width="60">
              <template #default="{ row }">
                <el-tag :type="row.side === 'BUY' ? 'success' : 'danger'" size="small">
                  {{ row.side }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="quantity" label="数量" align="right" />
            <el-table-column prop="state" label="状态" width="100">
              <template #default="{ row }">
                <el-tag size="small">{{ row.state }}</el-tag>
              </template>
            </el-table-column>
          </el-table>
        </el-card>
      </el-col>
    </el-row>
  </div>
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import { useAccountStore } from '@/stores/account'
import { useOrderStore } from '@/stores/order'
import { formatNumber, formatPercent } from '@/utils/format'
import EquityChart from '@/components/Charts/EquityChart.vue'
import PositionDistributionChart from '@/components/Charts/PositionDistributionChart.vue'
import { Wallet, TrendCharts, DataLine } from '@element-plus/icons-vue'

const accountStore = useAccountStore()
const orderStore = useOrderStore()

const selectedAccountId = ref(null)

const accounts = computed(() => accountStore.accounts || [])
const selectedAccount = computed(() => 
  accounts.value.find(a => a.id === selectedAccountId.value)
)
const positions = computed(() => orderStore.positions || [])
const recentOrders = computed(() => (orderStore.orders || []).slice(0, 10))

// 初始化选中第一个账号
watch(accounts, (newAccounts) => {
  if (newAccounts.length > 0 && !selectedAccountId.value) {
    selectedAccountId.value = newAccounts[0].id
  }
}, { immediate: true })

function handleAccountChange() {
  // 账号切换时可以刷新数据
  console.log('切换账号:', selectedAccountId.value)
}
</script>

<style lang="scss" scoped>
.viewer-dashboard {
  .el-alert {
    margin-bottom: 24px;
    background: rgba(59, 130, 246, 0.06) !important;
    border: 1px solid rgba(59, 130, 246, 0.15) !important;
    color: var(--text-primary) !important;
    border-radius: var(--radius) !important;
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

    .stat-change { font-size: 12px; color: var(--text-muted); margin-top: 8px; font-family: var(--font-mono); font-weight: 500; }
  }

  .charts-row { margin-bottom: 24px; }

  .card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--text-primary);
    font-weight: 600;
  }
}
</style>

