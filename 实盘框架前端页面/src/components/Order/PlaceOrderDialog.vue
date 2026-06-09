<template>
  <el-dialog
    v-model="visible"
    title="手动下单"
    width="500px"
    :close-on-click-modal="false"
  >
    <el-form
      ref="formRef"
      :model="form"
      :rules="rules"
      label-width="100px"
    >
      <el-form-item label="选择账户" prop="accountId">
        <el-select v-model="form.accountId" placeholder="选择交易账户" style="width: 100%">
          <el-option
            v-for="account in accounts"
            :key="account.id"
            :label="account.name"
            :value="account.id"
          />
        </el-select>
      </el-form-item>
      
      <el-form-item label="交易对" prop="symbol">
        <el-input v-model="form.symbol" placeholder="例如: BTC-USDT-SWAP" />
      </el-form-item>
      
      <el-form-item label="订单类型" prop="type">
        <el-radio-group v-model="form.type">
          <el-radio-button label="LIMIT">限价单</el-radio-button>
          <el-radio-button label="MARKET">市价单</el-radio-button>
        </el-radio-group>
      </el-form-item>
      
      <el-form-item label="方向" prop="side">
        <el-radio-group v-model="form.side">
          <el-radio-button label="BUY">
            <span style="color: #67c23a;">买入</span>
          </el-radio-button>
          <el-radio-button label="SELL">
            <span style="color: #f56c6c;">卖出</span>
          </el-radio-button>
        </el-radio-group>
      </el-form-item>
      
      <el-form-item label="价格" prop="price" v-if="form.type === 'LIMIT'">
        <el-input-number
          v-model="form.price"
          :precision="2"
          :step="0.1"
          :min="0"
          style="width: 100%"
        />
      </el-form-item>
      
      <el-form-item label="数量" prop="quantity">
        <el-input-number
          v-model="form.quantity"
          :precision="4"
          :step="0.001"
          :min="0"
          style="width: 100%"
        />
      </el-form-item>
      
      <el-divider />
      
      <el-form-item label="预估金额">
        <span style="font-size: 18px; font-weight: bold;">
          {{ estimatedAmount }} USDT
        </span>
      </el-form-item>
      
      <el-alert
        type="warning"
        :closable="false"
        show-icon
      >
        <template #title>
          <span style="font-size: 12px;">
            请仔细核对订单信息，下单后可能立即成交
          </span>
        </template>
      </el-alert>
    </el-form>
    
    <template #footer>
      <el-button @click="handleClose">取消</el-button>
      <el-button
        :type="form.side === 'BUY' ? 'success' : 'danger'"
        @click="handleSubmit"
        :loading="loading"
      >
        {{ form.side === 'BUY' ? '买入' : '卖出' }}
      </el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { ref, reactive, computed } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useAccountStore } from '@/stores/account'
import { useOrderStore } from '@/stores/order'

const props = defineProps({
  modelValue: Boolean
})

const emit = defineEmits(['update:modelValue', 'success'])

const accountStore = useAccountStore()
const orderStore = useOrderStore()

const formRef = ref(null)
const loading = ref(false)

const visible = computed({
  get: () => props.modelValue,
  set: (val) => emit('update:modelValue', val)
})

const accounts = computed(() => accountStore.accounts)

const form = reactive({
  accountId: '',
  symbol: '',
  type: 'LIMIT',
  side: 'BUY',
  price: 0,
  quantity: 0
})

const rules = {
  accountId: [
    { required: true, message: '请选择交易账户', trigger: 'change' }
  ],
  symbol: [
    { required: true, message: '请输入交易对', trigger: 'blur' }
  ],
  price: [
    { required: true, message: '请输入价格', trigger: 'blur' }
  ],
  quantity: [
    { required: true, message: '请输入数量', trigger: 'blur' },
    { type: 'number', min: 0.0001, message: '数量必须大于0', trigger: 'blur' }
  ]
}

const estimatedAmount = computed(() => {
  if (form.type === 'LIMIT' && form.price && form.quantity) {
    return (form.price * form.quantity).toFixed(2)
  }
  return '0.00'
})

async function handleSubmit() {
  try {
    await formRef.value.validate()
    
    const orderInfo = `${form.side === 'BUY' ? '买入' : '卖出'} ${form.quantity} ${form.symbol}`
    const confirmMsg = form.type === 'MARKET'
      ? `确定要以市价${orderInfo}吗？`
      : `确定要以 ${form.price} 的价格${orderInfo}吗？`
    
    await ElMessageBox.confirm(
      confirmMsg,
      '确认下单',
      {
        confirmButtonText: '确认',
        cancelButtonText: '取消',
        type: 'warning'
      }
    )
    
    loading.value = true
    
    await orderStore.placeOrder({
      ...form,
      price: form.type === 'MARKET' ? null : form.price
    })
    
    ElMessage.success('订单提交成功')
    emit('success')
    handleClose()
  } catch (error) {
    if (error !== 'cancel' && error !== false) {
      ElMessage.error('下单失败: ' + error.message)
    }
  } finally {
    loading.value = false
  }
}

function handleClose() {
  visible.value = false
  formRef.value?.resetFields()
}
</script>

