<template>
  <el-dialog
    v-model="visible"
    :title="`${account?.name} - 净值曲线`"
    width="900px"
  >
    <div class="equity-curve-dialog">
      <div class="time-range-selector">
        <el-radio-group v-model="timeRange" @change="fetchData">
          <el-radio-button label="1d">1天</el-radio-button>
          <el-radio-button label="7d">7天</el-radio-button>
          <el-radio-button label="30d">30天</el-radio-button>
          <el-radio-button label="90d">90天</el-radio-button>
          <el-radio-button label="all">全部</el-radio-button>
        </el-radio-group>
      </div>
      
      <div class="chart-container" v-loading="loading">
        <equity-chart
          v-if="account"
          :account-id="account.id"
          :time-range="timeRange"
          height="400px"
        />
      </div>
      
      <el-divider />
      
      <div class="statistics">
        <el-row :gutter="20">
          <el-col :span="6">
            <div class="stat-item">
              <div class="stat-label">初始净值</div>
              <div class="stat-value">{{ formatNumber(stats.initialEquity, 2) }}</div>
            </div>
          </el-col>
          
          <el-col :span="6">
            <div class="stat-item">
              <div class="stat-label">当前净值</div>
              <div class="stat-value">{{ formatNumber(stats.currentEquity, 2) }}</div>
            </div>
          </el-col>
          
          <el-col :span="6">
            <div class="stat-item">
              <div class="stat-label">累计收益</div>
              <div class="stat-value" :class="stats.totalReturn >= 0 ? 'text-success' : 'text-danger'">
                {{ formatNumber(stats.totalReturn, 2) }}
              </div>
            </div>
          </el-col>
          
          <el-col :span="6">
            <div class="stat-item">
              <div class="stat-label">收益率</div>
              <div class="stat-value" :class="stats.returnRate >= 0 ? 'text-success' : 'text-danger'">
                {{ formatPercent(stats.returnRate / 100) }}
              </div>
            </div>
          </el-col>
        </el-row>
      </div>
    </div>
    
    <template #footer>
      <el-button @click="visible = false">关闭</el-button>
      <el-button type="primary" :icon="Download" @click="handleExport">
        导出数据
      </el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { ref, computed, watch, reactive } from 'vue'
import { ElMessage } from 'element-plus'
import { useAccountStore } from '@/stores/account'
import { formatNumber, formatPercent } from '@/utils/format'
import { Download } from '@element-plus/icons-vue'
import EquityChart from '@/components/Charts/EquityChart.vue'

const props = defineProps({
  modelValue: Boolean,
  account: Object
})

const emit = defineEmits(['update:modelValue'])

const accountStore = useAccountStore()

const loading = ref(false)
const timeRange = ref('30d')

const visible = computed({
  get: () => props.modelValue,
  set: (val) => emit('update:modelValue', val)
})

const stats = reactive({
  initialEquity: 10000,
  currentEquity: 12500,
  totalReturn: 2500,
  returnRate: 25.0
})

async function fetchData() {
  if (!props.account) return
  
  loading.value = true
  try {
    const res = await accountStore.fetchAccountEquityCurve(
      props.account.id,
      timeRange.value
    )
    
    if (res.data) {
      // 更新统计数据
      Object.assign(stats, res.data.statistics)
    }
  } catch (error) {
    ElMessage.error('获取数据失败: ' + error.message)
  } finally {
    loading.value = false
  }
}

function handleExport() {
  ElMessage.info('导出功能开发中...')
}

watch(() => props.account, (val) => {
  if (val) {
    fetchData()
  }
}, { immediate: true })
</script>

<style lang="scss" scoped>
.equity-curve-dialog {
  .time-range-selector {
    margin-bottom: 20px;
    text-align: center;
  }
  
  .chart-container {
    min-height: 400px;
  }
  
  .statistics {
    .stat-item {
      text-align: center;
      padding: 20px;
      background: var(--bg-color);
      border-radius: 8px;
      
      .stat-label {
        font-size: 14px;
        color: var(--text-secondary);
        margin-bottom: 8px;
      }
      
      .stat-value {
        font-size: 20px;
        font-weight: bold;
      }
    }
  }
}
</style>

