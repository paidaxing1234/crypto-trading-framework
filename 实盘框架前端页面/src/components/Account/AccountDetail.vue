<template>
  <div class="account-detail">
    <el-descriptions :column="2" border>
      <el-descriptions-item label="策略ID">
        {{ account.strategy_id || account.id || '--' }}
      </el-descriptions-item>

      <el-descriptions-item label="账户ID">
        {{ account.account_id || '--' }}
      </el-descriptions-item>

      <el-descriptions-item label="交易所">
        <el-tag :type="account.exchange === 'okx' ? 'primary' : 'success'" size="small">
          {{ (account.exchange || 'okx').toUpperCase() }}
        </el-tag>
      </el-descriptions-item>

      <el-descriptions-item label="API Key">
        {{ account.api_key || maskApiKey(account.apiKey) || '--' }}
      </el-descriptions-item>

      <el-descriptions-item label="环境">
        <el-tag :type="account.is_testnet || account.isTestnet ? 'warning' : 'success'" size="small">
          {{ account.is_testnet || account.isTestnet ? '模拟盘' : '实盘' }}
        </el-tag>
      </el-descriptions-item>

      <el-descriptions-item label="状态">
        <el-tag :type="account.status === 'ACTIVE' ? 'success' : 'danger'" size="small">
          {{ statusMap[account.status] || account.status || '未知' }}
        </el-tag>
      </el-descriptions-item>

      <el-descriptions-item label="注册时间">
        {{ account.register_time ? formatTime(account.register_time) : '--' }}
      </el-descriptions-item>

      <el-descriptions-item label="净值 (USDT)">
        {{ formatNumber(account.equity, 2) }}
      </el-descriptions-item>

      <el-descriptions-item label="未实现盈亏 (USDT)">
        <span :class="(account.unrealizedPnl || 0) >= 0 ? 'text-success' : 'text-danger'">
          {{ formatNumber(account.unrealizedPnl, 2) }}
        </span>
      </el-descriptions-item>
    </el-descriptions>
  </div>
</template>

<script setup>
import { formatNumber, formatTime } from '@/utils/format'

defineProps({
  account: {
    type: Object,
    required: true
  }
})

const statusMap = {
  'ACTIVE': '正常',
  'DISABLED': '已禁用',
  'ERROR': '异常',
  'RATE_LIMITED': '限流'
}

function maskApiKey(apiKey) {
  if (!apiKey) return ''
  const len = apiKey.length
  if (len <= 8) return apiKey
  return apiKey.substring(0, 4) + '****' + apiKey.substring(len - 4)
}
</script>

<style lang="scss" scoped>
.account-detail {
  padding: 20px;
}
</style>
