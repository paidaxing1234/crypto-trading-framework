import axios from 'axios'

// 统计 API 客户端 —— 走独立的 Python stats_api.py(只读 Redis 的净值/成交时序),
// 与交易主进程解耦。 默认同源路径 /stats-api(由 vite 代理 / nginx 转发到 :8003),
// 也可用 VITE_STATS_API_URL 覆盖为完整地址。
const baseURL = import.meta.env.VITE_STATS_API_URL || '/stats-api'

const http = axios.create({ baseURL, timeout: 12000 })

export const statsApi = {
  async accountsOverview() {
    const { data } = await http.get('/api/accounts_overview')
    return data
  },
  async accountStats(accountId) {
    const { data } = await http.get('/api/account_stats', { params: { account_id: accountId } })
    return data
  },
  async equityCurve(accountId, range = '30d') {
    const { data } = await http.get('/api/equity_curve', { params: { account_id: accountId, range } })
    return data
  },
  async slippageHistory(accountId, range = 'all') {
    const { data } = await http.get('/api/slippage_history', { params: { account_id: accountId, range } })
    return data
  },
  async health() {
    const { data } = await http.get('/api/health')
    return data
  }
}

export default statsApi
