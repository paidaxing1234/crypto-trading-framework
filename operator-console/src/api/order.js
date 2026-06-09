/**
 * 订单相关API
 * 通过WebSocket获取数据，这里提供的是占位API
 */

export const orderApi = {
  /**
   * 获取订单列表
   */
  async getOrders(params) {
    return { data: [], success: true }
  },

  /**
   * 获取订单详情
   */
  async getOrderDetail(id) {
    return { data: null, success: true }
  },

  /**
   * 下单
   */
  async placeOrder(data) {
    console.log('Place order (WebSocket):', data)
    return { data: null, success: true }
  },

  /**
   * 撤单
   */
  async cancelOrder(id) {
    console.log('Cancel order (WebSocket):', id)
    return { data: null, success: true }
  },

  /**
   * 批量撤单
   */
  async cancelOrders(ids) {
    console.log('Cancel orders (WebSocket):', ids)
    return { data: null, success: true }
  },

  /**
   * 获取持仓
   */
  async getPositions(params) {
    return { data: [], success: true }
  },

  /**
   * 获取成交记录
   */
  async getTrades(params) {
    return { data: [], success: true }
  },

  /**
   * 批量撤单
   */
  async batchCancelOrders(ids) {
    console.log('Batch cancel orders (WebSocket):', ids)
    return { data: null, success: true }
  },

  /**
   * 平仓
   */
  async closePosition(data) {
    console.log('Close position (WebSocket):', data)
    return { data: null, success: true }
  }
}

export default orderApi

