<template>
  <div class="multi-strategy-chart">
    <!-- 策略选择器 -->
    <div class="chart-controls">
      <el-checkbox-group v-model="selectedStrategies" @change="updateChart">
        <el-checkbox 
          v-for="strategy in strategies" 
          :key="strategy.id" 
          :label="strategy.id"
          :disabled="selectedStrategies.length >= 5 && !selectedStrategies.includes(strategy.id)"
        >
          {{ strategy.name }}
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
import { useStrategyStore } from '@/stores/strategy'

const props = defineProps({
  height: {
    type: String,
    default: '400px'
  }
})

const strategyStore = useStrategyStore()

const chartRef = ref(null)
const chartInstance = ref(null)
const loading = ref(false)
const selectedStrategies = ref([])
const timeRange = ref('7d')

// 颜色方案
const colors = ['#5470c6', '#91cc75', '#fac858', '#ee6666', '#73c0de', '#3ba272', '#fc8452', '#9a60b4']

const strategies = computed(() => strategyStore.strategies || [])

// 生成单个策略的模拟数据
function generateStrategyData(strategyId, strategyName) {
  const data = []
  const now = Date.now()
  const dayMs = 24 * 60 * 60 * 1000
  
  let days = 7
  if (timeRange.value === '1d') days = 1
  else if (timeRange.value === '30d') days = 30
  else if (timeRange.value === 'all') days = 90
  
  // 每个策略不同的起始值和波动特征
  const seed = strategyId * 100
  let value = 0
  
  for (let i = days; i >= 0; i--) {
    const time = new Date(now - i * dayMs)
    // 不同策略不同的表现
    const trend = (strategyId % 3 === 0) ? 0.5 : -0.3  // 有的盈利，有的亏损
    value += (Math.random() + trend) * 50
    data.push({
      time: time.toISOString().split('T')[0],
      value: parseFloat(value.toFixed(2))
    })
  }
  
  return {
    name: strategyName,
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
  
  // 为每个选中的策略生成数据系列
  selectedStrategies.value.forEach((strategyId, index) => {
    const strategy = strategies.value.find(s => s.id === strategyId)
    if (!strategy) return
    
    const strategyData = generateStrategyData(strategyId, strategy.name)
    legendData.push(strategy.name)
    
    seriesList.push({
      name: strategy.name,
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
      data: strategyData.data.map(d => [d.time, d.value])
    })
  })
  
  const option = {
    title: {
      text: '多策略收益对比',
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
      },
      formatter: (params) => {
        let html = `<div style="padding: 5px;"><div style="font-weight: bold; margin-bottom: 5px;">${params[0].axisValue}</div>`
        params.forEach(param => {
          const value = parseFloat(param.data[1])
          const color = value >= 0 ? '#67c23a' : '#f56c6c'
          html += `
            <div style="margin: 3px 0;">
              <span style="display:inline-block;width:10px;height:10px;background:${param.color};border-radius:50%;margin-right:5px;"></span>
              ${param.seriesName}: <span style="color:${color};font-weight:bold;">${value >= 0 ? '+' : ''}${value.toFixed(2)}</span>
            </div>
          `
        })
        html += '</div>'
        return html
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
      },
      splitLine: {
        lineStyle: {
          type: 'dashed'
        }
      }
    },
    series: seriesList.length > 0 ? seriesList : []
  }
  
  chartInstance.value.setOption(option, true)
}

function resize() {
  chartInstance.value?.resize()
}

// 初始化：自动选中前3个策略
watch(strategies, (newStrategies) => {
  if (newStrategies.length > 0 && selectedStrategies.value.length === 0) {
    selectedStrategies.value = newStrategies.slice(0, Math.min(3, newStrategies.length)).map(s => s.id)
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
    padding: 12px;
    background: #f5f7fa;
    border-radius: 4px;
    flex-wrap: wrap;
    gap: 10px;
    
    .el-checkbox-group {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
    }
    
    .controls-right {
      display: flex;
      gap: 10px;
      align-items: center;
    }
  }
}
</style>

