<template>
  <div ref="chartRef" :style="{ width: '100%', height: height }" v-loading="loading"></div>
</template>

<script setup>
import { ref, onMounted, onUnmounted, watch, nextTick } from 'vue'
import * as echarts from 'echarts'

// 纯展示型净值曲线: 父组件传入已取好的点 (points: [{ts, equity, upnl}]) 与本金线。
const props = defineProps({
  points: { type: Array, default: () => [] },
  initialCapital: { type: Number, default: null },
  loading: { type: Boolean, default: false },
  height: { type: String, default: '360px' }
})

const chartRef = ref(null)
let chart = null

function render() {
  if (!chart) return
  const pts = props.points || []
  const data = pts.map(p => [p.ts, Number(p.equity)])
  const base = props.initialCapital || (pts.length ? Number(pts[0].equity) : null)

  const series = [{
    name: '净值',
    type: 'line',
    smooth: true,
    showSymbol: false,
    sampling: 'lttb',
    data,
    itemStyle: { color: '#409eff' },
    areaStyle: {
      color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
        { offset: 0, color: 'rgba(64, 158, 255, 0.28)' },
        { offset: 1, color: 'rgba(64, 158, 255, 0.03)' }
      ])
    }
  }]

  // 本金参考线
  if (props.initialCapital) {
    series[0].markLine = {
      silent: true,
      symbol: 'none',
      data: [{ yAxis: props.initialCapital }],
      lineStyle: { type: 'dashed', color: '#909399' },
      label: { formatter: '本金 ' + props.initialCapital, position: 'insideEndTop', color: '#909399' }
    }
  }

  chart.setOption({
    tooltip: {
      trigger: 'axis',
      axisPointer: { type: 'cross' },
      formatter: (params) => {
        const p = params[0]
        const v = Number(p.data[1])
        const d = new Date(p.data[0])
        const ds = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')} ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
        let html = `<div style="font-weight:bold;margin-bottom:4px">${ds}</div><div>净值: ${v.toFixed(2)} USDT</div>`
        if (base) {
          const chg = v - base
          const pct = (chg / base * 100).toFixed(2)
          html += `<div style="color:${chg >= 0 ? '#67c23a' : '#f56c6c'}">较本金: ${chg >= 0 ? '+' : ''}${chg.toFixed(2)} (${pct}%)</div>`
        }
        return html
      }
    },
    grid: { left: '3%', right: '4%', bottom: '3%', top: '8%', containLabel: true },
    xAxis: { type: 'time' },
    yAxis: { type: 'value', scale: true, axisLabel: { formatter: (v) => v.toFixed(0) } },
    series
  }, true)
}

function initChart() {
  if (!chartRef.value) return
  chart = echarts.init(chartRef.value)
  render()
}

function resize() { chart?.resize() }

onMounted(async () => {
  await nextTick()
  initChart()
  window.addEventListener('resize', resize)
})

onUnmounted(() => {
  window.removeEventListener('resize', resize)
  chart?.dispose()
  chart = null
})

watch(() => props.points, () => render(), { deep: true })
watch(() => props.initialCapital, () => render())
</script>
