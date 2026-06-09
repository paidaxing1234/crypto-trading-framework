<template>
  <el-dialog
    v-model="visible"
    title="创建策略"
    width="600px"
    :close-on-click-modal="false"
  >
    <el-form
      ref="formRef"
      :model="form"
      :rules="rules"
      label-width="100px"
    >
      <el-form-item label="策略名称" prop="name">
        <el-input v-model="form.name" placeholder="请输入策略名称" />
      </el-form-item>

      <el-form-item label="选择配置" prop="configFile">
        <el-select
          v-model="form.configFile"
          placeholder="选择策略配置模板"
          style="width: 100%"
          @change="handleConfigChange"
          :loading="configsLoading"
        >
          <el-option
            v-for="config in strategyConfigs"
            :key="config.filename"
            :label="config.strategy_name || config.filename"
            :value="config.filename"
          >
            <span>{{ config.strategy_name || config.filename }}</span>
            <span style="float: right; color: var(--el-text-color-secondary); font-size: 12px;">
              {{ config.exchange?.toUpperCase() }} | {{ config.strategy_id }}
            </span>
          </el-option>
        </el-select>
      </el-form-item>

      <el-form-item label="策略ID" prop="strategyId">
        <el-input v-model="form.strategyId" placeholder="策略标识符" />
      </el-form-item>

      <el-form-item label="选择账户" prop="accountId">
        <el-select v-model="form.accountId" placeholder="选择交易账户" style="width: 100%">
          <el-option
            v-for="account in accounts"
            :key="account.account_id || account.strategy_id"
            :label="(account.account_id || account.strategy_id || '默认账户') + ' (' + (account.exchange || 'okx').toUpperCase() + ')'"
            :value="account.account_id || account.strategy_id"
          />
        </el-select>
      </el-form-item>

      <el-form-item label="交易对">
        <el-input v-model="form.symbol" placeholder="可选，例如: BTCUSDT 或 BTC-USDT-SWAP" />
      </el-form-item>

      <el-form-item label="Python文件" prop="pythonFile">
        <el-select
          v-model="form.pythonFile"
          placeholder="选择策略Python文件"
          style="width: 100%"
          filterable
          :loading="pyFilesLoading"
        >
          <el-option
            v-for="file in pythonFiles"
            :key="file.filename"
            :label="file.filename"
            :value="file.filename"
          >
            <span>{{ file.filename }}</span>
            <span style="float: right; color: var(--el-text-color-secondary); font-size: 12px;">
              {{ formatFileSize(file.size) }}
            </span>
          </el-option>
        </el-select>
      </el-form-item>

      <el-form-item label="QQ邮箱">
        <el-input v-model="form.qqEmail" placeholder="可选，用于风控告警通知" />
      </el-form-item>

      <el-form-item label="飞书邮箱">
        <el-input v-model="form.larkEmail" placeholder="可选，用于飞书告警通知" />
      </el-form-item>

      <el-form-item label="配置预览" v-if="selectedConfigContent">
        <el-input
          v-model="selectedConfigContent"
          type="textarea"
          :rows="6"
          readonly
        />
      </el-form-item>

      <el-form-item label="描述">
        <el-input
          v-model="form.description"
          type="textarea"
          :rows="3"
          placeholder="策略描述（可选）"
        />
      </el-form-item>

      <el-form-item label="自动启动">
        <el-switch v-model="form.autoStart" />
      </el-form-item>
    </el-form>

    <template #footer>
      <el-button @click="handleClose">取消</el-button>
      <el-button type="primary" @click="handleSubmit" :loading="loading">
        创建
      </el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { ref, reactive, computed, watch } from 'vue'
import { ElMessage } from 'element-plus'
import { useAccountStore } from '@/stores/account'
import { useStrategyStore } from '@/stores/strategy'
import { strategyApi } from '@/api/strategy'

const props = defineProps({
  modelValue: Boolean
})

const emit = defineEmits(['update:modelValue', 'success'])

const accountStore = useAccountStore()
const strategyStore = useStrategyStore()

const formRef = ref(null)
const loading = ref(false)
const configsLoading = ref(false)
const strategyConfigs = ref([])
const selectedConfigContent = ref('')
const pythonFiles = ref([])
const pyFilesLoading = ref(false)

const visible = computed({
  get: () => props.modelValue,
  set: (val) => emit('update:modelValue', val)
})

const accounts = computed(() => accountStore.accounts)

const form = reactive({
  name: '',
  configFile: '',
  strategyId: '',
  accountId: '',
  symbol: '',
  pythonFile: '',
  qqEmail: '',
  larkEmail: '',
  description: '',
  autoStart: false
})

const rules = {
  name: [
    { required: true, message: '请输入策略名称', trigger: 'blur' }
  ],
  configFile: [
    { required: true, message: '请选择配置模板', trigger: 'change' }
  ],
  strategyId: [
    { required: true, message: '请输入策略ID', trigger: 'blur' }
  ],
  accountId: [
    { required: true, message: '请选择交易账户', trigger: 'change' }
  ],
  pythonFile: [
    { required: true, message: '请选择Python文件', trigger: 'blur' }
  ]
}

function handleConfigChange(filename) {
  const config = strategyConfigs.value.find(c => c.filename === filename)
  if (config) {
    if (config.strategy_id && !form.strategyId) {
      form.strategyId = config.strategy_id
    }
    if (config.account_id && !form.accountId) {
      form.accountId = config.account_id
    }
    if (config.strategy_name && !form.name) {
      form.name = config.strategy_name
    }
    const preview = { ...config }
    delete preview.api_key
    delete preview.secret_key
    delete preview.passphrase
    selectedConfigContent.value = JSON.stringify(preview, null, 2)
  }
}

function formatFileSize(bytes) {
  if (bytes < 1024) return bytes + ' B'
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB'
  return (bytes / (1024 * 1024)).toFixed(1) + ' MB'
}

async function fetchPythonFiles() {
  pyFilesLoading.value = true
  try {
    const res = await strategyApi.listStrategyFiles()
    if (res && res.data) {
      pythonFiles.value = res.data
    }
  } catch (e) {
    console.error('获取Python文件列表失败:', e)
  } finally {
    pyFilesLoading.value = false
  }
}

async function handleSubmit() {
  try {
    await formRef.value.validate()
  } catch {
    return
  }

  loading.value = true
  try {
    await strategyStore.createStrategy({
      config_file: form.configFile,
      strategy_id: form.strategyId,
      account_id: form.accountId,
      strategy_name: form.name,
      symbol: form.symbol || '',
      python_file: form.pythonFile,
      qq_email: form.qqEmail || '',
      lark_email: form.larkEmail || '',
      description: form.description,
      auto_start: form.autoStart
    })

    ElMessage.success('策略创建成功')
    emit('success')
    handleClose()
  } catch (error) {
    if (error !== false) {
      ElMessage.error('创建失败: ' + error.message)
    }
  } finally {
    loading.value = false
  }
}

function handleClose() {
  visible.value = false
  selectedConfigContent.value = ''
  formRef.value?.resetFields()
}

async function fetchConfigs() {
  configsLoading.value = true
  try {
    const res = await strategyApi.listStrategyConfigs()
    if (res && res.data) {
      strategyConfigs.value = res.data
    }
  } catch (e) {
    console.error('fetchConfigs 异常:', e)
  } finally {
    configsLoading.value = false
  }
}

watch(visible, (val) => {
  if (val) {
    accountStore.fetchAccounts()
    fetchConfigs()
    fetchPythonFiles()
  }
})
</script>
