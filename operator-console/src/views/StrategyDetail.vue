<template>
  <div class="strategy-detail-page">
    <!-- 头部 -->
    <div class="page-header">
      <div class="title-row">
        <el-button :icon="ArrowLeft" circle @click="goBack" />
        <div>
          <h2>{{ displayName }}</h2>
          <p>账户 {{ accountId || '—' }} · 策略与账户 1:1</p>
        </div>
      </div>
      <el-radio-group v-model="range" @change="loadCurve">
        <el-radio-button label="7d">7天</el-radio-button>
        <el-radio-button label="30d">30天</el-radio-button>
        <el-radio-button label="90d">90天</el-radio-button>
        <el-radio-button label="1y">1年</el-radio-button>
        <el-radio-button label="all">全部</el-radio-button>
      </el-radio-group>
    </div>

    <el-alert
      v-if="!statsStore.reachable"
      type="warning"
      :closable="false"
      show-icon
      title="统计服务 (stats_api:8003) 不可达，暂时无法加载指标 / 曲线。请确认该服务在运行。"
      style="margin-bottom: 16px"
    />

    <!-- 指标卡片 -->
    <el-row :gutter="16" class="metric-row" v-loading="statsLoading">
      <el-col v-for="m in metricCards" :key="m.label" :xs="12" :sm="8" :md="6" :lg="4">
        <div class="metric-card">
          <div class="metric-label">{{ m.label }}</div>
          <div class="metric-value" :class="m.cls">{{ m.value }}</div>
          <div class="metric-sub">{{ m.sub || ' ' }}</div>
        </div>
      </el-col>
    </el-row>

    <!-- 净值曲线 -->
    <el-card class="chart-card">
      <template #header>
        <div class="card-header">
          <span>净值曲线 ({{ rangeLabel }})</span>
          <span class="muted" v-if="curveMetrics.points != null">{{ curveMetrics.points }} 个采样点</span>
        </div>
      </template>
      <div v-if="curvePoints.length === 0 && !curveLoading" class="empty-curve">
        <el-empty :image-size="70" description="该区间暂无净值数据（记录器刚开始采集，曲线会随时间累积）" />
      </div>
      <account-equity-curve
        v-else
        :points="curvePoints"
        :initial-capital="initialCapital"
        :loading="curveLoading"
        height="380px"
      />
    </el-card>

    <!-- 滑点冲击成本 -->
    <el-card class="chart-card" style="margin-top: 20px">
      <template #header>
        <div class="card-header">
          <span>滑点冲击成本 · 每次调仓 ({{ rangeLabel }})</span>
          <span class="muted" v-if="slipTotals && slipTotals.rebalances">
            {{ slipTotals.rebalances }} 次调仓 | 成本合计 {{ slipTotals.cost_usdt }}U
            <template v-if="slipTotals.wavg_bps != null"> | 加权 {{ slipTotals.wavg_bps }}bp</template>
            | 手续费 {{ slipTotals.fee_usdt }}U
          </span>
        </div>
      </template>
      <div v-if="slipPoints.length === 0 && !slipLoading" class="empty-curve">
        <el-empty :image-size="60" description="暂无滑点数据(策略重启后, 每次调仓自动记录一次)" />
      </div>
      <slippage-chart v-else :points="slipPoints" :loading="slipLoading" height="300px" />
    </el-card>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ArrowLeft } from '@element-plus/icons-vue'
import { statsApi } from '@/utils/statsApi'
import { useStatsStore } from '@/stores/stats'
import { formatNumber, formatPercent } from '@/utils/format'
import AccountEquityCurve from '@/components/Charts/AccountEquityCurve.vue'
import SlippageChart from '@/components/Charts/SlippageChart.vue'

const route = useRoute()
const router = useRouter()
const statsStore = useStatsStore()

const strategyId = computed(() => route.params.id)
const accountId = computed(() => route.query.account_id || route.params.id)
const displayName = computed(() => route.query.name || route.params.id)

const range = ref('30d')
const rangeLabelMap = { '7d': '近7天', '30d': '近30天', '90d': '近90天', '1y': '近1年', 'all': '全部' }
const rangeLabel = computed(() => rangeLabelMap[range.value] || range.value)

const statsLoading = ref(false)
const curveLoading = ref(false)
const latest = ref({})           // 最新快照 (来自 account_stats)
const initialCapital = ref(null)
const curvePoints = ref([])
const curveMetrics = ref({})      // 区间指标 (来自 equity_curve)
const slipPoints = ref([])        // 滑点时序 (每次调仓一点)
const slipTotals = ref(null)
const slipLoading = ref(false)

function fmtPct(frac) {
  return (frac === null || frac === undefined) ? '-' : formatPercent(frac)
}
function fmtNum(v, d = 2) {
  return (v === null || v === undefined) ? '-' : formatNumber(v, d)
}

const metricCards = computed(() => {
  const l = latest.value || {}
  const m = curveMetrics.value || {}
  const ret = l.return_rate
  return [
    { label: '净值 (USDT)', value: fmtNum(l.equity), cls: '' },
    { label: '累计收益率', value: fmtPct(ret), cls: ret >= 0 ? 'pos' : (ret < 0 ? 'neg' : ''), sub: '相对初始本金' },
    { label: '累计盈亏 (USDT)', value: fmtNum(l.pnl), cls: l.pnl >= 0 ? 'pos' : (l.pnl < 0 ? 'neg' : ''), sub: '净值 - 本金' },
    { label: '可用余额', value: fmtNum(l.available), cls: '', sub: '可开仓资金' },
    { label: '钱包余额', value: fmtNum(l.wallet), cls: '', sub: '不含未实现' },
    { label: '未实现盈亏', value: fmtNum(l.upnl), cls: l.upnl >= 0 ? 'pos' : (l.upnl < 0 ? 'neg' : ''), sub: '当前持仓浮盈亏' },
    { label: '成交笔数', value: l.trade_count != null ? l.trade_count : '-', cls: '', sub: '累计成交' },
    { label: '夏普比率', value: m.sharpe != null ? Number(m.sharpe).toFixed(2) : '-', cls: m.sharpe >= 0 ? 'pos' : (m.sharpe < 0 ? 'neg' : ''), sub: rangeLabel.value },
    { label: '最大回撤', value: fmtPct(m.max_drawdown), cls: 'neg', sub: rangeLabel.value },
    { label: '年化收益', value: fmtPct(m.annualized_return), cls: m.annualized_return >= 0 ? 'pos' : (m.annualized_return < 0 ? 'neg' : ''), sub: rangeLabel.value },
    { label: '年化波动率', value: fmtPct(m.volatility), cls: '', sub: rangeLabel.value },
    { label: '本金 (USDT)', value: fmtNum(initialCapital.value), cls: '', sub: '配置值' }
  ]
})

async function loadStats() {
  if (!accountId.value) return
  statsLoading.value = true
  try {
    const data = await statsApi.accountStats(accountId.value)
    latest.value = data.latest || {}
    initialCapital.value = data.initial_capital ?? null
    statsStore.reachable = true
  } catch (e) {
    statsStore.reachable = false
  } finally {
    statsLoading.value = false
  }
}

async function loadCurve() {
  if (!accountId.value) return
  curveLoading.value = true
  loadSlippage()                                   // 滑点图跟随区间切换
  try {
    const data = await statsApi.equityCurve(accountId.value, range.value)
    curvePoints.value = data.points || []
    curveMetrics.value = data.metrics || {}
    if (data.initial_capital != null) initialCapital.value = data.initial_capital
    statsStore.reachable = true
  } catch (e) {
    statsStore.reachable = false
    curvePoints.value = []
    curveMetrics.value = {}
  } finally {
    curveLoading.value = false
  }
}

async function loadSlippage() {
  if (!accountId.value) return
  slipLoading.value = true
  try {
    const data = await statsApi.slippageHistory(accountId.value, range.value)
    slipPoints.value = data.points || []
    slipTotals.value = data.totals || null
  } catch (e) {
    slipPoints.value = []
    slipTotals.value = null
  } finally {
    slipLoading.value = false
  }
}

function goBack() {
  router.push('/strategy')
}

onMounted(() => {
  loadStats()
  loadCurve()
})
</script>

<style lang="scss" scoped>
.strategy-detail-page {
  .page-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 24px;

    .title-row {
      display: flex;
      align-items: center;
      gap: 16px;

      h2 { margin: 0 0 4px 0; color: var(--text-primary); font-weight: 800; font-size: 22px; letter-spacing: -0.5px; }
      p { margin: 0; color: var(--text-muted); font-size: 13px; }
    }
  }

  .metric-row {
    margin-bottom: 20px;

    .el-col { margin-bottom: 16px; }

    .metric-card {
      background: var(--card-bg, #fff);
      border: 1px solid var(--border-color, #ebeef5);
      border-radius: var(--radius-sm, 8px);
      padding: 16px;
      height: 100%;

      .metric-label {
        font-size: 11px;
        color: var(--text-muted);
        text-transform: uppercase;
        letter-spacing: 0.5px;
        font-weight: 600;
        margin-bottom: 8px;
      }
      .metric-value {
        font-size: 22px;
        font-weight: 800;
        font-family: var(--font-mono);
        color: var(--text-primary);
        letter-spacing: -0.5px;
        line-height: 1.1;
        &.pos { color: var(--accent-green, #67c23a); }
        &.neg { color: var(--accent-red, #f56c6c); }
      }
      .metric-sub {
        font-size: 11px;
        color: var(--text-muted);
        margin-top: 6px;
      }
    }
  }

  .chart-card {
    .card-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      color: var(--text-primary);
      font-weight: 600;
      .muted { color: var(--text-muted); font-size: 12px; font-weight: 400; }
    }
    .empty-curve { padding: 40px 0; }
  }
}
</style>
