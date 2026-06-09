<template>
  <div class="strategy-page">
    <!-- 页面头部 -->
    <div class="page-header">
      <div>
        <h2>策略管理</h2>
        <p>管理和监控所有交易策略</p>
      </div>
      <el-button 
        type="primary" 
        :icon="Plus" 
        @click="showCreateDialog = true"
        v-permission="'strategy:create'"
      >
        添加策略
      </el-button>
    </div>
    
    <!-- 策略统计 -->
    <el-row :gutter="20" class="stats-row">
      <el-col :span="6">
        <el-card class="stat-card running">
          <div class="stat-icon">
            <el-icon><VideoPlay /></el-icon>
          </div>
          <div class="stat-info">
            <div class="stat-value">{{ runningStrategies.length }}</div>
            <div class="stat-label">运行中</div>
          </div>
        </el-card>
      </el-col>
      
      <el-col :span="6">
        <el-card class="stat-card pending">
          <div class="stat-icon">
            <el-icon><Clock /></el-icon>
          </div>
          <div class="stat-info">
            <div class="stat-value">{{ pendingStrategies.length }}</div>
            <div class="stat-label">待启动</div>
          </div>
        </el-card>
      </el-col>
      
      <el-col :span="6">
        <el-card class="stat-card stopped">
          <div class="stat-icon">
            <el-icon><VideoPause /></el-icon>
          </div>
          <div class="stat-info">
            <div class="stat-value">{{ stoppedStrategies.length }}</div>
            <div class="stat-label">已停止</div>
          </div>
        </el-card>
      </el-col>
      
      <el-col :span="6">
        <el-card class="stat-card error">
          <div class="stat-icon">
            <el-icon><Warning /></el-icon>
          </div>
          <div class="stat-info">
            <div class="stat-value">{{ errorStrategies.length }}</div>
            <div class="stat-label">异常</div>
          </div>
        </el-card>
      </el-col>
    </el-row>
    
    <!-- 策略列表 -->
    <el-card>
      <template #header>
        <div class="card-header">
          <span>策略列表</span>
          <div class="filters">
            <el-input
              v-model="searchText"
              placeholder="搜索策略名称"
              :prefix-icon="Search"
              clearable
              style="width: 200px"
            />
            <el-select v-model="statusFilter" placeholder="状态筛选" clearable style="width: 120px">
              <el-option label="运行中" value="running" />
              <el-option label="已停止" value="stopped" />
              <el-option label="待启动" value="pending" />
              <el-option label="异常" value="error" />
            </el-select>
          </div>
        </div>
      </template>
      
      <el-table :data="filteredStrategies" v-loading="loading">
        <el-table-column prop="name" label="策略名称" min-width="150">
          <template #default="{ row }">
            <div class="strategy-name">
              <el-icon><Document /></el-icon>
              <span>{{ row.name }}</span>
            </div>
          </template>
        </el-table-column>
        
        <el-table-column prop="type" label="类型" width="120" />
        
        <el-table-column prop="account" label="账户" width="150" />
        
        <el-table-column prop="status" label="状态" width="100">
          <template #default="{ row }">
            <el-tag :type="getStatusType(row.status)">
              {{ formatStrategyStatus(row.status) }}
            </el-tag>
          </template>
        </el-table-column>
        
        <el-table-column prop="pnl" label="盈亏 (USDT)" width="120" align="right">
          <template #default="{ row }">
            <span :class="pnlClass(row.pnl)">
              {{ row.pnl == null ? '-' : formatNumber(row.pnl, 2) }}
            </span>
          </template>
        </el-table-column>

        <el-table-column prop="returnRate" label="收益率" width="100" align="right">
          <template #default="{ row }">
            <span :class="pnlClass(row.returnRate)">
              {{ row.returnRate == null ? '-' : formatPercent(row.returnRate / 100) }}
            </span>
          </template>
        </el-table-column>

        <el-table-column prop="trades" label="成交笔数" width="100" align="right">
          <template #default="{ row }">
            {{ row.trades == null ? '-' : row.trades }}
          </template>
        </el-table-column>

        <el-table-column prop="runTime" label="运行时长" width="120">
          <template #default="{ row }">
            {{ formatRunTime(row) }}
          </template>
        </el-table-column>
        
        <el-table-column label="操作" width="300" fixed="right">
          <template #default="{ row }">
            <el-button
              v-if="row.status === 'running'"
              type="warning"
              size="small"
              @click="handleStopStrategy(row)"
            >
              停止
            </el-button>
            <el-button
              v-if="row.status !== 'running'"
              type="success"
              size="small"
              @click="handleStartStrategy(row)"
            >
              启动
            </el-button>
            <el-button type="info" size="small" @click="handleViewLogs(row)">
              日志
            </el-button>
            <el-button type="primary" size="small" @click="handleViewDetail(row)">
              详情
            </el-button>
            <Permission :permission="'strategy:delete'">
              <el-button type="danger" size="small" @click="handleDelete(row)">
                删除
              </el-button>
            </Permission>
          </template>
        </el-table-column>
      </el-table>
    </el-card>
    
    <!-- 创建策略对话框 -->
    <create-strategy-dialog
      v-model="showCreateDialog"
      @success="handleCreateSuccess"
    />

    <!-- 策略日志对话框 -->
    <el-dialog
      v-model="showLogDialog"
      :title="'策略日志 - ' + currentLogStrategy"
      width="80%"
      top="5vh"
      destroy-on-close
    >
      <div class="log-dialog-content">
        <!-- 日志文件选择 -->
        <div class="log-toolbar">
          <el-select
            v-model="selectedLogFile"
            placeholder="选择日志文件"
            @change="handleLogFileChange"
            style="width: 400px"
          >
            <el-option
              v-for="file in strategyStore.strategyLogFiles"
              :key="file.filename"
              :label="file.filename"
              :value="file.filename"
            >
              <span>{{ file.filename }}</span>
              <span style="float: right; color: #8492a6; font-size: 12px">
                {{ formatFileSize(file.size) }}
              </span>
            </el-option>
          </el-select>
          <el-select v-model="logTailLines" style="width: 140px" @change="handleRefreshLogs">
            <el-option :label="'最近 100 行'" :value="100" />
            <el-option :label="'最近 200 行'" :value="200" />
            <el-option :label="'最近 500 行'" :value="500" />
            <el-option :label="'最近 1000 行'" :value="1000" />
          </el-select>
          <el-button :icon="Refresh" @click="handleRefreshLogs" :loading="strategyStore.logsLoading">
            刷新
          </el-button>
          <el-switch v-model="autoRefreshLogs" active-text="自动刷新" />
        </div>
        <!-- 日志内容 -->
        <div class="log-viewer" ref="logViewerRef" v-loading="strategyStore.logsLoading">
          <div v-if="strategyStore.strategyLogs.length === 0 && !strategyStore.logsLoading" class="log-empty">
            暂无日志内容
          </div>
          <pre v-else class="log-content"><code v-for="(line, idx) in strategyStore.strategyLogs" :key="idx" :class="getLogLineClass(line)">{{ line }}
</code></pre>
        </div>
      </div>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick, watch } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useStrategyStore } from '@/stores/strategy'
import { formatNumber, formatPercent, formatStrategyStatus, formatDuration, parseTimestamp } from '@/utils/format'
import {
  Plus,
  Search,
  Refresh,
  VideoPlay,
  VideoPause,
  Clock,
  Warning,
  Document
} from '@element-plus/icons-vue'

import CreateStrategyDialog from '@/components/Strategy/CreateStrategyDialog.vue'

const strategyStore = useStrategyStore()
const router = useRouter()

const searchText = ref('')
const statusFilter = ref('')
const showCreateDialog = ref(false)

// 用于"运行时长"实时跳动(每秒刷新一次显示)
const now = ref(Date.now())
let nowTimer = null

function formatRunTime(row) {
  if (row.status !== 'running') return '-'
  // 优先用"首次日志日期"作为起算点(对重启/暂停免疫); 没有则回退到进程 start_time
  const firstLog = strategyStore.firstLogDates[row.id]
  const startMs = (firstLog != null) ? firstLog : parseTimestamp(row.start_time)
  if (!startMs) return '-'
  return formatDuration(now.value - startMs)
}

function pnlClass(v) {
  if (v === null || v === undefined) return ''
  return v >= 0 ? 'text-success' : 'text-danger'
}

// 日志相关
const showLogDialog = ref(false)
const currentLogStrategy = ref('')
const selectedLogFile = ref('')
const logTailLines = ref(200)
const autoRefreshLogs = ref(false)
const logViewerRef = ref(null)
let autoRefreshTimer = null

const loading = computed(() => strategyStore.loading)
const strategies = computed(() => strategyStore.strategies)
const runningStrategies = computed(() => strategyStore.runningStrategies)
const stoppedStrategies = computed(() => strategyStore.stoppedStrategies)
const pendingStrategies = computed(() => strategyStore.pendingStrategies)
const errorStrategies = computed(() => strategyStore.errorStrategies)

const filteredStrategies = computed(() => {
  let result = strategies.value
  
  if (searchText.value) {
    result = result.filter(s => 
      s.name.toLowerCase().includes(searchText.value.toLowerCase())
    )
  }
  
  if (statusFilter.value) {
    result = result.filter(s => s.status === statusFilter.value)
  }
  
  return result
})

function getStatusType(status) {
  const typeMap = {
    running: 'success',
    stopped: 'info',
    pending: 'warning',
    error: 'danger'
  }
  return typeMap[status] || ''
}

async function handleStartStrategy(row) {
  try {
    const res = await strategyStore.startStrategy(row.id)
    if (res && res.success === false) {
      ElMessage.warning(res.message || '策略启动失败')
    } else {
      ElMessage.success('策略启动成功')
      // 刷新策略列表
      await strategyStore.fetchStrategies()
    }
  } catch (error) {
    ElMessage.error('策略启动失败: ' + error.message)
  }
}

async function handleStopStrategy(row) {
  try {
    await ElMessageBox.confirm(
      '确定要停止该策略吗?',
      '提示',
      {
        confirmButtonText: '确定',
        cancelButtonText: '取消',
        type: 'warning'
      }
    )

    const res = await strategyStore.stopStrategy(row.id)
    if (res && res.success === false) {
      ElMessage.warning(res.message || '策略停止失败')
    } else {
      ElMessage.success('策略停止成功')
    }
    // 刷新策略列表
    await strategyStore.fetchStrategies()
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('策略停止失败: ' + error.message)
    }
  }
}

function handleViewDetail(row) {
  router.push({
    path: '/strategy/' + encodeURIComponent(row.id || row.strategy_id),
    query: {
      account_id: row.account || row.account_id || '',
      name: row.name || ''
    }
  })
}

// 查看策略日志
async function handleViewLogs(row) {
  currentLogStrategy.value = row.name || row.id
  showLogDialog.value = true
  selectedLogFile.value = ''
  strategyStore.strategyLogs = []

  // 获取该策略的日志文件列表
  const strategyId = row.id || row.name || ''
  await strategyStore.fetchStrategyLogFiles(strategyId)

  // 自动选择最新的日志文件
  if (strategyStore.strategyLogFiles.length > 0) {
    const sorted = [...strategyStore.strategyLogFiles].sort((a, b) => b.filename.localeCompare(a.filename))
    selectedLogFile.value = sorted[0].filename
    await handleLogFileChange(sorted[0].filename)
  }
}

async function handleLogFileChange(filename) {
  if (!filename) return
  await strategyStore.fetchStrategyLogs(filename, logTailLines.value)
  await nextTick()
  scrollLogToBottom()
}

async function handleRefreshLogs() {
  if (selectedLogFile.value) {
    await strategyStore.fetchStrategyLogs(selectedLogFile.value, logTailLines.value)
    await nextTick()
    scrollLogToBottom()
  }
}

function scrollLogToBottom() {
  if (logViewerRef.value) {
    logViewerRef.value.scrollTop = logViewerRef.value.scrollHeight
  }
}

function formatFileSize(bytes) {
  if (!bytes || bytes === 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB']
  const i = Math.floor(Math.log(bytes) / Math.log(1024))
  return (bytes / Math.pow(1024, i)).toFixed(1) + ' ' + units[i]
}

function getLogLineClass(line) {
  if (!line) return ''
  if (line.includes('[ERROR]') || line.includes('ERROR')) return 'log-error'
  if (line.includes('[WARN]') || line.includes('WARNING')) return 'log-warn'
  if (line.includes('[DEBUG]')) return 'log-debug'
  return ''
}

// 自动刷新日志
watch(autoRefreshLogs, (val) => {
  if (val) {
    autoRefreshTimer = setInterval(() => {
      if (showLogDialog.value && selectedLogFile.value) {
        handleRefreshLogs()
      }
    }, 3000)
  } else {
    if (autoRefreshTimer) {
      clearInterval(autoRefreshTimer)
      autoRefreshTimer = null
    }
  }
})

watch(showLogDialog, (val) => {
  if (!val) {
    autoRefreshLogs.value = false
  }
})

async function handleDelete(row) {
  try {
    await ElMessageBox.confirm(
      '确定要删除该策略吗? 此操作不可恢复。',
      '警告',
      {
        confirmButtonText: '确定',
        cancelButtonText: '取消',
        type: 'warning'
      }
    )

    await strategyStore.deleteStrategy(row.id)
    ElMessage.success('策略删除成功')
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('策略删除失败: ' + error.message)
    }
  }
}

function handleCreateSuccess() {
  ElMessage.success('策略创建成功')
  strategyStore.fetchStrategies()
}

onMounted(() => {
  strategyStore.fetchStrategies()
  nowTimer = setInterval(() => { now.value = Date.now() }, 1000)
})

onUnmounted(() => {
  if (autoRefreshTimer) {
    clearInterval(autoRefreshTimer)
  }
  if (nowTimer) {
    clearInterval(nowTimer)
  }
})
</script>

<style lang="scss" scoped>
.strategy-page {
  .page-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 28px;
    animation: fadeInUp 0.4s cubic-bezier(0.22, 1, 0.36, 1);

    h2 {
      margin: 0 0 6px 0;
      color: var(--text-primary);
      font-weight: 800;
      font-size: 22px;
      letter-spacing: -0.5px;
    }

    p {
      margin: 0;
      color: var(--text-muted);
      font-size: 13px;
      font-weight: 400;
    }
  }

  .stats-row {
    margin-bottom: 24px;

    .stat-card {
      :deep(.el-card__body) {
        display: flex;
        align-items: center;
        gap: 16px;
        padding: 20px 24px;
      }

      .stat-icon {
        width: 52px;
        height: 52px;
        border-radius: var(--radius-sm);
        display: flex;
        align-items: center;
        justify-content: center;
        font-size: 24px;
        flex-shrink: 0;
      }

      .stat-info {
        .stat-value {
          font-size: 28px;
          font-weight: 800;
          margin-bottom: 4px;
          font-family: var(--font-mono);
          color: var(--text-primary);
          letter-spacing: -1px;
          line-height: 1;
        }

        .stat-label {
          font-size: 11px;
          color: var(--text-muted);
          text-transform: uppercase;
          letter-spacing: 1px;
          font-weight: 600;
        }
      }

      &.running .stat-icon {
        background: rgba(52, 211, 153, 0.1);
        color: var(--accent-green);
      }
      &.pending .stat-icon {
        background: rgba(245, 158, 11, 0.1);
        color: var(--accent-orange);
      }
      &.stopped .stat-icon {
        background: rgba(107, 114, 128, 0.1);
        color: var(--text-muted);
      }
      &.error .stat-icon {
        background: rgba(239, 68, 68, 0.1);
        color: var(--accent-red);
      }
    }
  }

  .card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--text-primary);
    font-weight: 600;

    .filters {
      display: flex;
      gap: 12px;
    }
  }

  .strategy-name {
    display: flex;
    align-items: center;
    gap: 10px;
    color: var(--text-primary);
    font-weight: 500;

    .el-icon { color: var(--text-muted); font-size: 16px; }
  }
}

.log-dialog-content {
  .log-toolbar {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-bottom: 16px;
    flex-wrap: wrap;
  }

  .log-viewer {
    height: 60vh;
    overflow-y: auto;
    background: var(--log-bg);
    border-radius: var(--radius);
    padding: 20px;
    border: 1px solid var(--border-color);
    font-family: var(--font-mono);
    font-size: 13px;
    line-height: 1.7;

    .log-empty {
      color: var(--text-muted);
      text-align: center;
      padding: 60px;
      font-size: 14px;
    }

    .log-content {
      margin: 0;
      padding: 0;
      white-space: pre-wrap;
      word-break: break-all;

      code {
        display: block;
        color: var(--log-text);
        padding: 2px 6px;
        border-radius: 3px;
        transition: background 0.2s;

        &:hover { background: rgba(255,255,255,0.03); }

        &.log-error {
          color: var(--accent-red);
          background: rgba(239, 68, 68, 0.06);
          border-left: 3px solid var(--accent-red);
          padding-left: 12px;
        }
        &.log-warn {
          color: var(--accent-orange);
          background: rgba(245, 158, 11, 0.06);
          border-left: 3px solid var(--accent-orange);
          padding-left: 12px;
        }
        &.log-debug {
          color: var(--text-muted);
          opacity: 0.7;
        }
      }
    }
  }
}
</style>

