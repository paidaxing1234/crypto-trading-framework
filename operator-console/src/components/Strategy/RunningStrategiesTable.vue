<template>
  <el-table :data="strategies" style="width: 100%">
    <el-table-column prop="name" label="策略名称" width="150">
      <template #default="{ row }">
        <div class="strategy-name">
          <el-icon color="#409eff"><Document /></el-icon>
          <span>{{ row.name }}</span>
        </div>
      </template>
    </el-table-column>
    
    <el-table-column prop="pnl" label="盈亏" width="120" align="right">
      <template #default="{ row }">
        <span :class="row.pnl >= 0 ? 'text-success' : 'text-danger'">
          {{ formatNumber(row.pnl, 2) }}
        </span>
      </template>
    </el-table-column>
    
    <el-table-column prop="returnRate" label="收益率" width="100" align="right">
      <template #default="{ row }">
        <span :class="row.returnRate >= 0 ? 'text-success' : 'text-danger'">
          {{ formatPercent(row.returnRate / 100) }}
        </span>
      </template>
    </el-table-column>
    
    <el-table-column prop="runTime" label="运行时长" width="120" />
  </el-table>
</template>

<script setup>
import { formatNumber, formatPercent } from '@/utils/format'
import { Document } from '@element-plus/icons-vue'

defineProps({
  strategies: {
    type: Array,
    default: () => []
  }
})
</script>

<style lang="scss" scoped>
.strategy-name {
  display: flex;
  align-items: center;
  gap: 8px;
}
</style>

