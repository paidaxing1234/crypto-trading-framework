<template>
  <el-table :data="orders" style="width: 100%">
    <el-table-column prop="symbol" label="交易对" width="130" />
    
    <el-table-column prop="side" label="方向" width="80">
      <template #default="{ row }">
        <el-tag :type="row.side === 'BUY' ? 'success' : 'danger'" size="small">
          {{ formatOrderSide(row.side) }}
        </el-tag>
      </template>
    </el-table-column>
    
    <el-table-column prop="price" label="价格" width="100" align="right">
      <template #default="{ row }">
        {{ row.price ? formatNumber(row.price, 2) : '-' }}
      </template>
    </el-table-column>
    
    <el-table-column prop="quantity" label="数量" width="100" align="right">
      <template #default="{ row }">
        {{ formatNumber(row.quantity, 4) }}
      </template>
    </el-table-column>
    
    <el-table-column prop="state" label="状态" width="100">
      <template #default="{ row }">
        <el-tag :type="getStateType(row.state)" size="small">
          {{ formatOrderState(row.state) }}
        </el-tag>
      </template>
    </el-table-column>
  </el-table>
</template>

<script setup>
import { formatNumber, formatOrderState, formatOrderSide } from '@/utils/format'

defineProps({
  orders: {
    type: Array,
    default: () => []
  }
})

function getStateType(state) {
  const typeMap = {
    FILLED: 'success',
    PARTIALLY_FILLED: 'warning',
    CANCELLED: 'info',
    REJECTED: 'danger'
  }
  return typeMap[state] || 'info'
}
</script>

