/**
 * 策略相关API
 * 通过 WebSocket 与后端通信
 */

import { wsClient } from '@/services/WebSocketClient'

// 响应等待器
const pendingRequests = new Map()

// 监听WebSocket响应
wsClient.on('response', (response) => {
  const { requestId, success, message, data } = response
  if (pendingRequests.has(requestId)) {
    const { resolve, reject } = pendingRequests.get(requestId)
    pendingRequests.delete(requestId)
    if (success) {
      // data 是整个 responseData 对象，真正的业务数据在 data.data 里
      resolve({ data: data?.data ?? data, success: true, message })
    } else {
      reject(new Error(message || '操作失败'))
    }
  }
})

// 生成请求ID
function generateRequestId() {
  return `strategy_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`
}

// 发送请求并等待响应
function sendRequest(action, data = {}, timeout = 10000) {
  return new Promise((resolve, reject) => {
    const requestId = generateRequestId()
    console.log(`[DEBUG] sendRequest: action=${action} requestId=${requestId}`)
    const timer = setTimeout(() => {
      pendingRequests.delete(requestId)
      console.error(`[DEBUG] sendRequest 超时: action=${action} requestId=${requestId}`)
      reject(new Error('请求超时'))
    }, timeout)

    pendingRequests.set(requestId, {
      resolve: (result) => { clearTimeout(timer); resolve(result) },
      reject: (error) => { clearTimeout(timer); reject(error) }
    })

    const sent = wsClient.send(action, { requestId, ...data })
    console.log(`[DEBUG] wsClient.send 返回: ${sent} action=${action}`)
  })
}

export const strategyApi = {
  /** 获取策略列表（从后端策略进程管理器获取） */
  async getStrategies(params) {
    try {
      return await sendRequest('list_strategies', {})
    } catch (error) {
      console.error('获取策略列表失败:', error)
      return { data: [], success: true }
    }
  },

  /** 获取策略详情 */
  async getStrategyDetail(id) {
    return { data: null, success: true }
  },

  /** 启动策略 */
  async startStrategy(id, config) {
    try {
      return await sendRequest('start_strategy', { strategy_id: id, config })
    } catch (error) {
      console.error('启动策略失败:', error)
      return { data: null, success: false, message: error.message }
    }
  },

  /** 停止策略 */
  async stopStrategy(id) {
    try {
      return await sendRequest('stop_strategy', { strategy_id: id })
    } catch (error) {
      console.error('停止策略失败:', error)
      return { data: null, success: false, message: error.message }
    }
  },

  /** 获取策略日志文件列表 */
  async getStrategyLogFiles(strategyId = '') {
    try {
      return await sendRequest('get_strategy_log_files', { strategyId })
    } catch (error) {
      console.error('获取策略日志文件列表失败:', error)
      return { data: [], success: false, message: error.message }
    }
  },

  /** 获取策略日志内容 */
  async getStrategyLogs(filename, tailLines = 200) {
    try {
      return await sendRequest('get_strategy_logs', { filename, tailLines }, 15000)
    } catch (error) {
      console.error('获取策略日志失败:', error)
      return { data: null, success: false, message: error.message }
    }
  },

  /** 获取系统日志文件列表 */
  async getSystemLogFiles() {
    try {
      return await sendRequest('get_system_log_files', {})
    } catch (error) {
      console.error('获取系统日志文件列表失败:', error)
      return { data: [], success: false, message: error.message }
    }
  },

  /** 获取系统日志内容 */
  async getSystemLogs(filename, tailLines = 200) {
    try {
      return await sendRequest('get_system_logs', { filename, tailLines }, 15000)
    } catch (error) {
      console.error('获取系统日志失败:', error)
      return { data: null, success: false, message: error.message }
    }
  },

  /** 获取账户持仓 */
  async getAccountPositions(accountId) {
    try {
      return await sendRequest('get_account_positions', { accountId })
    } catch (error) {
      console.error('获取账户持仓失败:', error)
      return { data: [], success: false, message: error.message }
    }
  },

  /** 获取最近订单（从策略日志解析） */
  async getRecentOrders(limit = 100) {
    try {
      return await sendRequest('get_recent_orders', { limit }, 15000)
    } catch (error) {
      console.error('获取最近订单失败:', error)
      return { data: [], success: false, message: error.message }
    }
  },

  /** 获取策略配置文件列表 */
  async listStrategyConfigs() {
    console.log('[DEBUG] listStrategyConfigs() 被调用')
    try {
      const result = await sendRequest('list_strategy_configs', {})
      console.log('[DEBUG] listStrategyConfigs() 成功:', result)
      return result
    } catch (error) {
      console.error('[DEBUG] listStrategyConfigs() 失败:', error)
      return { data: [], success: false, message: error.message }
    }
  },

  /** 创建策略 */
  async createStrategy(data) {
    try {
      return await sendRequest('create_strategy', data, 15000)
    } catch (error) {
      console.error('创建策略失败:', error)
      throw error
    }
  },

  /** 更新策略 */
  async updateStrategy(id, data) {
    console.log('Update strategy (WebSocket):', id, data)
    return { data: null, success: true }
  },

  /** 删除策略 */
  async deleteStrategy(id) {
    return await sendRequest('delete_strategy', {
      strategy_id: id
    })
  },

  /** 获取策略性能 */
  async fetchStrategyPerformance(id, timeRange) {
    return { data: null, success: true }
  },

  /** 列出策略源代码文件 */
  async listStrategyFiles() {
    try {
      return await sendRequest('list_strategy_files', {})
    } catch (error) {
      console.error('获取策略文件列表失败:', error)
      return { data: [], success: false, message: error.message }
    }
  },

  /** 获取策略源代码 */
  async getStrategySource(filename) {
    try {
      return await sendRequest('get_strategy_source', { filename }, 15000)
    } catch (error) {
      console.error('获取策略源代码失败:', error)
      return { data: null, success: false, message: error.message }
    }
  },

  /** 保存策略源代码 */
  async saveStrategySource(filename, content) {
    try {
      return await sendRequest('save_strategy_source', { filename, content }, 15000)
    } catch (error) {
      console.error('保存策略源代码失败:', error)
      return { data: null, success: false, message: error.message }
    }
  },

  /** 下载日志文件（完整内容） */
  async downloadLogFile(filename, logType = 'system') {
    try {
      return await sendRequest('download_log_file', { filename, logType }, 30000)
    } catch (error) {
      console.error('下载日志文件失败:', error)
      return { data: null, success: false, message: error.message }
    }
  }
}

export default strategyApi

