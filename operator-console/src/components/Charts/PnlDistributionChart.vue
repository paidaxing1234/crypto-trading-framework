<template>
  <div ref="chartRef" style="width: 100%; height: 100%"></div>
</template>

<script setup>
import { ref, onMounted, onUnmounted, watch, nextTick } from 'vue'
import * as echarts from 'echarts'

const props = defineProps({
  positions: {
    type: Array,
    default: () => []
  }
})

const chartRef = ref(null)
const chartInstance = ref(null)

function initChart() {
  if (!chartRef.value) return
  
  chartInstance.value = echarts.init(chartRef.value)
  updateChart()
}

function updateChart() {
  if (!chartInstance.value) return
  
  // 使用真实数据或模拟数据
  let data = []
  if (props.positions && props.positions.length > 0) {
    data = props.positions.map(pos => ({
      name: pos.symbol,
      value: pos.unrealizedPnl || 0
    }))
  } else {
    // 模拟数据
    data = [
      { name: 'BTC-USDT', value: 1250 },
      { name: 'ETH-USDT', value: 680 },
      { name: 'SOL-USDT', value: -320 },
      { name: 'BNB-USDT', value: 450 },
      { name: 'XRP-USDT', value: -180 }
    ]
  }
  
  const option = {
    tooltip: {
      trigger: 'axis',
      axisPointer: {
        type: 'shadow'
      },
      formatter: (params) => {
        const param = params[0]
        const value = param.value
        return `
          <div style="padding: 5px;">
            <div style="font-weight: bold;">${param.name}</div>
            <div style="color: ${value >= 0 ? '#67c23a' : '#f56c6c'}">
              盈亏: ${value >= 0 ? '+' : ''}${value.toFixed(2)} USDT
            </div>
          </div>
        `
      }
    },
    grid: {
      left: '3%',
      right: '4%',
      bottom: '3%',
      top: '3%',
      containLabel: true
    },
    xAxis: {
      type: 'category',
      data: data.map(item => item.name),
      axisLabel: {
        rotate: 45
      }
    },
    yAxis: {
      type: 'value',
      axisLabel: {
        formatter: '{value} USDT'
      }
    },
    series: [
      {
        name: '未实现盈亏',
        type: 'bar',
        data: data.map(item => ({
          value: item.value,
          itemStyle: {
            color: item.value >= 0 ? '#67c23a' : '#f56c6c'
          }
        })),
        label: {
          show: true,
          position: 'top',
          formatter: (params) => {
            const value = params.value
            return `${value >= 0 ? '+' : ''}${value.toFixed(0)}`
          }
        }
      }
    ]
  }
  
  chartInstance.value.setOption(option)
}

function resize() {
  chartInstance.value?.resize()
}

onMounted(async () => {
  await nextTick()
  initChart()
  window.addEventListener('resize', resize)
})

onUnmounted(() => {
  window.removeEventListener('resize', resize)
  chartInstance.value?.dispose()
})

watch(() => props.positions, () => {
  updateChart()
}, { deep: true })
</script>

