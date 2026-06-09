<template>
  <el-dialog
    v-model="visible"
    title="添加交易账户"
    width="500px"
    :close-on-click-modal="false"
  >
    <el-form
      ref="formRef"
      :model="form"
      :rules="rules"
      label-width="100px"
    >
      <el-form-item label="账户ID" prop="accountId">
        <el-input v-model="form.accountId" placeholder="账户标识符 (如: binance_live_1)" />
      </el-form-item>

      <el-form-item label="交易所" prop="exchange">
        <el-select v-model="form.exchange" style="width: 100%" @change="handleExchangeChange">
          <el-option label="OKX" value="okx" />
          <el-option label="Binance" value="binance" />
        </el-select>
      </el-form-item>

      <el-form-item label="API Key" prop="apiKey">
        <el-input v-model="form.apiKey" placeholder="输入API Key" show-password />
      </el-form-item>

      <el-form-item label="Secret Key" prop="secretKey">
        <el-input v-model="form.secretKey" placeholder="输入Secret Key" show-password />
      </el-form-item>

      <el-form-item label="Passphrase" prop="passphrase" v-if="form.exchange === 'okx'">
        <el-input v-model="form.passphrase" placeholder="输入Passphrase (仅OKX需要)" show-password />
      </el-form-item>

      <el-form-item label="模拟盘">
        <el-switch v-model="form.isTestnet" />
        <span style="margin-left: 10px; color: var(--text-secondary);">
          开启后使用模拟盘环境
        </span>
      </el-form-item>
      
      <el-alert
        title="安全提示"
        type="warning"
        :closable="false"
        style="margin-bottom: 20px;"
      >
        请确保API Key仅具有必要的权限，建议开启IP白名单。密钥将加密存储。
      </el-alert>
    </el-form>
    
    <template #footer>
      <el-button @click="handleClose">取消</el-button>
      <el-button type="primary" @click="handleSubmit" :loading="loading">
        添加
      </el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { ref, reactive, computed } from 'vue'
import { ElMessage } from 'element-plus'
import { useAccountStore } from '@/stores/account'

const props = defineProps({
  modelValue: Boolean
})

const emit = defineEmits(['update:modelValue', 'success'])

const accountStore = useAccountStore()

const formRef = ref(null)
const loading = ref(false)

const visible = computed({
  get: () => props.modelValue,
  set: (val) => emit('update:modelValue', val)
})

const form = reactive({
  accountId: '',
  exchange: 'okx',
  apiKey: '',
  secretKey: '',
  passphrase: '',
  isTestnet: true
})

const rules = {
  accountId: [
    { required: true, message: '请输入账户ID', trigger: 'blur' }
  ],
  exchange: [
    { required: true, message: '请选择交易所', trigger: 'change' }
  ],
  apiKey: [
    { required: true, message: '请输入API Key', trigger: 'blur' }
  ],
  secretKey: [
    { required: true, message: '请输入Secret Key', trigger: 'blur' }
  ],
  passphrase: [
    {
      required: true,
      message: '请输入Passphrase',
      trigger: 'blur',
      validator: (rule, value, callback) => {
        if (form.exchange === 'okx' && !value) {
          callback(new Error('OKX需要Passphrase'))
        } else {
          callback()
        }
      }
    }
  ]
}

function handleExchangeChange() {
  if (form.exchange === 'binance') {
    form.passphrase = ''
  }
}

async function handleSubmit() {
  try {
    await formRef.value.validate()
  } catch (validationError) {
    // 表单验证失败，不需要额外提示（Element Plus 已经在表单项下方显示了错误）
    return
  }

  loading.value = true
  try {
    await accountStore.addAccount(form)
    ElMessage.success('账户添加成功')
    emit('success')
    handleClose()
  } catch (error) {
    console.error('[AddAccount] 注册失败:', error)
    ElMessage.error(error.message || '添加失败')
  } finally {
    loading.value = false
  }
}

function handleClose() {
  visible.value = false
  formRef.value?.resetFields()
}
</script>

