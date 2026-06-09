<template>
  <div class="multi-account-chart">
    <!-- 账号选择器 -->
    <div class="chart-controls">
      <el-checkbox-group v-model="selectedAccounts" @change="updateChart">
        <el-checkbox 
          v-for="account in accounts" 
          :key="account.id" 
          :label="account.id"
          :disabled="selectedAccounts.length >= 5 && !selectedAccounts.includes(account.id)"
        >
          {{ account.name }}
        </el-checkbox>
      </el-checkbox-group>
      <div class="controls-right">
        <el-radio-group v-model="timeRange" size="small" @change="updateChart">
          <el-radio-button label="1d">1天</el-radio-button>
          <el-radio-button label="7d">7天</el-radio-button>
          <el-radio-button label="30d">30天</el-radio-button>
          <el-radio-button label="all">全部</el-radio-button>
        </el-radio-group>
      </div>
    </div>
    
    <!-- 图表 -->
    <div ref="chartRef" :style="{ width: '100%', height: height }" v-loading="loading"></div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, watch, nextTick } from 'vue'
import * as echarts from 'echarts'
import { useAccountStore } from '@/stores/account'

const props = defineProps({
  height: {
    type: String,
    default: '400px'
  }
})

const accountStore = useAccountStore()

const chartRef = ref(null)
const chartInstance = ref(null)
const loading = ref(false)
const selectedAccounts = ref([])
const timeRange = ref('7d')

// 颜色方案
const colors = ['#5470c6', '#91cc75', '#fac858', '#ee6666', '#73c0de', '#3ba272', '#fc8452', '#9a60b4']

const accounts = computed(() => accountStore.accounts || [])

// 生成单个账号的模拟数据
function generateAccountData(accountId, accountName) {
  const data = []
  const now = Date.now()
  const dayMs = 24 * 60 * 60 * 1000
  
  let days = 7
  if (timeRange.value === '1d') days = 1
  else if (timeRange.value === '30d') days = 30
  else if (timeRange.value === 'all') days = 90
  
  // 每个账号不同的起始值和波动
  const seed = accountId * 1000
  let value = 10000 + seed
  
  for (let i = days; i >= 0; i--) {
    const time = new Date(now - i * dayMs)
    // 不同账号不同的随机种子
    value += (Math.random() * accountId - 0.45) * 200
    data.push({
      time: time.toISOString().split('T')[0],
      value: parseFloat(value.toFixed(2))
    })
  }
  
  return {
    name: accountName,
    data: data
  }
}

function initChart() {
  if (!chartRef.value) return
  
  chartInstance.value = echarts.init(chartRef.value)
  updateChart()
}

function updateChart() {
  if (!chartInstance.value) return
  
  const seriesList = []
  const legendData = []
  
  // 为每个选中的账号生成数据系列
  selectedAccounts.value.forEach((accountId, index) => {
    const account = accounts.value.find(a => a.id === accountId)
    if (!account) return
    
    const accountData = generateAccountData(accountId, account.name)
    legendData.push(account.name)
    
    seriesList.push({
      name: account.name,
      type: 'line',
      smooth: true,
      symbol: 'circle',
      symbolSize: 6,
      itemStyle: {
        color: colors[index % colors.length]
      },
      lineStyle: {
        width: 2
      },
      data: accountData.data.map(d => [d.time, d.value])
    })
  })
  
  const option = {
    title: {
      text: '多账号净值对比',
      left: 'center',
      textStyle: {
        fontSize: 14,
        fontWeight: 'normal'
      }
    },
    tooltip: {
      trigger: 'axis',
      axisPointer: {
        type: 'cross'
      }
    },
    legend: {
      data: legendData,
      top: 30,
      left: 'center'
    },
    grid: {
      left: '3%',
      right: '4%',
      bottom: '3%',
      top: '60px',
      containLabel: true
    },
    xAxis: {
      type: 'category',
      boundaryGap: false,
      data: []
    },
    yAxis: {
      type: 'value',
      scale: true,
      axisLabel: {
        formatter: '{value} USDT'
      }
    },
    series: seriesList.length > 0 ? seriesList : []
  }
  
  chartInstance.value.setOption(option, true)
}

function resize() {
  chartInstance.value?.resize()
}

// 初始化选中第一个账号
watch(accounts, (newAccounts) => {
  if (newAccounts.length > 0 && selectedAccounts.value.length === 0) {
    selectedAccounts.value = [newAccounts[0].id]
    nextTick(() => updateChart())
  }
}, { immediate: true })

onMounted(async () => {
  await nextTick()
  initChart()
  window.addEventListener('resize', resize)
})

onUnmounted(() => {
  window.removeEventListener('resize', resize)
  chartInstance.value?.dispose()
})
</script>

<style lang="scss" scoped>
.multi-account-chart {
  .chart-controls {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 15px;
    padding: 10px;
    background: #f5f7fa;
    border-radius: 4px;
    
    .el-checkbox-group {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
    }
    
    .controls-right {
      display: flex;
      gap: 10px;
    }
  }
}
</style>

