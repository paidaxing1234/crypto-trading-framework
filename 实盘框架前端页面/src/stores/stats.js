import { defineStore } from 'pinia'
import { ref } from 'vue'
import { statsApi } from '@/utils/statsApi'

// 账户/策略统计 store —— 数据来自独立的 stats_api(:8003), 与 WebSocket 解耦。
// overview: { account_id -> { equity, available, wallet, upnl, pnl, return_rate, trade_count, ... } }
export const useStatsStore = defineStore('stats', () => {
  const overview = ref({})
  const reachable = ref(true)     // 统计服务是否可达(不可达时前端优雅降级)

  async function fetchOverview() {
    try {
      const data = await statsApi.accountsOverview()
      const m = {}
      for (const a of (data.accounts || [])) {
        if (a && a.account_id) m[a.account_id] = a
      }
      overview.value = m
      reachable.value = true
      return m
    } catch (e) {
      reachable.value = false
      console.warn('[stats] 统计服务不可达:', e?.message || e)
      return overview.value
    }
  }

  function statOf(accountId) {
    return overview.value[accountId] || {}
  }

  return { overview, reachable, fetchOverview, statOf }
})
