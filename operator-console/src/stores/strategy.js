import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { wsClient } from '@/services/WebSocketClient'
import { strategyApi } from '@/api/strategy'
import { statsApi } from '@/utils/statsApi'

export const useStrategyStore = defineStore('strategy', () => {
  // 状态
  const strategies = ref([])
  const currentStrategy = ref(null)
  const loading = ref(false)
  const strategyLogFiles = ref([])
  const strategyLogs = ref([])
  const logsLoading = ref(false)
  const currentLogFilename = ref('')
  // 策略"首次有日志记录"的日期(ms) —— 用作运行时长起算点, 对重启/暂停免疫。
  // key = strategy_id; value = ms 时间戳 / null(无可解析日期) / undefined(未查过)
  const firstLogDates = ref({})

  // WebSocket 连接后自动获取策略列表
  if (typeof window !== 'undefined') {
    wsClient.on('connected', () => {
      // 延迟获取，等待认证完成
      setTimeout(() => fetchStrategies(), 2000)
    })

    // 定时刷新策略列表（每10秒）
    setInterval(() => {
      if (wsClient.connected) {
        fetchStrategies()
      }
    }, 10000)
  }

  // 计算属性
  const runningStrategies = computed(() =>
    strategies.value.filter(s => s.status === 'running')
  )

  const stoppedStrategies = computed(() =>
    strategies.value.filter(s => s.status === 'stopped')
  )

  const pendingStrategies = computed(() =>
    strategies.value.filter(s => s.status === 'pending')
  )

  const errorStrategies = computed(() =>
    strategies.value.filter(s => s.status === 'error')
  )

  // 操作
  async function fetchStrategies(params) {
    loading.value = true
    try {
      const res = await strategyApi.getStrategies(params)
      // 后端返回 strategy_id，前端使用 id
      const rawData = res.data || []
      // 从独立统计服务取各账户盈亏/收益率/成交笔数(1:1 策略=账户), 按 account_id join。
      // 统计服务不可达时优雅降级(这些字段为 null, 表格显示 '-')。
      let ov = {}
      try {
        const data = await statsApi.accountsOverview()
        for (const a of (data.accounts || [])) {
          if (a && a.account_id) ov[a.account_id] = a
        }
      } catch (e) { /* stats api down -> degrade gracefully */ }
      strategies.value = rawData.map(s => {
        const aid = s.account_id || ''
        const st = ov[aid] || {}
        return {
          ...s,
          id: s.strategy_id || s.id,
          name: s.strategy_id || s.name || '',
          account: aid,
          type: s.exchange || 'unknown',
          pnl: st.pnl ?? null,
          returnRate: (st.return_rate !== null && st.return_rate !== undefined) ? st.return_rate * 100 : null,
          trades: (st.trade_count !== null && st.trade_count !== undefined) ? st.trade_count : null,
          statEquity: st.equity ?? null
        }
      })
      // 异步补"首次日志日期"(每个策略只查一次, 缓存)，用于运行时长起算
      strategies.value.forEach(s => fetchFirstLogDate(s.id))
      return res
    } finally {
      loading.value = false
    }
  }

  // 查某策略最早的日志日期(ms)。复用 get_strategy_log_files(按 strategy_id 子串匹配),
  // 从文件名里的 _YYYYMMDD 取最小值。每个策略只查一次(已查过则跳过)。
  async function fetchFirstLogDate(strategyId) {
    if (!strategyId || firstLogDates.value[strategyId] !== undefined) return
    // 先占位, 防止 10s 刷新期间重复发起
    firstLogDates.value = { ...firstLogDates.value, [strategyId]: null }
    try {
      const res = await strategyApi.getStrategyLogFiles(strategyId)
      const files = res.data || []
      let minMs = null
      for (const f of files) {
        const fn = String(f.filename || '')
        if (/dryrun|backtest/i.test(fn)) continue   // 排除试运行/回测日志, 只算真实运行
        const m = fn.match(/(\d{8})\.log$/)
        if (!m) continue
        const d = m[1]
        const ms = Date.parse(`${d.slice(0, 4)}-${d.slice(4, 6)}-${d.slice(6, 8)}T00:00:00`)
        if (!isNaN(ms) && (minMs === null || ms < minMs)) minMs = ms
      }
      firstLogDates.value = { ...firstLogDates.value, [strategyId]: minMs }
    } catch (e) {
      // 保留 null 占位(回退到 start_time)
    }
  }

  async function fetchStrategyDetail(id) {
    loading.value = true
    try {
      const res = await strategyApi.getStrategyDetail(id)
      currentStrategy.value = res.data
      return res
    } finally {
      loading.value = false
    }
  }

  async function startStrategy(id, config) {
    return await strategyApi.startStrategy(id, config)
  }

  async function stopStrategy(id) {
    return await strategyApi.stopStrategy(id)
  }

  async function createStrategy(data) {
    const res = await strategyApi.createStrategy(data)
    await fetchStrategies()
    return res
  }

  async function fetchStrategyConfigs() {
    try {
      return await strategyApi.listStrategyConfigs()
    } catch (error) {
      console.error('获取策略配置列表失败:', error)
      return { data: [], success: false }
    }
  }

  async function deleteStrategy(id) {
    const res = await strategyApi.deleteStrategy(id)
    strategies.value = strategies.value.filter(s => s.id !== id)
    return res
  }

  function updateStrategyStatus(id, status) {
    const strategy = strategies.value.find(s => s.id === id)
    if (strategy) {
      strategy.status = status
    }
  }

  // 策略日志相关
  async function fetchStrategyLogFiles(strategyId = '') {
    logsLoading.value = true
    try {
      const res = await strategyApi.getStrategyLogFiles(strategyId)
      strategyLogFiles.value = res.data || []
      return res
    } finally {
      logsLoading.value = false
    }
  }

  async function fetchStrategyLogs(filename, tailLines = 200) {
    logsLoading.value = true
    currentLogFilename.value = filename
    try {
      const res = await strategyApi.getStrategyLogs(filename, tailLines)
      if (res.success && res.data) {
        strategyLogs.value = res.data.lines || []
      }
      return res
    } finally {
      logsLoading.value = false
    }
  }

  // 刷新当前日志
  async function refreshCurrentLogs(tailLines = 200) {
    if (currentLogFilename.value) {
      return await fetchStrategyLogs(currentLogFilename.value, tailLines)
    }
  }

  return {
    // 状态
    strategies,
    currentStrategy,
    loading,
    strategyLogFiles,
    strategyLogs,
    logsLoading,
    currentLogFilename,
    firstLogDates,

    // 计算属性
    runningStrategies,
    stoppedStrategies,
    pendingStrategies,
    errorStrategies,

    // 操作
    fetchStrategies,
    fetchStrategyDetail,
    startStrategy,
    stopStrategy,
    createStrategy,
    deleteStrategy,
    fetchFirstLogDate,
    updateStrategyStatus,
    fetchStrategyConfigs,
    fetchStrategyLogFiles,
    fetchStrategyLogs,
    refreshCurrentLogs
  }
})

