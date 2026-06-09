<template>
  <div ref="chartRef" :style="{ width: '100%', height: height }" v-loading="loading"></div>
</template>

<script setup>
import { ref, onMounted, onUnmounted, watch, nextTick } from 'vue'
import * as echarts from 'echarts'

// 滑点冲击成本(每次调仓一根柱) + 加权滑点bp(折线, 右轴)。纯展示, 数据由父组件传入。
const props = defineProps({
  points: { type: Array, default: () => [] },   // [{ts, cost_usdt, wbps, notional_usdt, trades, fee_usdt, rebalance_no}]
  loading: { type: Boolean, default: false },
  height: { type: String, default: '300px' }
})

const chartRef = ref(null)
let chart = null

function fmtDate(ts) {
  const d = new Date(ts)
  const p = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}

function render() {
  if (!chart) return
  const pts = props.points || []
  chart.setOption({
    tooltip: {
      trigger: 'axis',
      formatter: (ps) => {
        const p = pts[ps[0]?.dataIndex] || {}
        return `<b>${fmtDate(p.ts)}</b> 调仓#${p.rebalance_no ?? '-'}<br/>` +
          `冲击成本: <b>${(p.cost_usdt ?? 0).toFixed(2)} U</b><br/>` +
          `加权滑点: ${p.wbps == null ? '-' : p.wbps.toFixed(2) + ' bp'}<br/>` +
          `成交名义: ${(p.notional_usdt ?? 0).toLocaleString()} U<br/>` +
          `成交 ${p.trades ?? '-'} 笔 | 手续费 ${(p.fee_usdt ?? 0).toFixed(2)} U`
      }
    },
    legend: { data: ['冲击成本(USDT)', '加权滑点(bp)'] },
    grid: { left: '3%', right: '4%', bottom: '3%', top: '16%', containLabel: true },
    xAxis: {
      type: 'category',
      data: pts.map(p => { const d = new Date(p.ts); return `${d.getMonth() + 1}/${d.getDate()}` })
    },
    yAxis: [
      { type: 'value', name: 'USDT' },
      { type: 'value', name: 'bp', splitLine: { show: false } }
    ],
    series: [
      {
        name: '冲击成本(USDT)', type: 'bar',
        data: pts.map(p => p.cost_usdt == null ? 0 : +p.cost_usdt.toFixed(4)),
        // 正(吃亏)红 / 负(优于决策价)绿
        itemStyle: { color: (q) => ((pts[q.dataIndex]?.cost_usdt ?? 0) >= 0 ? '#f56c6c' : '#67c23a') }
      },
      {
        name: '加权滑点(bp)', type: 'line', yAxisIndex: 1, smooth: true,
        data: pts.map(p => p.wbps), itemStyle: { color: '#409eff' }
      }
    ]
  }, true)
}

function resize() { chart?.resize() }

onMounted(async () => {
  await nextTick()
  chart = echarts.init(chartRef.value)
  render()
  window.addEventListener('resize', resize)
})

onUnmounted(() => {
  window.removeEventListener('resize', resize)
  chart?.dispose()
  chart = null
})

watch(() => props.points, () => render(), { deep: true })
</script>
