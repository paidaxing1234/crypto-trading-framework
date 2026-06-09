import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { wsClient } from '@/services/WebSocketClient'

export const useLogStore = defineStore('log', () => {
  // 日志列表
  const logs = ref([])
  
  // 日志统计
  const stats = ref({
    total: 0,
    info: 0,
    warning: 0,
    error: 0,
    debug: 0
  })
  
  // 最大日志数量（防止内存溢出）- 降低以提升性能
  const MAX_LOGS = 1000
  
  // 初始化标志
  const initialized = ref(false)
  
  // 初始化WebSocket监听器
  function initWebSocketListeners() {
    if (initialized.value || typeof window === 'undefined') return
    
    try {
      // 监听日志事件
      wsClient.on('log', ({ data }) => {
        if (data && data.message) {
          addLog(data)
        }
      })
      
      // 监听快照中的日志 - 限制批量添加的数量
      wsClient.on('snapshot', ({ data }) => {
        if (data && data.logs && Array.isArray(data.logs)) {
          // 只处理最新的100条日志，避免一次性加载过多
          const recentLogs = data.logs.slice(-100)
          recentLogs.forEach(log => {
            if (log && log.message) {
              addLog(log, false)
            }
          })
          updateStats()
        }
      })
      
      // 监听其他事件产生的日志
      wsClient.on('order_filled', ({ data }) => {
        if (data && data.symbol) {
          addLog({
            level: 'info',
            source: 'order',
            message: `订单成交: ${data.symbol} ${data.side || ''} 成交数量: ${data.filled_quantity || 0}`,
            timestamp: Date.now(),
            data: data
          })
        }
      })
      
      initialized.value = true
      console.log('✅ Log store WebSocket listeners initialized')
    } catch (error) {
      console.error('❌ Failed to initialize log store listeners:', error)
    }
  }
  
  /**
   * 添加日志 - 优化性能，添加错误处理
   */
  function addLog(log, updateStat = true) {
    try {
      // 验证日志数据
      if (!log || typeof log !== 'object') {
        console.warn('Invalid log data:', log)
        return
      }
      
      // 标准化日志格式
      const normalizedLog = {
        id: log.id || `${Date.now()}-${Math.random()}`,
        level: log.level || 'info',
        source: log.source || 'system',
        message: String(log.message || ''),
        timestamp: log.timestamp || Date.now(),
        data: log.data || null
      }
      
      // 添加到列表开头（最新的在前）
      logs.value.unshift(normalizedLog)
      
      // 限制日志数量 - 直接裁剪而不是slice，性能更好
      if (logs.value.length > MAX_LOGS) {
        logs.value.length = MAX_LOGS
      }
      
      // 更新统计
      if (updateStat) {
        updateStats()
      }
    } catch (error) {
      console.error('Error adding log:', error, log)
    }
  }
  
  /**
   * 发送前端日志到后端
   */
  function sendLogToBackend(level, message, data = null) {
    // 先添加到前端日志列表
    addLog({
      level,
      source: 'frontend',
      message,
      data,
      timestamp: Date.now()
    })
    
    // 发送到后端
    if (typeof window !== 'undefined' && wsClient) {
      wsClient.sendLog(level, message, data)
    }
  }
  
  /**
   * 更新统计信息
   */
  function updateStats() {
    stats.value = {
      total: logs.value.length,
      info: logs.value.filter(log => log.level === 'info').length,
      warning: logs.value.filter(log => log.level === 'warning').length,
      error: logs.value.filter(log => log.level === 'error').length,
      debug: logs.value.filter(log => log.level === 'debug').length
    }
  }
  
  /**
   * 清空日志
   */
  function clearLogs() {
    logs.value = []
    updateStats()
  }
  
  /**
   * 根据条件筛选日志
   */
  function filterLogs(filters) {
    let filtered = [...logs.value]
    
    // 按级别筛选
    if (filters.level && filters.level !== 'all') {
      filtered = filtered.filter(log => log.level === filters.level)
    }
    
    // 按来源筛选
    if (filters.source && filters.source !== 'all') {
      filtered = filtered.filter(log => log.source === filters.source)
    }
    
    // 按关键词搜索
    if (filters.keyword) {
      const keyword = filters.keyword.toLowerCase()
      filtered = filtered.filter(log => 
        log.message.toLowerCase().includes(keyword)
      )
    }
    
    // 按时间范围筛选
    if (filters.dateRange && filters.dateRange.length === 2) {
      const [start, end] = filters.dateRange
      filtered = filtered.filter(log => {
        const logTime = new Date(log.timestamp).getTime()
        return logTime >= start.getTime() && logTime <= end.getTime()
      })
    }
    
    return filtered
  }
  
  /**
   * 从后端获取历史日志文件
   */
  async function fetchLogsFromFile(options = {}) {
    const { date = '', source = '', level = '', limit = 500, offset = 0 } = options

    try {
      const result = await wsClient.sendRequest('get_logs', {
        date,
        source,
        level,
        limit,
        offset
      })

      if (result.success && result.data) {
        return result.data
      }
      return { logs: [], total: 0 }
    } catch (error) {
      console.error('获取历史日志失败:', error)
      return { logs: [], total: 0 }
    }
  }

  /**
   * 获取可用的日志日期列表
   */
  async function fetchLogDates() {
    try {
      const result = await wsClient.sendRequest('get_log_dates', {})
      if (result.success && result.dates) {
        return result.dates
      }
      return []
    } catch (error) {
      console.error('获取日志日期列表失败:', error)
      return []
    }
  }

  /**
   * 导出日志
   */
  function exportLogs(filteredLogs = null) {
    const logsToExport = filteredLogs || logs.value
    const content = logsToExport.map(log => {
      const time = new Date(log.timestamp).toISOString()
      return `[${time}] [${log.level.toUpperCase()}] [${log.source}] ${log.message}`
    }).join('\n')
    
    // 创建下载
    const blob = new Blob([content], { type: 'text/plain' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `logs_${Date.now()}.txt`
    a.click()
    URL.revokeObjectURL(url)
  }
  
  // 计算属性
  const errorLogs = computed(() => 
    logs.value.filter(log => log.level === 'error')
  )
  
  const warningLogs = computed(() => 
    logs.value.filter(log => log.level === 'warning')
  )
  
  const recentLogs = computed(() => 
    logs.value.slice(0, 100)
  )
  
  // 获取所有日志来源
  const sources = computed(() => {
    const sourceSet = new Set(logs.value.map(log => log.source))
    return ['all', ...Array.from(sourceSet)]
  })
  
  return {
    // 状态
    logs,
    stats,
    initialized,
    
    // 计算属性
    errorLogs,
    warningLogs,
    recentLogs,
    sources,
    
    // 方法
    addLog,
    clearLogs,
    filterLogs,
    exportLogs,
    updateStats,
    sendLogToBackend,
    initWebSocketListeners,
    fetchLogsFromFile,
    fetchLogDates
  }
})


