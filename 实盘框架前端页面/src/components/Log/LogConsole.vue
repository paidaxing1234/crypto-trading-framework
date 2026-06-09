<template>
  <div class="log-console" :class="{ 'fullscreen': isFullscreen }">
    <!-- 控制台工具栏 -->
    <div class="console-toolbar">
      <div class="toolbar-left">
        <el-tag :type="connected ? 'success' : 'danger'" size="small">
          <el-icon><Connection /></el-icon>
          {{ connected ? '已连接' : '未连接' }}
        </el-tag>
        <el-tag size="small">
          日志数: {{ logCount }}
        </el-tag>
        <el-tag size="small" type="info">
          延迟: {{ latency }}ms
        </el-tag>
      </div>
      
      <div class="toolbar-right">
        <el-button-group size="small">
          <el-button 
            :type="autoScroll ? 'primary' : ''"
            :icon="Bottom"
            @click="toggleAutoScroll"
          >
            自动滚动
          </el-button>
          <el-button 
            :icon="isFullscreen ? 'ZoomOut' : 'FullScreen'"
            @click="toggleFullscreen"
          >
            {{ isFullscreen ? '退出全屏' : '全屏' }}
          </el-button>
          <el-button :icon="Download" @click="handleExport">
            导出
          </el-button>
          <el-button :icon="Delete" @click="handleClear">
            清空
          </el-button>
        </el-button-group>
        
        <el-button 
          size="small" 
          :icon="Setting"
          @click="showConfigDialog = true"
        >
          配置
        </el-button>
      </div>
    </div>
    
    <!-- 快速过滤 -->
    <div class="console-filters">
      <el-radio-group v-model="logMode" size="small" @change="handleModeChange">
        <el-radio-button label="realtime">实时日志</el-radio-button>
        <el-radio-button label="history">历史日志</el-radio-button>
      </el-radio-group>

      <el-select
        v-if="logMode === 'history'"
        v-model="selectedDate"
        placeholder="选择日期"
        size="small"
        style="width: 130px; margin-left: 10px;"
        @change="loadHistoryLogs"
      >
        <el-option
          v-for="date in availableDates"
          :key="date"
          :label="formatDateLabel(date)"
          :value="date"
        />
      </el-select>

      <el-checkbox-group v-model="visibleLevels" size="small" style="margin-left: 10px;">
        <el-checkbox-button label="debug">
          <el-icon><Tools /></el-icon> 调试
        </el-checkbox-button>
        <el-checkbox-button label="info">
          <el-icon><InfoFilled /></el-icon> 信息
        </el-checkbox-button>
        <el-checkbox-button label="warning">
          <el-icon><Warning /></el-icon> 警告
        </el-checkbox-button>
        <el-checkbox-button label="error">
          <el-icon><CircleClose /></el-icon> 错误
        </el-checkbox-button>
      </el-checkbox-group>

      <el-select
        v-model="filterSource"
        placeholder="来源"
        clearable
        size="small"
        style="width: 120px; margin-left: 10px;"
        @change="logMode === 'history' && loadHistoryLogs()"
      >
        <el-option label="全部来源" value="" />
        <el-option label="系统" value="system" />
        <el-option label="行情" value="market" />
        <el-option label="订单" value="order" />
        <el-option label="策略" value="strategy" />
        <el-option label="前端" value="frontend" />
      </el-select>

      <el-input
        v-model="filterKeyword"
        placeholder="过滤关键词..."
        clearable
        size="small"
        style="width: 150px; margin-left: 10px;"
      >
        <template #prefix>
          <el-icon><Search /></el-icon>
        </template>
      </el-input>

      <el-tag v-if="logMode === 'history'" size="small" type="info" style="margin-left: 10px;">
        共 {{ historyTotal }} 条
      </el-tag>
    </div>
    
    <!-- 日志显示区域 -->
    <div 
      ref="logContainerRef" 
      class="console-content"
      @scroll="handleScroll"
    >
      <div 
        v-for="log in filteredLogs" 
        :key="log.id"
        :class="['console-line', `level-${log.level}`]"
        @click="handleLogClick(log)"
      >
        <span class="log-time">{{ formatConsoleTime(log.timestamp) }}</span>
        <span :class="['log-level', `level-${log.level}`]">
          [{{ getLevelLabel(log.level) }}]
        </span>
        <span class="log-source">[{{ log.source }}]</span>
        <span class="log-message">{{ log.message }}</span>
        <el-icon v-if="log.data" class="log-has-data" title="包含附加数据">
          <Document />
        </el-icon>
      </div>
      
      <!-- 无日志提示 -->
      <div v-if="filteredLogs.length === 0" class="no-logs">
        <el-empty description="暂无日志" />
      </div>
      
      <!-- 滚动到底部按钮 -->
      <transition name="fade">
        <div 
          v-show="!isAtBottom && !autoScroll" 
          class="scroll-to-bottom"
          @click="scrollToBottom"
        >
          <el-icon><Bottom /></el-icon>
          新日志
        </div>
      </transition>
    </div>
    
    <!-- 日志配置对话框 -->
    <el-dialog
      v-model="showConfigDialog"
      title="日志配置"
      width="600px"
      :append-to-body="true"
    >
      <el-form :model="logConfig" label-width="120px">
        <el-form-item label="后端日志级别">
          <el-select v-model="logConfig.backendLevel" style="width: 100%">
            <el-option label="调试 (DEBUG)" value="debug" />
            <el-option label="信息 (INFO)" value="info" />
            <el-option label="警告 (WARNING)" value="warning" />
            <el-option label="错误 (ERROR)" value="error" />
          </el-select>
          <div class="form-item-tip">
            设置C++后端的最低日志级别
          </div>
        </el-form-item>
        
        <el-form-item label="最大日志数量">
          <el-input-number 
            v-model="logConfig.maxLogs" 
            :min="100" 
            :max="50000" 
            :step="100"
            style="width: 100%"
          />
          <div class="form-item-tip">
            前端保存的最大日志条数（防止内存溢出）
          </div>
        </el-form-item>
        
        <el-form-item label="日志来源过滤">
          <el-checkbox-group v-model="logConfig.enabledSources">
            <el-checkbox label="system">系统</el-checkbox>
            <el-checkbox label="strategy">策略</el-checkbox>
            <el-checkbox label="order">订单</el-checkbox>
            <el-checkbox label="account">账户</el-checkbox>
            <el-checkbox label="market">行情</el-checkbox>
            <el-checkbox label="backend">后端</el-checkbox>
          </el-checkbox-group>
          <div class="form-item-tip">
            选择需要接收的日志来源
          </div>
        </el-form-item>
        
        <el-form-item label="显示时间戳">
          <el-switch v-model="logConfig.showTimestamp" />
        </el-form-item>
        
        <el-form-item label="显示来源">
          <el-switch v-model="logConfig.showSource" />
        </el-form-item>
        
        <el-form-item label="彩色输出">
          <el-switch v-model="logConfig.coloredOutput" />
        </el-form-item>
      </el-form>
      
      <template #footer>
        <el-button @click="showConfigDialog = false">取消</el-button>
        <el-button type="primary" @click="applyLogConfig">
          应用配置
        </el-button>
      </template>
    </el-dialog>
    
    <!-- 日志详情对话框 -->
    <el-dialog
      v-model="showDetailDialog"
      title="日志详情"
      width="700px"
      :append-to-body="true"
    >
      <div v-if="selectedLog" class="log-detail">
        <el-descriptions :column="2" border>
          <el-descriptions-item label="时间">
            {{ formatTime(selectedLog.timestamp) }}
          </el-descriptions-item>
          <el-descriptions-item label="级别">
            <el-tag :type="getLevelType(selectedLog.level)">
              {{ getLevelLabel(selectedLog.level) }}
            </el-tag>
          </el-descriptions-item>
          <el-descriptions-item label="来源">
            {{ selectedLog.source }}
          </el-descriptions-item>
          <el-descriptions-item label="日志ID">
            {{ selectedLog.id }}
          </el-descriptions-item>
          <el-descriptions-item label="消息" :span="2">
            <pre class="log-message-detail">{{ selectedLog.message }}</pre>
          </el-descriptions-item>
          <el-descriptions-item v-if="selectedLog.data" label="附加数据" :span="2">
            <pre class="log-data-detail">{{ JSON.stringify(selectedLog.data, null, 2) }}</pre>
          </el-descriptions-item>
        </el-descriptions>
      </div>
      
      <template #footer>
        <el-button @click="showDetailDialog = false">关闭</el-button>
        <el-button type="primary" @click="copyLogDetail">复制</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, watch, nextTick, onMounted, onUnmounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useLogStore } from '@/stores/log'
import { formatTime } from '@/utils/format'
import { wsClient } from '@/services/WebSocketClient'
import {
  Connection,
  Bottom,
  Download,
  Delete,
  Setting,
  Search,
  Document,
  InfoFilled,
  Warning,
  CircleClose,
  Tools
} from '@element-plus/icons-vue'

const logStore = useLogStore()

// 状态
const logContainerRef = ref(null)
const autoScroll = ref(true)
const isAtBottom = ref(true)
const isFullscreen = ref(false)
const showConfigDialog = ref(false)
const showDetailDialog = ref(false)
const selectedLog = ref(null)

// 过滤
const visibleLevels = ref(['debug', 'info', 'warning', 'error'])
const filterKeyword = ref('')
const filterSource = ref('')
const filterDateRange = ref(null)

// 日志模式：realtime 实时 / history 历史
const logMode = ref('realtime')
const selectedDate = ref('')
const availableDates = ref([])
const historyLogs = ref([])
const historyTotal = ref(0)

// 日期快捷选项
const dateShortcuts = [
  {
    text: '最近1小时',
    value: () => {
      const end = new Date()
      const start = new Date()
      start.setTime(start.getTime() - 3600 * 1000)
      return [start, end]
    }
  },
  {
    text: '最近6小时',
    value: () => {
      const end = new Date()
      const start = new Date()
      start.setTime(start.getTime() - 3600 * 1000 * 6)
      return [start, end]
    }
  },
  {
    text: '今天',
    value: () => {
      const end = new Date()
      const start = new Date()
      start.setHours(0, 0, 0, 0)
      return [start, end]
    }
  }
]

// 连接状态
const connected = ref(false)
const latency = ref(0)

// 日志配置
const logConfig = ref({
  backendLevel: 'info',
  maxLogs: 10000,
  enabledSources: ['system', 'strategy', 'order', 'account', 'market', 'backend'],
  showTimestamp: true,
  showSource: true,
  coloredOutput: true
})

// 计算属性
const logCount = computed(() => logMode.value === 'history' ? historyTotal.value : logStore.logs.length)

const filteredLogs = computed(() => {
  try {
    // 根据模式选择日志源
    let logs = logMode.value === 'history' ? historyLogs.value : (logStore.logs || [])

    // 按级别过滤
    logs = logs.filter(log => log && visibleLevels.value.includes(log.level))

    // 按来源过滤（实时模式下）
    if (logMode.value === 'realtime') {
      if (filterSource.value) {
        logs = logs.filter(log => log && log.source === filterSource.value)
      } else {
        logs = logs.filter(log =>
          log && log.source && logConfig.value.enabledSources.includes(log.source)
        )
      }
    }

    // 按关键词过滤
    if (filterKeyword.value) {
      const keyword = filterKeyword.value.toLowerCase()
      logs = logs.filter(log =>
        log && log.message && (
          log.message.toLowerCase().includes(keyword) ||
          (log.source && log.source.toLowerCase().includes(keyword))
        )
      )
    }

    // 限制显示数量，提升性能 - 最多显示500条
    return logs.slice(0, 500)
  } catch (error) {
    console.error('Error filtering logs:', error)
    return []
  }
})

// 方法
function formatConsoleTime(timestamp) {
  if (!logConfig.value.showTimestamp) return ''
  return formatTime(timestamp, 'HH:mm:ss.SSS')
}

function formatDateLabel(date) {
  // 20260107 -> 2026-01-07
  if (!date || date.length !== 8) return date
  return `${date.slice(0, 4)}-${date.slice(4, 6)}-${date.slice(6, 8)}`
}

async function handleModeChange(mode) {
  if (mode === 'history') {
    // 加载可用日期列表
    availableDates.value = await logStore.fetchLogDates()
    // 默认选择最新日期
    if (availableDates.value.length > 0) {
      selectedDate.value = availableDates.value[availableDates.value.length - 1]
      await loadHistoryLogs()
    }
  }
}

async function loadHistoryLogs() {
  if (!selectedDate.value) return

  const result = await logStore.fetchLogsFromFile({
    date: selectedDate.value,
    source: filterSource.value,
    limit: 1000
  })

  historyLogs.value = result.logs || []
  historyTotal.value = result.total || 0

  // 给每条日志添加 id
  historyLogs.value = historyLogs.value.map((log, index) => ({
    ...log,
    id: `history-${selectedDate.value}-${index}`
  }))
}

function getLevelLabel(level) {
  const labels = {
    debug: 'DEBUG',
    info: 'INFO',
    warning: 'WARN',
    error: 'ERROR'
  }
  return labels[level] || level.toUpperCase()
}

function getLevelType(level) {
  const types = {
    debug: 'info',
    info: 'success',
    warning: 'warning',
    error: 'danger'
  }
  return types[level] || 'info'
}

function toggleAutoScroll() {
  autoScroll.value = !autoScroll.value
  if (autoScroll.value) {
    scrollToBottom()
  }
}

function toggleFullscreen() {
  isFullscreen.value = !isFullscreen.value
}

function scrollToBottom() {
  if (!logContainerRef.value) return
  
  nextTick(() => {
    logContainerRef.value.scrollTop = logContainerRef.value.scrollHeight
  })
}

function handleScroll() {
  if (!logContainerRef.value) return
  
  const { scrollTop, scrollHeight, clientHeight } = logContainerRef.value
  isAtBottom.value = scrollHeight - scrollTop - clientHeight < 50
}

function handleClear() {
  ElMessageBox.confirm(
    '确定要清空所有日志吗？',
    '警告',
    {
      confirmButtonText: '确定',
      cancelButtonText: '取消',
      type: 'warning'
    }
  ).then(() => {
    logStore.clearLogs()
    ElMessage.success('日志已清空')
  }).catch(() => {})
}

function handleExport() {
  try {
    logStore.exportLogs(filteredLogs.value)
    ElMessage.success('日志导出成功')
  } catch (error) {
    ElMessage.error('导出失败: ' + error.message)
  }
}

function handleLogClick(log) {
  selectedLog.value = log
  showDetailDialog.value = true
}

function copyLogDetail() {
  if (!selectedLog.value) return
  
  const detail = `
时间: ${formatTime(selectedLog.value.timestamp)}
级别: ${getLevelLabel(selectedLog.value.level)}
来源: ${selectedLog.value.source}
消息: ${selectedLog.value.message}
${selectedLog.value.data ? '附加数据:\n' + JSON.stringify(selectedLog.value.data, null, 2) : ''}
  `.trim()
  
  navigator.clipboard.writeText(detail).then(() => {
    ElMessage.success('已复制到剪贴板')
  }).catch(() => {
    ElMessage.error('复制失败')
  })
}

function applyLogConfig() {
  // 发送配置到后端
  const success = wsClient.send('set_log_config', {
    level: logConfig.value.backendLevel,
    sources: logConfig.value.enabledSources
  })
  
  if (success) {
    ElMessage.success('日志配置已发送到后端')
    showConfigDialog.value = false
    
    // 保存到本地存储
    localStorage.setItem('log_config', JSON.stringify(logConfig.value))
  } else {
    ElMessage.error('发送配置失败，请检查连接')
  }
}

// 监听WebSocket连接状态
function updateConnectionStatus({ connected: isConnected }) {
  connected.value = isConnected
}

// 监听日志更新 - 添加节流，避免频繁滚动
let scrollTimer = null
watch(() => logStore.logs.length, () => {
  if (autoScroll.value) {
    // 节流滚动，100ms内只滚动一次
    if (scrollTimer) clearTimeout(scrollTimer)
    scrollTimer = setTimeout(() => {
      scrollToBottom()
    }, 100)
  }
})

// 监听快照更新（获取延迟信息）
function handleSnapshot({ latency: lat }) {
  latency.value = Math.round(lat)
}

onMounted(() => {
  try {
    // 初始化日志store的WebSocket监听器
    logStore.initWebSocketListeners()
    
    // 连接WebSocket事件
    wsClient.on('connected', updateConnectionStatus)
    wsClient.on('disconnected', updateConnectionStatus)
    wsClient.on('snapshot', handleSnapshot)
    
    // 初始化连接状态
    connected.value = wsClient.connected
    
    // 加载保存的配置
    const savedConfig = localStorage.getItem('log_config')
    if (savedConfig) {
      try {
        logConfig.value = { ...logConfig.value, ...JSON.parse(savedConfig) }
      } catch (e) {
        console.error('加载日志配置失败', e)
      }
    }
    
    // 延迟滚动，等待DOM渲染
    setTimeout(() => {
      scrollToBottom()
    }, 100)
  } catch (error) {
    console.error('LogConsole mounted error:', error)
  }
})

onUnmounted(() => {
  wsClient.off('connected', updateConnectionStatus)
  wsClient.off('disconnected', updateConnectionStatus)
  wsClient.off('snapshot', handleSnapshot)
})
</script>

<style lang="scss" scoped>
.log-console {
  display: flex;
  flex-direction: column;
  height: 100%;
  background: #1e1e1e;
  color: #d4d4d4;
  border-radius: 4px;
  overflow: hidden;
  
  &.fullscreen {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    z-index: 9999;
    border-radius: 0;
  }
}

.console-toolbar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px 15px;
  background: #2d2d30;
  border-bottom: 1px solid #3e3e42;
  
  .toolbar-left,
  .toolbar-right {
    display: flex;
    gap: 10px;
    align-items: center;
  }
}

.console-filters {
  display: flex;
  align-items: center;
  padding: 10px 15px;
  background: #252526;
  border-bottom: 1px solid #3e3e42;
  
  :deep(.el-checkbox-button) {
    .el-checkbox-button__inner {
      background: #3c3c3c;
      border-color: #3e3e42;
      color: #d4d4d4;
    }
    
    &.is-checked .el-checkbox-button__inner {
      background: #0e639c;
      border-color: #007acc;
    }
  }
}

.console-content {
  flex: 1;
  overflow-y: auto;
  padding: 10px;
  font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
  font-size: 13px;
  line-height: 1.5;
  position: relative;
  
  &::-webkit-scrollbar {
    width: 10px;
  }
  
  &::-webkit-scrollbar-track {
    background: #1e1e1e;
  }
  
  &::-webkit-scrollbar-thumb {
    background: #424242;
    border-radius: 5px;
    
    &:hover {
      background: #4e4e4e;
    }
  }
}

.console-line {
  padding: 2px 5px;
  cursor: pointer;
  transition: background 0.2s;
  display: flex;
  align-items: center;
  gap: 8px;
  
  &:hover {
    background: #2a2d2e;
  }
  
  .log-time {
    color: #858585;
    font-size: 12px;
    flex-shrink: 0;
  }
  
  .log-level {
    font-weight: bold;
    flex-shrink: 0;
    
    &.level-debug {
      color: #858585;
    }
    
    &.level-info {
      color: #4ec9b0;
    }
    
    &.level-warning {
      color: #dcdcaa;
    }
    
    &.level-error {
      color: #f48771;
    }
  }
  
  .log-source {
    color: #9cdcfe;
    flex-shrink: 0;
  }
  
  .log-message {
    color: #d4d4d4;
    word-break: break-all;
  }
  
  .log-has-data {
    color: #858585;
    font-size: 12px;
    flex-shrink: 0;
  }
  
  &.level-error {
    background: rgba(244, 135, 113, 0.1);
  }
  
  &.level-warning {
    background: rgba(220, 220, 170, 0.05);
  }
}

.scroll-to-bottom {
  position: absolute;
  bottom: 20px;
  right: 20px;
  background: #007acc;
  color: white;
  padding: 8px 16px;
  border-radius: 4px;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 5px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
  transition: all 0.3s;
  
  &:hover {
    background: #005a9e;
    transform: translateY(-2px);
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.4);
  }
}

.no-logs {
  display: flex;
  justify-content: center;
  align-items: center;
  height: 100%;
  color: #858585;
}

.log-detail {
  .log-message-detail,
  .log-data-detail {
    margin: 0;
    padding: 10px;
    background: #f5f7fa;
    border-radius: 4px;
    font-family: 'Consolas', 'Monaco', monospace;
    font-size: 13px;
    line-height: 1.5;
    overflow-x: auto;
    max-height: 300px;
    color: #333;
  }
}

.form-item-tip {
  font-size: 12px;
  color: #909399;
  margin-top: 5px;
}

.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.3s;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}
</style>

