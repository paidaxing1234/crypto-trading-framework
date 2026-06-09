<template>
  <el-dialog
    v-model="visible"
    :title="`订单详情 #${order?.id}`"
    width="700px"
  >
    <div class="order-detail" v-if="order">
      <el-descriptions :column="2" border>
        <el-descriptions-item label="订单ID">
          {{ order.id }}
        </el-descriptions-item>
        
        <el-descriptions-item label="交易所订单ID">
          {{ order.exchangeOrderId || '-' }}
        </el-descriptions-item>
        
        <el-descriptions-item label="交易对">
          {{ order.symbol }}
        </el-descriptions-item>
        
        <el-descriptions-item label="方向">
          <el-tag :type="order.side === 'BUY' ? 'success' : 'danger'">
            {{ formatOrderSide(order.side) }}
          </el-tag>
        </el-descriptions-item>
        
        <el-descriptions-item label="订单类型">
          {{ formatOrderType(order.type) }}
        </el-descriptions-item>
        
        <el-descriptions-item label="状态">
          <el-tag :type="getStateType(order.state)">
            {{ formatOrderState(order.state) }}
          </el-tag>
        </el-descriptions-item>
        
        <el-descriptions-item label="订单价格">
          {{ order.price ? formatNumber(order.price, 2) : '市价' }}
        </el-descriptions-item>
        
        <el-descriptions-item label="订单数量">
          {{ formatNumber(order.quantity, 4) }}
        </el-descriptions-item>
        
        <el-descriptions-item label="已成交数量">
          {{ formatNumber(order.filledQuantity || 0, 4) }}
        </el-descriptions-item>
        
        <el-descriptions-item label="成交价格">
          {{ order.filledPrice ? formatNumber(order.filledPrice, 2) : '-' }}
        </el-descriptions-item>
        
        <el-descriptions-item label="成交金额">
          {{ order.filledQuantity && order.filledPrice
            ? formatNumber(order.filledQuantity * order.filledPrice, 2)
            : '-'
          }}
        </el-descriptions-item>
        
        <el-descriptions-item label="手续费">
          {{ order.fee ? formatNumber(order.fee, 4) : '-' }}
        </el-descriptions-item>
        
        <el-descriptions-item label="创建时间" :span="2">
          {{ formatTime(order.timestamp) }}
        </el-descriptions-item>
        
        <el-descriptions-item label="更新时间" :span="2">
          {{ formatTime(order.updateTime) }}
        </el-descriptions-item>
        
        <el-descriptions-item label="备注" :span="2">
          {{ order.note || '-' }}
        </el-descriptions-item>
      </el-descriptions>
      
      <el-divider content-position="left">成交记录</el-divider>
      
      <el-table :data="order.trades || []" max-height="300">
        <el-table-column prop="tradeId" label="成交ID" width="150" />
        <el-table-column prop="price" label="成交价格" width="120" align="right">
          <template #default="{ row }">
            {{ formatNumber(row.price, 2) }}
          </template>
        </el-table-column>
        <el-table-column prop="quantity" label="成交数量" width="120" align="right">
          <template #default="{ row }">
            {{ formatNumber(row.quantity, 4) }}
          </template>
        </el-table-column>
        <el-table-column prop="fee" label="手续费" width="100" align="right">
          <template #default="{ row }">
            {{ formatNumber(row.fee, 4) }}
          </template>
        </el-table-column>
        <el-table-column prop="timestamp" label="时间" width="180">
          <template #default="{ row }">
            {{ formatTime(row.timestamp) }}
          </template>
        </el-table-column>
      </el-table>
      
      <el-empty v-if="!order.trades || order.trades.length === 0" description="暂无成交记录" />
    </div>
    
    <template #footer>
      <el-button @click="visible = false">关闭</el-button>
      <el-button
        v-if="canCancel"
        type="danger"
        @click="handleCancel"
        :loading="loading"
      >
        取消订单
      </el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { ref, computed } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useOrderStore } from '@/stores/order'
import {
  formatNumber,
  formatTime,
  formatOrderState,
  formatOrderSide,
  formatOrderType
} from '@/utils/format'

const props = defineProps({
  modelValue: Boolean,
  order: Object
})

const emit = defineEmits(['update:modelValue'])

const orderStore = useOrderStore()

const loading = ref(false)

const visible = computed({
  get: () => props.modelValue,
  set: (val) => emit('update:modelValue', val)
})

const canCancel = computed(() => {
  if (!props.order) return false
  return ['SUBMITTED', 'ACCEPTED', 'PARTIALLY_FILLED'].includes(props.order.state)
})

function getStateType(state) {
  const typeMap = {
    CREATED: 'info',
    SUBMITTED: 'info',
    ACCEPTED: 'warning',
    PARTIALLY_FILLED: 'warning',
    FILLED: 'success',
    CANCELLED: 'info',
    REJECTED: 'danger'
  }
  return typeMap[state] || 'info'
}

async function handleCancel() {
  try {
    await ElMessageBox.confirm(
      '确定要取消该订单吗?',
      '提示',
      {
        confirmButtonText: '确定',
        cancelButtonText: '取消',
        type: 'warning'
      }
    )
    
    loading.value = true
    await orderStore.cancelOrder(props.order.id)
    
    ElMessage.success('订单取消成功')
    visible.value = false
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('订单取消失败: ' + error.message)
    }
  } finally {
    loading.value = false
  }
}
</script>

<style lang="scss" scoped>
.order-detail {
  :deep(.el-divider__text) {
    font-weight: bold;
  }
}
</style>

