<template>
  <div ref="chartRef" style="width: 100%; height: 100%"></div>
</template>

<script setup>
import { ref, onMounted, onUnmounted, nextTick } from 'vue'
import * as echarts from 'echarts'

const chartRef = ref(null)
const chartInstance = ref(null)

// 模拟数据
const mockData = [
  { name: '网格策略A', value: 15.8 },
  { name: '趋势策略B', value: 12.3 },
  { name: '套利策略C', value: 8.5 },
  { name: '做市策略D', value: 6.2 },
  { name: '均值回归E', value: 4.1 }
]

function initChart() {
  if (!chartRef.value) return
  
  chartInstance.value = echarts.init(chartRef.value)
  
  const option = {
    tooltip: {
      trigger: 'axis',
      axisPointer: {
        type: 'shadow'
      },
      formatter: (params) => {
        const param = params[0]
        return `
          <div style="padding: 5px;">
            <div style="font-weight: bold;">${param.name}</div>
            <div>收益率: ${param.value}%</div>
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
      type: 'value',
      axisLabel: {
        formatter: '{value}%'
      }
    },
    yAxis: {
      type: 'category',
      data: mockData.map(item => item.name),
      inverse: true
    },
    series: [
      {
        name: '收益率',
        type: 'bar',
        data: mockData.map(item => ({
          value: item.value,
          itemStyle: {
            color: new echarts.graphic.LinearGradient(0, 0, 1, 0, [
              { offset: 0, color: '#83bff6' },
              { offset: 0.5, color: '#188df0' },
              { offset: 1, color: '#188df0' }
            ])
          }
        })),
        label: {
          show: true,
          position: 'right',
          formatter: '{c}%'
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
</script>

