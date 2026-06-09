<template>
  <el-card class="log-sender">
    <template #header>
      <div class="card-header">
        <span>
          <el-icon><Upload /></el-icon>
          发送日志到后端
        </span>
        <el-button 
          type="text" 
          :icon="isExpanded ? 'ArrowUp' : 'ArrowDown'"
          @click="isExpanded = !isExpanded"
        />
      </div>
    </template>
    
    <div v-show="isExpanded" class="sender-content">
      <el-form :inline="true" :model="logForm" @submit.prevent="handleSend">
        <el-form-item label="日志级别">
          <el-select v-model="logForm.level" style="width: 120px;">
            <el-option label="调试" value="debug" />
            <el-option label="信息" value="info" />
            <el-option label="警告" value="warning" />
            <el-option label="错误" value="error" />
          </el-select>
        </el-form-item>
        
        <el-form-item label="日志消息">
          <el-input 
            v-model="logForm.message"
            placeholder="请输入日志消息..."
            style="width: 400px;"
            clearable
          />
        </el-form-item>
        
        <el-form-item>
          <el-button 
            type="primary" 
            :icon="Promotion"
            @click="handleSend"
            :disabled="!logForm.message"
          >
            发送
          </el-button>
          <el-button @click="handleReset">重置</el-button>
        </el-form-item>
      </el-form>
      
      <el-collapse v-model="activeCollapse" accordion>
        <el-collapse-item title="附加数据 (JSON格式)" name="data">
          <el-input
            v-model="logForm.dataJson"
            type="textarea"
            :rows="4"
            placeholder='{"key": "value"}'
          />
        </el-collapse-item>
      </el-collapse>
      
      <div class="sender-tips">
        <el-alert
          title="提示"
          type="info"
          :closable="false"
          show-icon
        >
          <template #default>
            <ul>
              <li>发送的日志会同时显示在前端日志列表和发送到C++后端</li>
              <li>后端可以根据日志级别过滤和处理这些日志</li>
              <li>附加数据需要是有效的JSON格式</li>
            </ul>
          </template>
        </el-alert>
      </div>
      
      <div class="quick-actions">
        <span class="label">快速发送:</span>
        <el-button-group size="small">
          <el-button @click="sendQuickLog('info', '前端启动成功')">
            启动日志
          </el-button>
          <el-button @click="sendQuickLog('warning', '前端检测到性能问题')">
            性能警告
          </el-button>
          <el-button @click="sendQuickLog('error', '前端发生未捕获错误')">
            错误日志
          </el-button>
          <el-button @click="sendQuickLog('debug', '前端调试信息', { component: 'LogSender', action: 'test' })">
            调试日志
          </el-button>
        </el-button-group>
      </div>
    </div>
  </el-card>
</template>

<script setup>
import { ref, reactive } from 'vue'
import { ElMessage } from 'element-plus'
import { useLogStore } from '@/stores/log'
import {
  Upload,
  Promotion
} from '@element-plus/icons-vue'

const logStore = useLogStore()

const isExpanded = ref(true)
const activeCollapse = ref('')

const logForm = reactive({
  level: 'info',
  message: '',
  dataJson: ''
})

function handleSend() {
  if (!logForm.message) {
    ElMessage.warning('请输入日志消息')
    return
  }
  
  let data = null
  
  // 解析JSON数据
  if (logForm.dataJson) {
    try {
      data = JSON.parse(logForm.dataJson)
    } catch (error) {
      ElMessage.error('附加数据格式错误，请检查JSON格式')
      return
    }
  }
  
  // 发送日志
  logStore.sendLogToBackend(logForm.level, logForm.message, data)
  
  ElMessage.success('日志已发送到后端')
  
  // 重置表单
  handleReset()
}

function handleReset() {
  logForm.message = ''
  logForm.dataJson = ''
}

function sendQuickLog(level, message, data = null) {
  logStore.sendLogToBackend(level, message, data)
  ElMessage.success(`已发送${level}日志`)
}
</script>

<style lang="scss" scoped>
.log-sender {
  margin-bottom: 20px;
  
  .card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    
    span {
      display: flex;
      align-items: center;
      gap: 5px;
      font-weight: 600;
    }
  }
  
  .sender-content {
    .el-form {
      margin-bottom: 15px;
    }
    
    .el-collapse {
      margin-bottom: 15px;
    }
    
    .sender-tips {
      margin-bottom: 15px;
      
      ul {
        margin: 0;
        padding-left: 20px;
        
        li {
          margin: 5px 0;
          font-size: 13px;
        }
      }
    }
    
    .quick-actions {
      display: flex;
      align-items: center;
      gap: 10px;
      padding-top: 15px;
      border-top: 1px solid #ebeef5;
      
      .label {
        font-weight: 600;
        color: #606266;
      }
    }
  }
}
</style>

