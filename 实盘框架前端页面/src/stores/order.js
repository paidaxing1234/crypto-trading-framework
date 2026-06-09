import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { wsClient } from '@/services/WebSocketClient'
import { orderApi } from '@/api/order'

export const useOrderStore = defineStore('order', () => {
  // 状态
  const orders = ref([])
  const trades = ref([])
  const positions = ref([])
  const loading = ref(false)
  
  // 监听WebSocket快照（批量更新）
  if (typeof window !== 'undefined') {
    wsClient.on('snapshot', ({ data }) => {
      if (data.orders) {
        orders.value = data.orders
      }
      if (data.positions) {
        positions.value = data.positions
      }
    })
    
    // 监听订单成交事件（立即更新）
    wsClient.on('order_filled', ({ data }) => {
      const order = orders.value.find(o => o.id === data.order_id)
      if (order) {
        order.state = 'FILLED'
        order.filled_quantity = data.filled_quantity
        order.filled_price = data.filled_price
      }
    })
  }
  
  // 计算属性
  const activeOrders = computed(() => 
    orders.value.filter(o => 
      ['SUBMITTED', 'ACCEPTED', 'PARTIALLY_FILLED'].includes(o.state)
    )
  )
  
  const filledOrders = computed(() => 
    orders.value.filter(o => o.state === 'FILLED')
  )
  
  const cancelledOrders = computed(() => 
    orders.value.filter(o => o.state === 'CANCELLED')
  )
  
  const totalPositionValue = computed(() => 
    positions.value.reduce((sum, pos) => 
      sum + (pos.notionalValue || 0), 0
    )
  )
  
  const totalUnrealizedPnL = computed(() => 
    positions.value.reduce((sum, pos) => 
      sum + (pos.unrealizedPnl || 0), 0
    )
  )
  
  // 批量设置订单（从WebSocket快照）
  function setOrders(newOrders) {
    orders.value = newOrders || []
  }
  
  // 批量设置持仓（从WebSocket快照）
  function setPositions(newPositions) {
    positions.value = newPositions || []
  }
  
  // 操作（通过WebSocket命令）
  async function fetchOrders(params) {
    // 不需要主动fetch，WebSocket会推送
    // 这里保留兼容性
    return { data: orders.value }
  }
  
  async function fetchOrderDetail(id) {
    const res = await orderApi.getOrderDetail(id)
    return res
  }
  
  async function fetchTrades(params) {
    loading.value = true
    try {
      const res = await orderApi.getTrades(params)
      trades.value = res.data || []
      return res
    } finally {
      loading.value = false
    }
  }
  
  async function fetchPositions(params) {
    loading.value = true
    try {
      const res = await orderApi.getPositions(params)
      positions.value = res.data || []
      return res
    } finally {
      loading.value = false
    }
  }
  
  async function placeOrder(data) {
    // 发送命令到C++
    wsClient.send('place_order', data)
    
    // WebSocket会推送订单更新
    return { success: true }
  }
  
  async function cancelOrder(id) {
    // 发送命令到C++
    wsClient.send('cancel_order', { order_id: id })
    
    // WebSocket会推送更新
    return { success: true }
  }
  
  async function batchCancelOrders(ids) {
    const res = await orderApi.batchCancelOrders(ids)
    // 批量更新订单状态
    ids.forEach(id => {
      const order = orders.value.find(o => o.id === id)
      if (order) {
        order.state = 'CANCELLED'
      }
    })
    return res
  }
  
  function updateOrderStatus(id, state, data = {}) {
    const order = orders.value.find(o => o.id === id)
    if (order) {
      order.state = state
      Object.assign(order, data)
    }
  }
  
  function addOrder(order) {
    orders.value.unshift(order)
  }
  
  function addTrade(trade) {
    trades.value.unshift(trade)
  }
  
  function updatePosition(symbol, data) {
    const position = positions.value.find(p => p.symbol === symbol)
    if (position) {
      Object.assign(position, data)
    } else {
      positions.value.push(data)
    }
  }
  
  return {
    // 状态
    orders,
    trades,
    positions,
    loading,
    
    // 计算属性
    activeOrders,
    filledOrders,
    cancelledOrders,
    totalPositionValue,
    totalUnrealizedPnL,
    
    // 批量更新方法（WebSocket专用）
    setOrders,
    setPositions,
    
    // 操作
    fetchOrders,
    fetchOrderDetail,
    fetchTrades,
    fetchPositions,
    placeOrder,
    cancelOrder,
    batchCancelOrders,
    updateOrderStatus,
    addOrder,
    addTrade,
    updatePosition
  }
})

