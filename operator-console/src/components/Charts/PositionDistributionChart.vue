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
      value: pos.notionalValue || 0
    }))
  } else {
    // 模拟数据
    data = [
      { name: 'BTC-USDT', value: 15000 },
      { name: 'ETH-USDT', value: 8000 },
      { name: 'SOL-USDT', value: 5000 },
      { name: 'BNB-USDT', value: 3000 },
      { name: 'XRP-USDT', value: 2000 }
    ]
  }
  
  const option = {
    tooltip: {
      trigger: 'item',
      formatter: (params) => {
        return `
          <div style="padding: 5px;">
            <div style="font-weight: bold;">${params.name}</div>
            <div>市值: ${params.value.toFixed(2)} USDT</div>
            <div>占比: ${params.percent}%</div>
          </div>
        `
      }
    },
    legend: {
      orient: 'vertical',
      right: '10%',
      top: 'center'
    },
    series: [
      {
        name: '持仓分布',
        type: 'pie',
        radius: ['40%', '70%'],
        center: ['35%', '50%'],
        avoidLabelOverlap: false,
        itemStyle: {
          borderRadius: 10,
          borderColor: '#fff',
          borderWidth: 2
        },
        label: {
          show: false,
          position: 'center'
        },
        emphasis: {
          label: {
            show: true,
            fontSize: 20,
            fontWeight: 'bold'
          }
        },
        labelLine: {
          show: false
        },
        data: data
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

