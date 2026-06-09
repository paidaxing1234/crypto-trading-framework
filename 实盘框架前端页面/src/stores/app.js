import { defineStore } from 'pinia'
import { ref } from 'vue'
import { wsClient } from '@/services/WebSocketClient'

export const useAppStore = defineStore('app', () => {
  // 状态
  const sidebarCollapsed = ref(false)
  const theme = ref('light')
  const language = ref('zh-CN')
  
  // WebSocket连接状态
  const wsConnected = ref(false)
  const wsReconnecting = ref(false)
  const wsLatency = ref(0)
  
  // 监听WebSocket连接状态
  if (typeof window !== 'undefined') {
    wsClient.on('connected', () => {
      wsConnected.value = true
      wsReconnecting.value = false
    })
    
    wsClient.on('disconnected', () => {
      wsConnected.value = false
      wsReconnecting.value = true
    })
    
    wsClient.on('snapshot', ({ latency }) => {
      wsLatency.value = latency
    })
  }
  
  // 系统通知
  const notifications = ref([])
  
  // 操作
  function toggleSidebar() {
    sidebarCollapsed.value = !sidebarCollapsed.value
  }
  
  function setSidebarCollapsed(collapsed) {
    sidebarCollapsed.value = collapsed
  }
  
  function setTheme(newTheme) {
    theme.value = newTheme
    // 应用主题
    if (newTheme === 'dark') {
      document.documentElement.classList.add('dark')
    } else {
      document.documentElement.classList.remove('dark')
    }
    localStorage.setItem('theme', newTheme)
  }
  
  function setLanguage(lang) {
    language.value = lang
    localStorage.setItem('language', lang)
  }
  
  function setWsConnected(connected) {
    wsConnected.value = connected
  }
  
  function setWsReconnecting(reconnecting) {
    wsReconnecting.value = reconnecting
  }
  
  function addNotification(notification) {
    notifications.value.unshift({
      id: Date.now(),
      timestamp: new Date(),
      ...notification
    })
    
    // 保留最近100条
    if (notifications.value.length > 100) {
      notifications.value = notifications.value.slice(0, 100)
    }
  }
  
  function removeNotification(id) {
    notifications.value = notifications.value.filter(n => n.id !== id)
  }
  
  function clearNotifications() {
    notifications.value = []
  }
  
  // 初始化
  function init() {
    // 从本地存储恢复主题
    const savedTheme = localStorage.getItem('theme')
    if (savedTheme) {
      setTheme(savedTheme)
    }
    
    // 从本地存储恢复语言
    const savedLanguage = localStorage.getItem('language')
    if (savedLanguage) {
      setLanguage(savedLanguage)
    }
  }
  
  return {
    // 状态
    sidebarCollapsed,
    theme,
    language,
    wsConnected,
    wsReconnecting,
    notifications,
    
    // 操作
    toggleSidebar,
    setSidebarCollapsed,
    setTheme,
    setLanguage,
    setWsConnected,
    setWsReconnecting,
    addNotification,
    removeNotification,
    clearNotifications,
    init
  }
})

