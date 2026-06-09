<template>
  <div class="strategy-logs-page">
    <div class="page-header">
      <div>
        <h2>策略日志</h2>
        <p>查看策略运行日志和源代码</p>
      </div>
    </div>

    <el-row :gutter="20">
      <!-- 左侧：文件列表 -->
      <el-col :span="6">
        <el-card>
          <template #header>
            <el-tabs v-model="activeTab" @tab-change="handleTabChange">
              <el-tab-pane label="日志文件" name="logs" />
              <el-tab-pane label="策略代码" name="source" />
            </el-tabs>
          </template>

          <el-input v-model="fileSearch" placeholder="搜索文件..." clearable style="margin-bottom: 10px" />

          <div class="file-list" v-loading="filesLoading">
            <div
              v-for="file in filteredFiles"
              :key="file.filename"
              class="file-item"
              :class="{ active: selectedFile === file.filename }"
              @click="handleSelectFile(file)"
            >
              <div class="file-name">{{ file.filename }}</div>
              <div class="file-size">{{ formatFileSize(file.size) }}</div>
            </div>
            <div v-if="filteredFiles.length === 0" class="no-files">暂无文件</div>
          </div>
        </el-card>
      </el-col>

      <!-- 右侧：内容查看 -->
      <el-col :span="18">
        <el-card>
          <template #header>
            <div class="content-header">
              <span>{{ selectedFile || '请选择文件' }}</span>
              <div class="toolbar" v-if="selectedFile">
                <template v-if="activeTab === 'logs'">
                  <el-select v-model="logTailLines" style="width: 130px" @change="refreshContent">
                    <el-option label="最近 100 行" :value="100" />
                    <el-option label="最近 200 行" :value="200" />
                    <el-option label="最近 500 行" :value="500" />
                    <el-option label="最近 1000 行" :value="1000" />
                  </el-select>
                  <el-button :icon="Refresh" @click="refreshContent" :loading="contentLoading">刷新</el-button>
                  <el-button :icon="Download" @click="handleExport" :loading="exporting">导出</el-button>
                  <el-switch v-model="autoRefresh" active-text="自动刷新" />
                </template>
                <template v-else-if="activeTab === 'source'">
                  <template v-if="userStore.isSuperAdmin">
                    <el-button v-if="!isEditing" type="primary" :icon="Edit" @click="startEdit">编辑</el-button>
                    <template v-else>
                      <el-button type="success" :icon="Check" @click="saveEdit" :loading="saving">保存</el-button>
                      <el-button :icon="Close" @click="cancelEdit">取消</el-button>
                    </template>
                  </template>
                  <el-button :icon="Refresh" @click="refreshContent" :loading="contentLoading">刷新</el-button>
                </template>
              </div>
            </div>
          </template>

          <div class="content-viewer" ref="viewerRef" v-loading="contentLoading">
            <div v-if="!selectedFile" class="content-empty">← 请从左侧选择文件查看</div>
            <div v-else-if="activeTab === 'logs' && logLines.length === 0 && !contentLoading" class="content-empty">暂无日志内容</div>
            <pre v-else-if="activeTab === 'logs'" class="log-content"><code v-for="(line, idx) in logLines" :key="idx" :class="getLogLineClass(line)">{{ line }}
</code></pre>
            <el-input
              v-else-if="activeTab === 'source' && isEditing"
              v-model="editingContent"
              type="textarea"
              :rows="30"
              class="source-editor"
            />
            <pre v-else-if="activeTab === 'source'" class="source-content"><code>{{ sourceContent }}</code></pre>
          </div>
        </el-card>
      </el-col>
    </el-row>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, watch, nextTick } from 'vue'
import { Refresh, Edit, Check, Close, Download } from '@element-plus/icons-vue'
import { useUserStore } from '@/stores/user'
import { ElMessage, ElMessageBox } from 'element-plus'
import { strategyApi } from '@/api/strategy'

const userStore = useUserStore()
const activeTab = ref('logs')
const fileSearch = ref('')
const selectedFile = ref('')
const filesLoading = ref(false)
const contentLoading = ref(false)
const logTailLines = ref(200)
const autoRefresh = ref(false)
const viewerRef = ref(null)

const logFiles = ref([])
const sourceFiles = ref([])
const logLines = ref([])
const sourceContent = ref('')
const isEditing = ref(false)
const editingContent = ref('')
const saving = ref(false)
const exporting = ref(false)
let autoRefreshTimer = null

const filteredFiles = computed(() => {
  const files = activeTab.value === 'logs' ? logFiles.value : sourceFiles.value
  if (!fileSearch.value) return files
  return files.filter(f => f.filename.toLowerCase().includes(fileSearch.value.toLowerCase()))
})

async function loadLogFiles() {
  filesLoading.value = true
  try {
    const res = await strategyApi.getStrategyLogFiles()
    logFiles.value = (res.data || []).sort((a, b) => b.filename.localeCompare(a.filename))
  } finally { filesLoading.value = false }
}

async function loadSourceFiles() {
  filesLoading.value = true
  try {
    const res = await strategyApi.listStrategyFiles()
    sourceFiles.value = (res.data || []).sort((a, b) => a.filename.localeCompare(b.filename))
  } finally { filesLoading.value = false }
}

async function handleSelectFile(file) {
  selectedFile.value = file.filename
  contentLoading.value = true
  try {
    if (activeTab.value === 'logs') {
      const res = await strategyApi.getStrategyLogs(file.filename, logTailLines.value)
      logLines.value = res.success && res.data ? res.data.lines || [] : []
      await nextTick()
      if (viewerRef.value) viewerRef.value.scrollTop = viewerRef.value.scrollHeight
    } else {
      const res = await strategyApi.getStrategySource(file.filename)
      sourceContent.value = res.success && res.data ? res.data.content || '' : '加载失败'
    }
  } finally { contentLoading.value = false }
}

async function refreshContent() {
  if (!selectedFile.value) return
  const file = { filename: selectedFile.value }
  await handleSelectFile(file)
}

async function handleExport() {
  if (!selectedFile.value) return
  exporting.value = true
  try {
    const res = await strategyApi.downloadLogFile(selectedFile.value, 'strategy')
    if (res.success && res.data?.content != null) {
      const blob = new Blob([res.data.content], { type: 'text/plain;charset=utf-8' })
      const url = URL.createObjectURL(blob)
      const a = document.createElement('a')
      a.href = url
      a.download = selectedFile.value
      a.click()
      URL.revokeObjectURL(url)
      ElMessage.success('导出成功')
    } else {
      ElMessage.error(res.message || '导出失败')
    }
  } catch (e) {
    ElMessage.error('导出失败: ' + e.message)
  } finally {
    exporting.value = false
  }
}

function handleTabChange() {
  selectedFile.value = ''
  logLines.value = []
  sourceContent.value = ''
  fileSearch.value = ''
  autoRefresh.value = false
  isEditing.value = false
  editingContent.value = ''
  if (activeTab.value === 'logs') loadLogFiles()
  else loadSourceFiles()
}

function startEdit() {
  editingContent.value = sourceContent.value
  isEditing.value = true
}

function cancelEdit() {
  isEditing.value = false
  editingContent.value = ''
}

async function saveEdit() {
  try {
    await ElMessageBox.confirm('确定要保存修改吗？', '提示', {
      confirmButtonText: '确定',
      cancelButtonText: '取消',
      type: 'warning'
    })

    saving.value = true
    const res = await strategyApi.saveStrategySource(selectedFile.value, editingContent.value)

    if (res.success) {
      ElMessage.success('保存成功')
      sourceContent.value = editingContent.value
      isEditing.value = false
    } else {
      ElMessage.error('保存失败: ' + (res.message || '未知错误'))
    }
  } catch (error) {
    if (error !== 'cancel') {
      ElMessage.error('保存失败: ' + error.message)
    }
  } finally {
    saving.value = false
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

watch(autoRefresh, (val) => {
  if (val) {
    autoRefreshTimer = setInterval(() => {
      if (selectedFile.value && activeTab.value === 'logs') refreshContent()
    }, 3000)
  } else {
    if (autoRefreshTimer) { clearInterval(autoRefreshTimer); autoRefreshTimer = null }
  }
})

onMounted(() => { loadLogFiles() })
onUnmounted(() => { if (autoRefreshTimer) clearInterval(autoRefreshTimer) })
</script>

<style lang="scss" scoped>
.strategy-logs-page {
  .page-header {
    margin-bottom: 24px;
    animation: fadeInUp 0.4s cubic-bezier(0.22, 1, 0.36, 1);

    h2 { margin: 0 0 6px 0; color: var(--text-primary); font-weight: 800; font-size: 22px; letter-spacing: -0.5px; }
    p { margin: 0; color: var(--text-muted); font-size: 13px; }
  }

  .file-list {
    max-height: 65vh;
    overflow-y: auto;

    .file-item {
      padding: 10px 14px;
      cursor: pointer;
      border-radius: var(--radius-sm);
      border-bottom: 1px solid var(--border-color);
      transition: all 0.25s;

      &:hover { background: var(--bg-card-hover); }
      &.active {
        background: linear-gradient(90deg, rgba(16, 185, 129, 0.1) 0%, transparent 100%);
        border-left: 3px solid var(--accent-green);
        color: var(--accent-green);
      }

      .file-name { font-size: 13px; word-break: break-all; font-family: var(--font-mono); color: var(--text-primary); font-weight: 500; }
      .file-size { font-size: 11px; color: var(--text-muted); margin-top: 4px; font-family: var(--font-mono); }
    }

    .no-files { text-align: center; color: var(--text-muted); padding: 40px 0; font-size: 13px; }
  }

  .content-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--text-primary);
    font-weight: 600;
    font-family: var(--font-mono);

    .toolbar { display: flex; align-items: center; gap: 12px; }
  }

  .content-viewer {
    height: 70vh;
    overflow-y: auto;
    background: var(--log-bg);
    border-radius: var(--radius);
    padding: 20px;
    border: 1px solid var(--border-color);
    font-family: var(--font-mono);
    font-size: 13px;
    line-height: 1.7;

    .content-empty { color: var(--text-muted); text-align: center; padding: 60px; font-size: 14px; }

    .log-content, .source-content {
      margin: 0; padding: 0; white-space: pre-wrap; word-break: break-all;
      code {
        display: block; color: var(--log-text); padding: 2px 6px; border-radius: 3px; transition: background 0.2s;
        &:hover { background: rgba(255,255,255,0.03); }
        &.log-error { color: var(--accent-red); background: rgba(239,68,68,0.06); border-left: 3px solid var(--accent-red); padding-left: 12px; }
        &.log-warn { color: var(--accent-orange); background: rgba(245,158,11,0.06); border-left: 3px solid var(--accent-orange); padding-left: 12px; }
        &.log-debug { color: var(--text-muted); opacity: 0.7; }
      }
    }

    .source-content code { color: var(--log-text); }
  }
}
</style>

