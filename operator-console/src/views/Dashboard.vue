<template>
  <div class="dashboard">
    <!-- 根据角色显示不同的Dashboard -->
    <super-admin-dashboard v-if="userStore.isSuperAdmin" />
    <strategy-manager-dashboard v-else-if="userStore.isStrategyManager" />
    
    <!-- 通用Dashboard（备用） -->
    <div v-else class="default-dashboard">
    <el-row :gutter="20" class="stats-row">
      <!-- 总资产 -->
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
      
      <!-- 未实现盈亏 -->
      <el-col :xs="24" :sm="12" :md="6">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">未实现盈亏 (USDT)</span>
            <el-icon color="#67c23a"><TrendCharts /></el-icon>
          </div>
          <div class="stat-value" :class="totalPnL >= 0 ? 'text-success' : 'text-danger'">
            {{ formatNumber(totalPnL, 2) }}
          </div>
          <div class="stat-change">
            今日收益: {{ formatNumber(todayPnL, 2) }}
          </div>
        </div>
      </el-col>
      
      <!-- 运行策略数 -->
      <el-col :xs="24" :sm="12" :md="6">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">运行中策略</span>
            <el-icon color="#e6a23c"><SetUp /></el-icon>
          </div>
          <div class="stat-value">{{ runningStrategies.length }}</div>
          <div class="stat-change">
            总策略数: {{ strategies.length }}
          </div>
        </div>
      </el-col>
      
      <!-- 活跃订单 -->
      <el-col :xs="24" :sm="12" :md="6">
        <div class="stat-card">
          <div class="stat-header">
            <span class="stat-label">活跃订单</span>
            <el-icon color="#f56c6c"><List /></el-icon>
          </div>
          <div class="stat-value">{{ activeOrders.length }}</div>
          <div class="stat-change">
            今日成交: {{ todayTrades }}
          </div>
        </div>
      </el-col>
    </el-row>
    
    <!-- 数据列表 -->
    <el-row :gutter="20" class="tables-row">
      <!-- 运行中的策略 -->
      <el-col :xs="24" :md="12">
        <el-card>
          <template #header>
            <div class="card-header">
              <span>运行中的策略</span>
              <el-button type="primary" size="small" @click="$router.push('/strategy')">
                查看全部
              </el-button>
            </div>
          </template>
          <running-strategies-table :strategies="runningStrategies.slice(0, 5)" />
        </el-card>
      </el-col>
      
      <!-- 最近订单 -->
      <el-col :xs="24" :md="12">
        <el-card>
          <template #header>
            <div class="card-header">
              <span>最近订单</span>
              <el-button type="primary" size="small" @click="$router.push('/orders')">
                查看全部
              </el-button>
            </div>
          </template>
          <recent-orders-table :orders="orders.slice(0, 5)" />
        </el-card>
      </el-col>
    </el-row>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { useUserStore } from '@/stores/user'
import { useAccountStore } from '@/stores/account'
import { useStrategyStore } from '@/stores/strategy'
import { useOrderStore } from '@/stores/order'
import { formatNumber, formatPercent } from '@/utils/format'
import SuperAdminDashboard from '@/views/Dashboard/SuperAdminDashboard.vue'
import StrategyManagerDashboard from '@/views/Dashboard/StrategyManagerDashboard.vue'
import { Wallet, TrendCharts, SetUp, List } from '@element-plus/icons-vue'
import RunningStrategiesTable from '@/components/Strategy/RunningStrategiesTable.vue'
import RecentOrdersTable from '@/components/Order/RecentOrdersTable.vue'

const userStore = useUserStore()

const accountStore = useAccountStore()
const strategyStore = useStrategyStore()
const orderStore = useOrderStore()

// 计算属性
const totalEquity = computed(() => accountStore.totalEquity)
const totalPnL = computed(() => accountStore.totalPnL)
const runningStrategies = computed(() => strategyStore.runningStrategies)
const strategies = computed(() => strategyStore.strategies)
const activeOrders = computed(() => orderStore.activeOrders)
const orders = computed(() => orderStore.orders)

// 模拟数据
const equityChange = ref(5.23)
const todayPnL = ref(1250.50)
const todayTrades = ref(45)

onMounted(async () => {
  // 加载数据
  await Promise.all([
    accountStore.fetchAccounts(),
    strategyStore.fetchStrategies(),
    orderStore.fetchOrders({ limit: 10 })
  ])
})
</script>

<style lang="scss" scoped>
.dashboard {
  .stats-row {
    margin-bottom: 24px;
  }

  .stat-card {
    padding: 24px;
    background: var(--bg-card);
    border-radius: var(--radius);
    border: 1px solid var(--border-color);
    backdrop-filter: blur(12px);
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
      transform: translateY(-6px);
      border-color: var(--border-glow);
      box-shadow: var(--shadow-glow), var(--shadow-md);
      &::before { opacity: 1; }
    }

    @for $i from 1 through 4 {
      &:nth-child(#{$i}) {
        animation-delay: #{$i * 0.08}s;
      }
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

      .el-icon {
        opacity: 0.5;
        font-size: 20px;
      }
    }

    .stat-value {
      font-size: 32px;
      font-weight: 800;
      font-family: var(--font-mono);
      margin-bottom: 10px;
      color: var(--text-primary);
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
    }
  }

  .charts-row {
    margin-bottom: 24px;
  }

  .chart-card {
    height: 400px;

    :deep(.el-card__body) {
      height: calc(100% - 57px);
    }
  }

  .card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--text-primary);
    font-weight: 600;
    letter-spacing: -0.3px;
  }
}
</style>

