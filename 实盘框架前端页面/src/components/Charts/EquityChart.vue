<template>
  <div ref="chartRef" :style="{ width: '100%', height: height }" v-loading="loading"></div>
</template>

<script setup>
import { ref, onMounted, onUnmounted, watch, nextTick } from 'vue'
import * as echarts from 'echarts'
import { useAccountStore } from '@/stores/account'

const props = defineProps({
  accountId: {
    type: [String, Number],
    default: null
  },
  timeRange: {
    type: String,
    default: '7d'
  },
  height: {
    type: String,
    default: '100%'
  }
})

const accountStore = useAccountStore()

const chartRef = ref(null)
const chartInstance = ref(null)
const loading = ref(false)

// 模拟数据生成
function generateMockData() {
  const data = []
  const now = Date.now()
  const dayMs = 24 * 60 * 60 * 1000
  
  let days = 7
  if (props.timeRange === '1d') days = 1
  else if (props.timeRange === '30d') days = 30
  else if (props.timeRange === 'all') days = 90
  
  let value = 10000
  for (let i = days; i >= 0; i--) {
    const time = new Date(now - i * dayMs)
    value += (Math.random() - 0.45) * 200 // 有上升趋势的随机游走
    data.push([
      time.toISOString().split('T')[0],
      value.toFixed(2)
    ])
  }
  
  return data
}

function initChart() {
  if (!chartRef.value) return
  
  chartInstance.value = echarts.init(chartRef.value)
  updateChart()
}

function updateChart() {
  if (!chartInstance.value) return
  
  const data = generateMockData()
  
  const option = {
    tooltip: {
      trigger: 'axis',
      axisPointer: {
        type: 'cross'
      },
      formatter: (params) => {
        const param = params[0]
        const value = parseFloat(param.data[1])
        const initial = parseFloat(data[0][1])
        const change = value - initial
        const changePercent = ((change / initial) * 100).toFixed(2)
        
        return `
          <div style="padding: 5px;">
            <div style="font-weight: bold; margin-bottom: 5px;">${param.data[0]}</div>
            <div>净值: ${value.toFixed(2)} USDT</div>
            <div style="color: ${change >= 0 ? '#67c23a' : '#f56c6c'}">
              累计: ${change >= 0 ? '+' : ''}${change.toFixed(2)} (${changePercent}%)
            </div>
          </div>
        `
      }
    },
    grid: {
      left: '3%',
      right: '4%',
      bottom: '3%',
      top: '10%',
      containLabel: true
    },
    xAxis: {
      type: 'category',
      boundaryGap: false,
      data: data.map(item => item[0])
    },
    yAxis: {
      type: 'value',
      scale: true,
      axisLabel: {
        formatter: '{value} USDT'
      }
    },
    series: [
      {
        name: '净值',
        type: 'line',
        smooth: true,
        symbol: 'circle',
        symbolSize: 6,
        sampling: 'lttb',
        itemStyle: {
          color: '#409eff'
        },
        areaStyle: {
          color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
            {
              offset: 0,
              color: 'rgba(64, 158, 255, 0.3)'
            },
            {
              offset: 1,
              color: 'rgba(64, 158, 255, 0.05)'
            }
          ])
        },
        data: data.map(item => item[1])
      }
    ]
  }
  
  chartInstance.value.setOption(option)
}

async function fetchData() {
  if (!props.accountId) {
    updateChart()
    return
  }
  
  loading.value = true
  try {
    await accountStore.fetchAccountEquityCurve(props.accountId, props.timeRange)
    updateChart()
  } catch (error) {
    console.error('获取净值数据失败:', error)
    // 失败时使用模拟数据
    updateChart()
  } finally {
    loading.value = false
  }
}

function resize() {
  chartInstance.value?.resize()
}

onMounted(async () => {
  await nextTick()
  initChart()
  fetchData()
  window.addEventListener('resize', resize)
})

onUnmounted(() => {
  window.removeEventListener('resize', resize)
  chartInstance.value?.dispose()
})

watch(() => props.timeRange, () => {
  fetchData()
})
</script>

