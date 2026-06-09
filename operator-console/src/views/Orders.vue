<template>
  <div class="orders-page">
    <!-- 页面头部 -->
    <div class="page-header">
      <div>
        <h2>订单管理</h2>
        <p>查看和管理所有交易订单</p>
      </div>
      <el-button 
        type="primary" 
        :icon="Plus" 
        @click="showPlaceOrderDialog = true"
        v-permission="'order:create'"
      >
        手动下单
      </el-button>
    </div>
    
    <!-- 订单统计 -->
    <el-row :gutter="20" class="stats-row">
      <el-col :span="6">
        <el-card class="stat-mini">
          <el-statistic title="活跃订单" :value="activeOrders.length">
            <template #prefix>
              <el-icon><Clock /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
      
      <el-col :span="6">
        <el-card class="stat-mini">
          <el-statistic title="已成交" :value="filledOrders.length">
            <template #prefix>
              <el-icon style="color: #67c23a;"><SuccessFilled /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
      
      <el-col :span="6">
        <el-card class="stat-mini">
          <el-statistic title="已取消" :value="cancelledOrders.length">
            <template #prefix>
              <el-icon style="color: #f56c6c;"><CircleClose /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
      
      <el-col :span="6">
        <el-card class="stat-mini">
          <el-statistic title="今日成交额" :value="todayVolume" :precision="2" suffix="USDT">
            <template #prefix>
              <el-icon><Money /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
    </el-row>
    
    <!-- 筛选和搜索 -->
    <el-card class="filter-card">
      <el-form :inline="true" :model="filters">
        <el-form-item label="交易对">
          <el-input v-model="filters.symbol" placeholder="BTC-USDT" clearable />
        </el-form-item>
        
        <el-form-item label="状态">
          <el-select v-model="filters.state" placeholder="全部" clearable>
            <el-option label="已创建" value="CREATED" />
            <el-option label="已提交" value="SUBMITTED" />
            <el-option label="已接受" value="ACCEPTED" />
            <el-option label="部分成交" value="PARTIALLY_FILLED" />
            <el-option label="完全成交" value="FILLED" />
            <el-option label="已取消" value="CANCELLED" />
            <el-option label="已拒绝" value="REJECTED" />
          </el-select>
        </el-form-item>
        
        <el-form-item label="订单类型">
          <el-select v-model="filters.type" placeholder="全部" clearable>
            <el-option label="限价单" value="LIMIT" />
            <el-option label="市价单" value="MARKET" />
            <el-option label="止损单" value="STOP" />
          </el-select>
        </el-form-item>
        
        <el-form-item label="方向">
          <el-select v-model="filters.side" placeholder="全部" clearable>
            <el-option label="买入" value="BUY" />
            <el-option label="卖出" value="SELL" />
          </el-select>
        </el-form-item>
        
        <el-form-item label="时间范围">
          <el-date-picker
            v-model="filters.dateRange"
            type="daterange"
            range-separator="至"
            start-placeholder="开始日期"
            end-placeholder="结束日期"
          />
        </el-form-item>
        
        <el-form-item>
          <el-button type="primary" :icon="Search" @click="handleSearch">
            搜索
          </el-button>
          <el-button @click="handleReset">重置</el-button>
          <Permission :permission="'order:cancel'">
            <el-button type="danger" @click="handleBatchCancel" :disabled="selectedOrders.length === 0">
              批量取消
            </el-button>
          </Permission>
          <el-button :icon="Download" @click="handleExport">
            导出
          </el-button>
        </el-form-item>
      </el-form>
    </el-card>
    
    <!-- 订单列表 -->
    <el-card>
      <el-table
        :data="orders"
        v-loading="loading"
        @selection-change="handleSelectionChange"
      >
        <el-table-column type="selection" width="55" />
        
        <el-table-column prop="id" label="订单ID" width="100" />
        
        <el-table-column prop="symbol" label="交易对" width="130" />
        
        <el-table-column prop="side" label="方向" width="80">
          <template #default="{ row }">
            <el-tag :type="row.side === 'BUY' ? 'success' : 'danger'">
              {{ formatOrderSide(row.side) }}
            </el-tag>
          </template>
        </el-table-column>
        
        <el-table-column prop="type" label="类型" width="100">
          <template #default="{ row }">
            {{ formatOrderType(row.type) }}
          </template>
        </el-table-column>
        
        <el-table-column prop="price" label="价格" width="120" align="right">
          <template #default="{ row }">
            {{ row.price ? formatNumber(row.price, 2) : '-' }}
          </template>
        </el-table-column>
        
        <el-table-column prop="quantity" label="数量" width="120" align="right">
          <template #default="{ row }">
            {{ formatNumber(row.quantity, 4) }}
          </template>
        </el-table-column>
        
        <el-table-column prop="filledQuantity" label="已成交" width="120" align="right">
          <template #default="{ row }">
            {{ formatNumber(row.filledQuantity || 0, 4) }}
          </template>
        </el-table-column>
        
        <el-table-column prop="state" label="状态" width="120">
          <template #default="{ row }">
            <el-tag :type="getOrderStateType(row.state)">
              {{ formatOrderState(row.state) }}
            </el-tag>
          </template>
        </el-table-column>
        
        <el-table-column prop="timestamp" label="时间" width="180">
          <template #default="{ row }">
            {{ formatTime(row.timestamp) }}
          </template>
        </el-table-column>
        
        <el-table-column label="操作" width="150" fixed="right">
          <template #default="{ row }">
            <Permission :permission="'order:cancel'">
              <el-button
                v-if="canCancel(row.state)"
                type="danger"
                size="small"
                @click="handleCancel(row)"
              >
                取消
              </el-button>
            </Permission>
            <el-button
              type="primary"
              size="small"
              @click="handleViewDetail(row)"
            >
              详情
            </el-button>
          </template>
        </el-table-column>
      </el-table>
      
      <el-pagination
        v-model:current-page="pagination.page"
        v-model:page-size="pagination.pageSize"
        :total="pagination.total"
        :page-sizes="[10, 20, 50, 100]"
        layout="total, sizes, prev, pager, next, jumper"
        @size-change="handleSizeChange"
        @current-change="handlePageChange"
      />
    </el-card>
    
    <!-- 下单对话框 -->
    <place-order-dialog
      v-model="showPlaceOrderDialog"
      @success="handlePlaceOrderSuccess"
    />
    
    <!-- 订单详情对话框 -->
    <order-detail-dialog
      v-model="showDetailDialog"
      :order="selectedOrder"
    />
  </div>
</template>

<script setup>
import { ref, computed, onMounted, reactive } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useOrderStore } from '@/stores/order'
import {
  formatNumber,
  formatTime,
  formatOrderState,
  formatOrderSide,
  formatOrderType
} from '@/utils/format'
import {
  Plus,
  Search,
  Download,
  Clock,
  SuccessFilled,
  CircleClose,
  Money
} from '@element-plus/icons-vue'

import PlaceOrderDialog from '@/components/Order/PlaceOrderDialog.vue'
import OrderDetailDialog from '@/components/Order/OrderDetailDialog.vue'

const orderStore = useOrderStore()

const showPlaceOrderDialog = ref(false)
const showDetailDialog = ref(false)
const selectedOrder = ref(null)
const selectedOrders = ref([])

const filters = reactive({
  symbol: '',
  state: '',
  type: '',
  side: '',
  dateRange: null
})

const pagination = reactive({
  page: 1,
  pageSize: 20,
  total: 0
})

const todayVolume = ref(125680.50)

const loading = computed(() => orderStore.loading)
const orders = computed(() => orderStore.orders)
const activeOrders = computed(() => orderStore.activeOrders)
const filledOrders = computed(() => orderStore.filledOrders)
const cancelledOrders = computed(() => orderStore.cancelledOrders)

function getOrderStateType(state) {
  const typeMap = {
    CREATED: '',
    SUBMITTED: '',
    ACCEPTED: 'warning',
    PARTIALLY_FILLED: 'warning',
    FILLED: 'success',
    CANCELLED: '',
    REJECTED: 'danger'
  }
  return typeMap[state] || ''
}

function canCancel(state) {
  return ['SUBMITTED', 'ACCEPTED', 'PARTIALLY_FILLED'].includes(state)
}

async function handleSearch() {
  await orderStore.fetchOrders({
    ...filters,
    page: pagination.page,
    pageSize: pagination.pageSize
  })
}

function handleReset() {
  Object.assign(filters, {
    symbol: '',
    state: '',
    type: '',
    side: '',
    dateRange: null
  })
  handleSearch()
}

async function handleCancel(row) {
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
    
    await orderStore.cancelOrder(row.id)
    ElMessage.success('订单取消成功')
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('订单取消失败: ' + error.message)
    }
  }
}

async function handleBatchCancel() {
  try {
    await ElMessageBox.confirm(
      `确定要取消选中的 ${selectedOrders.value.length} 个订单吗?`,
      '批量取消',
      {
        confirmButtonText: '确定',
        cancelButtonText: '取消',
        type: 'warning'
      }
    )
    
    const ids = selectedOrders.value.map(o => o.id)
    await orderStore.batchCancelOrders(ids)
    ElMessage.success('批量取消成功')
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('批量取消失败: ' + error.message)
    }
  }
}

function handleViewDetail(row) {
  selectedOrder.value = row
  showDetailDialog.value = true
}

function handleSelectionChange(selection) {
  selectedOrders.value = selection
}

function handleSizeChange() {
  handleSearch()
}

function handlePageChange() {
  handleSearch()
}

function handleExport() {
  ElMessage.info('导出功能开发中...')
}

function handlePlaceOrderSuccess() {
  ElMessage.success('订单提交成功')
  handleSearch()
}

onMounted(() => {
  handleSearch()
})
</script>

<style lang="scss" scoped>
.orders-page {
  .page-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 20px;

    h2 { margin: 0 0 5px 0; color: var(--text-primary); font-weight: 700; }
    p { margin: 0; color: var(--text-secondary); font-size: 13px; }
  }

  .stats-row {
    margin-bottom: 20px;
    .stat-mini {
      :deep(.el-card__body) { padding: 20px; }
      :deep(.el-statistic__head) { color: var(--text-secondary) !important; font-size: 12px; text-transform: uppercase; letter-spacing: 0.5px; }
      :deep(.el-statistic__content) { font-family: var(--font-mono); }
      :deep(.el-statistic__number) { color: var(--text-primary) !important; }
    }
  }

  .filter-card {
    margin-bottom: 20px;
    :deep(.el-card__body) { padding: 15px 20px; }
    :deep(.el-form-item__label) { color: var(--text-muted) !important; font-size: 12px; }
  }

  :deep(.el-date-editor) {
    --el-date-editor-bg-color: var(--bg-input) !important;
  }

  :deep(.el-pagination) {
    margin-top: 20px;
    justify-content: flex-end;
  }
}
</style>

