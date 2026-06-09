/**
 * WebSocket客户端 - 符合Kungfu架构
 * 直接连接C++ UI服务器，接收共享内存快照
 * 
 * 性能：
 * - 快照频率：100ms
 * - 延迟：1-4ms
 * - 比HTTP/SSE快5-10倍
 */

import { ElMessage, ElNotification } from 'element-plus'

class WebSocketClient {
  constructor() {
    this.ws = null
    this.connected = false
    this.reconnectAttempts = 0
    this.maxReconnectAttempts = 10
    this.reconnectDelay = 1000
    this.listeners = new Map()
    
    // 性能监控
    this.lastSnapshotTime = 0
    this.snapshotCount = 0
    this.avgLatency = 0
    
    // Mock模式
    this.mockMode = false
    this.mockTimer = null
  }
  
  /**
   * 连接C++ UI服务器
   */
  connect() {
    // 优先使用环境变量，否则使用后端服务器地址（端口8002）
    const wsUrl = import.meta.env.VITE_WS_URL || 'ws://localhost:8002'

    console.log('🔌 连接C++ UI服务器:', wsUrl)
    
    try {
      this.ws = new WebSocket(wsUrl)
      
      // 设置超时，如果5秒内没连上，启用Mock模式
      const timeout = setTimeout(() => {
        if (!this.connected) {
          console.warn('⚠️ WebSocket连接超时，启用Mock模式')
          this.enableMockMode()
        }
      }, 5000)
      
      // 连接打开
      this.ws.onopen = () => {
        clearTimeout(timeout)
        console.log('✅ WebSocket连接已建立')
        this.connected = true
        this.reconnectAttempts = 0
        this.mockMode = false
        
        ElNotification({
          title: '连接成功',
          message: 'C++ UI服务器已连接',
          type: 'success',
          duration: 2000
        })
        
        this.emit('connected', { timestamp: Date.now() })
        
        // 发送认证（如果需要）
        const token = localStorage.getItem('token')
        if (token) {
          this.send('auth', { token })
        }
      }
      
      // 接收消息
      this.ws.onmessage = (event) => {
        try {
          const message = JSON.parse(event.data)
          this.handleMessage(message)
        } catch (error) {
          console.error('消息解析失败:', error)
        }
      }
      
      // 连接关闭
      this.ws.onclose = () => {
        console.log('🔌 WebSocket连接关闭')
        this.connected = false
        this.emit('disconnected', { timestamp: Date.now() })
        
        // 自动重连
        this.handleReconnect()
      }
      
      // 连接错误
      this.ws.onerror = (error) => {
        console.error('❌ WebSocket错误:', error)
      }
      
    } catch (error) {
      console.error('创建WebSocket失败:', error)
      this.handleReconnect()
    }
  }
  
  /**
   * 处理消息 - 添加错误处理
   */
  handleMessage(message) {
    try {
      if (!message || typeof message !== 'object') {
        console.warn('Invalid message:', message)
        return
      }
      
      const { type, timestamp } = message
      
      // 计算延迟
      const now = Date.now()
      const latency = now - (timestamp || now)
      this.updateLatency(latency)
      
      if (type === 'snapshot') {
        // 完整快照（100ms一次）
        if (message.data) {
          this.handleSnapshot(message.data, latency)
        }
      } else if (type === 'event') {
        // 增量事件（立即推送）
        if (message.event_type && message.data) {
          this.handleEvent(message.event_type, message.data, latency)
        }
      } else if (type === 'log') {
        // 日志消息（来自C++）
        if (message.data) {
          this.handleLogMessage(message.data, latency)
        }
      } else if (type === 'response') {
        // 命令响应
        this.handleResponse(message)
      }
    } catch (error) {
      console.error('Error handling message:', error, message)
    }
  }
  
  /**
   * 处理日志消息 - 添加验证和限流
   */
  handleLogMessage(data, latency) {
    try {
      // 验证数据
      if (!data || !data.message) {
        return
      }
      
      // 触发日志事件
      this.emit('log', {
        data: {
          level: data.level || 'info',
          source: data.source || 'backend',
          message: String(data.message),
          timestamp: data.timestamp || Date.now(),
          data: data.extra || null
        },
        latency,
        timestamp: Date.now()
      })
      
      // 如果是错误日志，显示通知（限制频率）
      if (data.level === 'error' && !this.errorNotificationTimeout) {
        ElNotification({
          title: '系统错误',
          message: String(data.message).substring(0, 100),
          type: 'error',
          duration: 3000
        })
        
        // 限制错误通知频率：5秒内只显示一次
        this.errorNotificationTimeout = setTimeout(() => {
          this.errorNotificationTimeout = null
        }, 5000)
      }
    } catch (error) {
      console.error('Error handling log message:', error)
    }
  }
  
  /**
   * 处理快照（批量更新）- 添加数据验证
   */
  handleSnapshot(data, latency) {
    try {
      this.snapshotCount++
      this.lastSnapshotTime = Date.now()
      
      // 验证数据
      if (!data || typeof data !== 'object') {
        console.warn('Invalid snapshot data')
        return
      }
      
      // 触发快照事件（Store会监听）
      this.emit('snapshot', {
        data,
        latency,
        timestamp: this.lastSnapshotTime
      })
      
      // 如果延迟过高，警告（降低阈值到100ms）
      if (latency > 100) {
        console.warn(`⚠️ 快照延迟过高: ${latency}ms`)
      }
    } catch (error) {
      console.error('Error handling snapshot:', error)
    }
  }
  
  /**
   * 处理增量事件
   */
  handleEvent(eventType, data, latency) {
    console.log(`📨 收到事件 [${eventType}], 延迟: ${latency}ms`)
    
    // 触发特定事件
    this.emit(eventType, {
      data,
      latency,
      timestamp: Date.now()
    })
    
    // 关键事件通知用户
    if (eventType === 'order_filled') {
      ElNotification({
        title: '订单成交',
        message: `${data.symbol} ${data.side} 已成交`,
        type: 'success'
      })
    }
    
    // 日志事件单独处理
    if (eventType === 'log') {
      this.emit('log', {
        data,
        latency,
        timestamp: Date.now()
      })
    }
  }
  
  /**
   * 处理命令响应
   */
  handleResponse(message) {
    // 获取完整的响应数据
    const responseData = message.data || message
    const { requestId, success, message: msg, type } = responseData

    console.log('[WS] handleResponse:', { requestId, success, msg, type, responseData })

    // 如果有requestId，触发response事件（用于API调用）
    if (requestId) {
      this.emit('response', {
        requestId,
        success,
        message: msg,
        data: responseData  // 传递完整的响应数据，包含 token, user 等
      })
      return
    }

    // 特殊处理登录响应（没有requestId但有type）
    if (type === 'login_response') {
      this.emit('response', {
        success,
        message: msg,
        data: responseData
      })
      return
    }

    // 否则显示消息提示（兼容旧代码）
    if (success) {
      ElMessage.success(msg || '操作成功')
    } else {
      ElMessage.error(msg || '操作失败')
    }
  }
  
  /**
   * 发送命令到C++
   */
  send(action, data) {
    if (this.mockMode) {
      console.warn('Mock模式下无法发送命令:', action)
      return false
    }
    if (!this.connected || !this.ws || this.ws.readyState !== WebSocket.OPEN) {
      console.warn('WebSocket未连接，无法发送消息:', action)
      return false
    }

    const message = {
      action,
      data,
      timestamp: Date.now()
    }

    try {
      this.ws.send(JSON.stringify(message))
      console.log(`📤 发送命令: ${action}`, data)
      return true
    } catch (error) {
      console.error('发送命令失败:', error)
      return false
    }
  }
  
  /**
   * 发送前端日志到后端
   */
  sendLog(level, message, data = null) {
    return this.send('frontend_log', {
      level,
      message,
      data,
      source: 'frontend',
      timestamp: Date.now()
    })
  }
  
  /**
   * 注册监听器
   */
  on(eventType, callback) {
    if (!this.listeners.has(eventType)) {
      this.listeners.set(eventType, [])
    }
    this.listeners.get(eventType).push(callback)
  }
  
  /**
   * 取消监听
   */
  off(eventType, callback) {
    if (!this.listeners.has(eventType)) return
    
    const callbacks = this.listeners.get(eventType)
    const index = callbacks.indexOf(callback)
    if (index > -1) {
      callbacks.splice(index, 1)
    }
  }
  
  /**
   * 触发事件
   */
  emit(eventType, data) {
    const callbacks = this.listeners.get(eventType) || []
    callbacks.forEach(callback => {
      try {
        callback(data)
      } catch (error) {
        console.error(`事件处理错误 [${eventType}]:`, error)
      }
    })
  }
  
  /**
   * 处理重连
   */
  handleReconnect() {
    if (this.reconnectAttempts >= this.maxReconnectAttempts) {
      console.error('❌ 达到最大重连次数')
      ElMessage.error('无法连接到C++服务器，请检查服务是否启动')
      return
    }
    
    this.reconnectAttempts++
    const delay = this.reconnectDelay * Math.pow(2, this.reconnectAttempts - 1)
    
    console.log(`⏳ ${delay/1000}秒后重连... (${this.reconnectAttempts}/${this.maxReconnectAttempts})`)
    
    setTimeout(() => {
      this.connect()
    }, delay)
  }
  
  /**
   * 更新延迟统计
   */
  updateLatency(latency) {
    // 移动平均
    this.avgLatency = this.avgLatency * 0.9 + latency * 0.1
  }
  
  /**
   * 获取统计信息
   */
  getStats() {
    return {
      connected: this.connected,
      snapshotCount: this.snapshotCount,
      avgLatency: this.avgLatency.toFixed(2),
      lastSnapshotTime: this.lastSnapshotTime
    }
  }
  
  /**
   * 断开连接
   */
  disconnect() {
    if (this.ws) {
      this.ws.close()
      this.ws = null
    }
    this.connected = false
    this.disableMockMode()
  }
  
  /**
   * 启用Mock模式 - 前端独立运行
   */
  enableMockMode() {
    if (this.mockMode) return
    
    console.log('🎭 启用Mock模式 - 使用模拟数据')
    this.mockMode = true
    this.connected = true
    
    ElNotification({
      title: 'Mock模式',
      message: '后端未连接，使用模拟数据运行',
      type: 'warning',
      duration: 3000
    })
    
    this.emit('connected', { timestamp: Date.now() })
    
    // 定期推送模拟快照数据
    this.mockTimer = setInterval(() => {
      this.pushMockSnapshot()
    }, 1000) // 每秒推送一次
  }
  
  /**
   * 禁用Mock模式
   */
  disableMockMode() {
    if (this.mockTimer) {
      clearInterval(this.mockTimer)
      this.mockTimer = null
    }
    this.mockMode = false
  }
  
  /**
   * 推送模拟快照数据
   */
  pushMockSnapshot() {
    const mockData = {
      orders: [],
      positions: [],
      strategies: []
    }
    
    this.emit('snapshot', {
      data: mockData,
      latency: 0,
      timestamp: Date.now()
    })
  }
}

// 全局单例
export const wsClient = new WebSocketClient()

// Vue插件
export default {
  install(app) {
    app.config.globalProperties.$ws = wsClient
    app.provide('wsClient', wsClient)
  }
}

