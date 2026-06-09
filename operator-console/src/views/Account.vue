<template>
  <div class="account-page">
    <!-- 页面头部 -->
    <div class="page-header">
      <div>
        <h2>账户管理</h2>
        <p>管理多策略多交易所账户 (OKX / Binance)</p>
      </div>
      <el-button
        type="primary"
        :icon="Plus"
        @click="showAddDialog = true"
        v-permission="'account:create'"
      >
        注册账户
      </el-button>
    </div>

    <!-- 账户选择器 + 概览 -->
    <el-card class="account-overview-card">
      <template #header>
        <div class="card-header">
          <span>账户概览</span>
          <el-select
            v-model="selectedAccountId"
            placeholder="请选择账户"
            style="width: 260px"
            clearable
            @change="onAccountSelect"
          >
            <el-option
              v-for="acc in accounts"
              :key="acc.id"
              :label="(acc.exchange?.toUpperCase() || 'OKX') + ' - ' + (acc.account_id || acc.name || acc.id)"
              :value="acc.id"
            />
          </el-select>
        </div>
      </template>

      <div v-if="selectedAccount" class="selected-account-overview">
        <!-- 选中账户的统计卡片 -->
        <el-row :gutter="20" class="overview-row">
          <el-col :span="6">
            <div class="stat-item">
              <div class="stat-label">交易所</div>
              <div class="stat-value">
                <el-tag :type="selectedAccount.exchange === 'okx' ? 'primary' : 'success'" size="small">
                  {{ selectedAccount.exchange?.toUpperCase() || 'OKX' }}
                </el-tag>
                <el-tag :type="(selectedAccount.is_testnet || selectedAccount.isTestnet) ? 'warning' : 'success'" size="small" style="margin-left: 6px;">
                  {{ (selectedAccount.is_testnet || selectedAccount.isTestnet) ? '模拟盘' : '实盘' }}
                </el-tag>
              </div>
            </div>
          </el-col>
          <el-col :span="6">
            <div class="stat-item">
              <div class="stat-label">账户ID</div>
              <div class="stat-value">{{ selectedAccount.account_id || selectedAccount.id }}</div>
            </div>
          </el-col>
          <el-col :span="6">
            <div class="stat-item">
              <div class="stat-label">总资产 (USDT)</div>
              <div class="stat-value highlight">{{ formatNumber(selectedAccount.equity, 2) || '0.00' }}</div>
            </div>
          </el-col>
          <el-col :span="6">
            <div class="stat-item">
              <div class="stat-label">未实现盈亏 (USDT)</div>
              <div class="stat-value" :class="(selectedAccount.unrealizedPnl || 0) >= 0 ? 'text-success' : 'text-danger'">
                {{ formatNumber(selectedAccount.unrealizedPnl, 2) || '0.00' }}
              </div>
            </div>
          </el-col>
        </el-row>

        <!-- 当前持仓 -->
        <div class="positions-section">
          <div class="positions-header">
            <span class="positions-title">当前持仓</span>
            <el-button :icon="Refresh" size="small" circle @click="loadPositions" :loading="positionsLoading" />
          </div>
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
        </div>
      </div>

      <div v-else class="no-account-selected">
        <el-empty description="请在右上角下拉框中选择一个账户查看详情" :image-size="80" />
      </div>
    </el-card>

    <!-- 账户列表 -->
    <el-card>
      <template #header>
        <div class="card-header">
          <span>账户列表 ({{ accounts.length }})</span>
          <el-input
            v-model="searchText"
            placeholder="搜索账户名称"
            :prefix-icon="Search"
            clearable
            style="width: 200px"
          />
        </div>
      </template>

      <el-table :data="filteredAccounts" v-loading="loading" row-key="id">
        <el-table-column type="expand">
          <template #default="{ row }">
            <account-detail :account="row" />
          </template>
        </el-table-column>

        <el-table-column prop="name" label="账户名称 / 交易所" min-width="180">
          <template #default="{ row }">
            <div class="account-name clickable" @click="handleAccountClick(row)">
              <el-tag :type="row.exchange === 'okx' ? 'primary' : 'success'" size="small">
                {{ row.exchange?.toUpperCase() || 'OKX' }}
              </el-tag>
              <span class="account-link">{{ row.name || row.strategy_id || '默认账户' }}</span>
            </div>
          </template>
        </el-table-column>

        <el-table-column prop="account_id" label="账户ID" width="150">
          <template #default="{ row }">
            {{ row.account_id || '--' }}
          </template>
        </el-table-column>

        <el-table-column prop="api_key" label="API Key" width="200">
          <template #default="{ row }">
            <el-text truncated>{{ row.api_key || maskApiKey(row.apiKey) }}</el-text>
          </template>
        </el-table-column>

        <el-table-column label="钱包余额 (USDT)" width="140" align="right">
          <template #header>
            <el-tooltip content="totalWalletBalance：账户里的本金，不含未实现盈亏" placement="top">
              <span>钱包余额 <el-icon style="vertical-align:-2px"><InfoFilled /></el-icon></span>
            </el-tooltip>
          </template>
          <template #default="{ row }">
            {{ formatNumber(statOf(row).wallet ?? row.balance, 2) }}
          </template>
        </el-table-column>

        <el-table-column prop="equity" label="净值 (USDT)" width="140" align="right">
          <template #header>
            <el-tooltip content="totalMarginBalance = 钱包余额 + 未实现盈亏，账户当前真实价值" placement="top">
              <span>净值 <el-icon style="vertical-align:-2px"><InfoFilled /></el-icon></span>
            </el-tooltip>
          </template>
          <template #default="{ row }">
            {{ formatNumber(statOf(row).equity ?? row.equity, 2) }}
          </template>
        </el-table-column>

        <el-table-column label="可用 (USDT)" width="140" align="right">
          <template #header>
            <el-tooltip content="availableBalance：扣除持仓占用保证金后，可用于开新仓的资金" placement="top">
              <span>可用 <el-icon style="vertical-align:-2px"><InfoFilled /></el-icon></span>
            </el-tooltip>
          </template>
          <template #default="{ row }">
            {{ formatNumber(statOf(row).available, 2) }}
          </template>
        </el-table-column>

        <el-table-column prop="unrealizedPnl" label="未实现盈亏" width="140" align="right">
          <template #default="{ row }">
            <span :class="(statOf(row).upnl ?? row.unrealizedPnl) >= 0 ? 'text-success' : 'text-danger'">
              {{ formatNumber(statOf(row).upnl ?? row.unrealizedPnl, 2) }}
            </span>
          </template>
        </el-table-column>

        <el-table-column label="收益率" width="110" align="right">
          <template #header>
            <el-tooltip content="(净值 - 初始本金) / 初始本金，自开仓以来累计" placement="top">
              <span>收益率 <el-icon style="vertical-align:-2px"><InfoFilled /></el-icon></span>
            </el-tooltip>
          </template>
          <template #default="{ row }">
            <span :class="statOf(row).return_rate == null ? '' : (statOf(row).return_rate >= 0 ? 'text-success' : 'text-danger')">
              {{ statOf(row).return_rate == null ? '-' : formatPercent(statOf(row).return_rate) }}
            </span>
          </template>
        </el-table-column>

        <el-table-column prop="status" label="状态" width="120">
          <template #default="{ row }">
            <el-tag :type="(row.is_testnet || row.isTestnet) ? 'warning' : 'success'">
              {{ (row.is_testnet || row.isTestnet) ? '模拟盘' : '实盘' }}
            </el-tag>
          </template>
        </el-table-column>

        <el-table-column label="操作" width="200" fixed="right">
          <template #default="{ row }">
            <el-button
              type="primary"
              size="small"
              plain
              @click="selectAccount(row)"
            >
              查看
            </el-button>
            <Permission :permission="'account:delete'">
              <el-button
                type="danger"
                size="small"
                @click="handleDelete(row)"
              >
                注销
              </el-button>
            </Permission>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- 添加账户对话框 -->
    <add-account-dialog
      v-model="showAddDialog"
      @success="handleAddSuccess"
    />
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, watch } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useAccountStore } from '@/stores/account'
import { useStatsStore } from '@/stores/stats'
import { wsClient } from '@/services/WebSocketClient'
import { strategyApi } from '@/api/strategy'
import { formatNumber, formatPercent, formatMoney } from '@/utils/format'
import {
  Plus,
  Search,
  Refresh,
  InfoFilled,
} from '@element-plus/icons-vue'

import AccountDetail from '@/components/Account/AccountDetail.vue'
import AddAccountDialog from '@/components/Account/AddAccountDialog.vue'

const router = useRouter()
const accountStore = useAccountStore()
const statsStore = useStatsStore()

const searchText = ref('')
const showAddDialog = ref(false)
const selectedAccountId = ref(null)
let statsTimer = null

// 取某账户的统计快照(钱包余额/可用/盈亏等, 来自独立 stats_api)
function statOf(row) {
  return statsStore.statOf(row.account_id || row.id)
}

// 持仓数据
const positions = ref([])
const positionsLoading = ref(false)
let positionsTimer = null

const loading = computed(() => accountStore.loading)
const accounts = computed(() => accountStore.accounts)

const selectedAccount = computed(() => {
  if (!selectedAccountId.value) return null
  return accounts.value.find(acc => acc.id === selectedAccountId.value) || null
})

const filteredAccounts = computed(() => {
  if (!searchText.value) return accounts.value

  return accounts.value.filter(acc =>
    (acc.account_id || acc.strategy_id || acc.name || '').toLowerCase().includes(searchText.value.toLowerCase())
  )
})

function maskApiKey(apiKey) {
  if (!apiKey) return ''
  const len = apiKey.length
  if (len <= 8) return apiKey
  return apiKey.substring(0, 4) + '****' + apiKey.substring(len - 4)
}

function selectAccount(row) {
  selectedAccountId.value = row.id
}

function onAccountSelect(val) {
  // el-select change 已经更新了 selectedAccountId
}

async function handleDelete(row) {
  try {
    await ElMessageBox.confirm(
      '确定要注销该账户吗? 此操作不可恢复。',
      '警告',
      {
        confirmButtonText: '确定',
        cancelButtonText: '取消',
        type: 'warning'
      }
    )

    await accountStore.deleteAccount(row.strategy_id || row.id || 'default', row.exchange || 'okx')
    ElMessage.success('账户注销成功')
    // 如果删除的是当前选中的账户，清空选择
    if (selectedAccountId.value === row.id) {
      selectedAccountId.value = null
    }
    // 从后端重新拉取账户列表(后端为准): store 里的本地 filter 按 acc.id 过滤, 但删除传的是
    // strategy_id, 两者不一致时本地删不掉 -> UI 仍显示已删账户。重新 fetch 以后端真实状态为准。
    await accountStore.fetchAccounts()
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('账户注销失败: ' + error.message)
    }
  }
}

function handleAddSuccess() {
  ElMessage.success('账户注册成功')
  accountStore.fetchAccounts()
}

function handleAccountClick(row) {
  router.push({
    path: '/account/' + (row.id || row.strategy_id)
  })
}

// === 持仓相关 ===
async function loadPositions() {
  const acc = selectedAccount.value
  if (!acc) return
  const accountId = acc.account_id || acc.id
  positionsLoading.value = true
  try {
    const res = await strategyApi.getAccountPositions(accountId)
    positions.value = res.data || []
  } finally {
    positionsLoading.value = false
  }
}

function getPositionSide(row) {
  // Binance: positionSide 或根据 positionAmt 正负判断
  if (row.positionSide && row.positionSide !== 'BOTH') return row.positionSide
  const amt = parseFloat(row.positionAmt || row.pos || 0)
  if (amt > 0) return 'LONG'
  if (amt < 0) return 'SHORT'
  // OKX: posSide
  if (row.posSide === 'long') return 'LONG'
  if (row.posSide === 'short') return 'SHORT'
  return ''
}

function formatPrice(price) {
  if (!price) return '--'
  const p = parseFloat(price)
  if (isNaN(p)) return '--'
  if (p >= 1000) return p.toFixed(2)
  if (p >= 1) return p.toFixed(4)
  return p.toFixed(6)
}

// 选中账户变化时加载持仓
watch(selectedAccountId, (val) => {
  if (positionsTimer) { clearInterval(positionsTimer); positionsTimer = null }
  if (val) {
    loadPositions()
    // 每 12 秒自动刷新持仓（与账户监控频率一致）
    positionsTimer = setInterval(loadPositions, 12000)
  } else {
    positions.value = []
  }
})

onMounted(async () => {
  console.log('[Account] onMounted, wsClient.connected:', wsClient.connected)
  // 拉取账户统计(钱包余额/可用/收益率), 并每 15s 刷新
  statsStore.fetchOverview()
  statsTimer = setInterval(() => statsStore.fetchOverview(), 15000)
  if (wsClient.connected) {
    try {
      await accountStore.fetchAccounts()
      console.log('[Account] fetchAccounts 完成, accounts:', accountStore.accounts.length)
      if (accountStore.accounts.length === 1) {
        selectedAccountId.value = accountStore.accounts[0].id
      }
    } catch (e) {
      console.error('[Account] fetchAccounts 失败:', e)
    }
  } else {
    const onConnected = () => {
      console.log('[Account] WebSocket 连接建立，开始获取账户')
      accountStore.fetchAccounts().then(() => {
        if (accountStore.accounts.length === 1) {
          selectedAccountId.value = accountStore.accounts[0].id
        }
      })
      wsClient.off('connected', onConnected)
    }
    wsClient.on('connected', onConnected)
  }
})

onUnmounted(() => {
  if (positionsTimer) { clearInterval(positionsTimer); positionsTimer = null }
  if (statsTimer) { clearInterval(statsTimer); statsTimer = null }
})
</script>

<style lang="scss" scoped>
.account-page {
  .page-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 28px;
    animation: fadeInUp 0.4s cubic-bezier(0.22, 1, 0.36, 1);

    h2 { margin: 0 0 6px 0; color: var(--text-primary); font-weight: 800; font-size: 22px; letter-spacing: -0.5px; }
    p { margin: 0; color: var(--text-muted); font-size: 13px; }
  }

  .account-overview-card { margin-bottom: 24px; }

  .selected-account-overview {
    .overview-row {
      margin-bottom: 24px;

      .stat-item {
        .stat-label {
          font-size: 11px;
          color: var(--text-muted);
          margin-bottom: 10px;
          text-transform: uppercase;
          letter-spacing: 1px;
          font-weight: 600;
        }
        .stat-value {
          font-size: 22px;
          font-weight: 700;
          font-family: var(--font-mono);
          color: var(--text-primary);
          letter-spacing: -0.5px;
          &.highlight { color: var(--accent-green); }
        }
      }
    }

    .positions-section {
      .positions-header {
        display: flex;
        justify-content: space-between;
        align-items: center;
        margin-bottom: 16px;

        .positions-title {
          font-size: 15px;
          font-weight: 700;
          color: var(--text-primary);
          letter-spacing: -0.3px;
        }

        .el-button {
          color: var(--text-secondary) !important;
          border-color: var(--border-color) !important;
          background: transparent !important;
          &:hover { color: var(--accent-green) !important; border-color: var(--accent-green) !important; }
        }
      }
    }
  }

  .no-account-selected {
    padding: 32px 0;
  }

  .card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--text-primary);
    font-weight: 600;
  }

  .account-name {
    display: flex;
    align-items: center;
    gap: 12px;

    &.clickable {
      cursor: pointer;

      .account-link {
        color: var(--accent-blue);
        font-weight: 500;
        transition: color 0.2s;

        &:hover {
          color: var(--accent-green);
          text-decoration: underline;
          text-underline-offset: 3px;
        }
      }
    }
  }
}
</style>
