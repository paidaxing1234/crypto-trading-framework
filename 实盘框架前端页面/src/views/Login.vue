<template>
  <div class="login-container">
    <div class="login-box">
      <div class="login-header">
        <el-icon :size="50" color="#409eff"><TrendCharts /></el-icon>
        <h2>实盘交易管理系统</h2>
        <p>Trading Management System</p>
      </div>
      
      <el-form
        ref="formRef"
        :model="form"
        :rules="rules"
        @keyup.enter="handleLogin"
      >
        <el-form-item prop="username">
          <el-input
            v-model="form.username"
            placeholder="请输入用户名"
            size="large"
            :prefix-icon="User"
            clearable
          />
        </el-form-item>
        
        <el-form-item prop="password">
          <el-input
            v-model="form.password"
            type="password"
            placeholder="请输入密码"
            size="large"
            :prefix-icon="Lock"
            show-password
            clearable
          />
        </el-form-item>
        
        <el-form-item>
          <el-checkbox v-model="form.remember">记住密码</el-checkbox>
        </el-form-item>
        
        <el-form-item>
          <el-button
            type="primary"
            size="large"
            style="width: 100%"
            :loading="loading"
            @click="handleLogin"
          >
            登录
          </el-button>
        </el-form-item>
      </el-form>

      <div class="login-footer">
        <p>&copy; 2024 实盘交易管理系统. All rights reserved.</p>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive } from 'vue'
import { useRouter } from 'vue-router'
import { useUserStore } from '@/stores/user'
import { TrendCharts, User, Lock } from '@element-plus/icons-vue'

const router = useRouter()
const userStore = useUserStore()

const formRef = ref(null)
const loading = ref(false)

const form = reactive({
  username: '',
  password: '',
  remember: false
})

const rules = {
  username: [
    { required: true, message: '请输入用户名', trigger: 'blur' }
  ],
  password: [
    { required: true, message: '请输入密码', trigger: 'blur' },
    { min: 6, message: '密码长度不能少于6位', trigger: 'blur' }
  ]
}

async function handleLogin() {
  try {
    await formRef.value.validate()
    
    loading.value = true
    await userStore.login({
      username: form.username,
      password: form.password
    })
    
    // 登录成功，跳转到首页
    router.push('/')
  } catch (error) {
    console.error('登录失败:', error)
  } finally {
    loading.value = false
  }
}
</script>

<style lang="scss" scoped>
.login-container {
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--bg-base);
  padding: 20px;
  position: relative;
  overflow: hidden;

  /* 网格背景 */
  &::before {
    content: '';
    position: absolute;
    inset: 0;
    background-image:
      linear-gradient(rgba(16, 185, 129, 0.04) 1px, transparent 1px),
      linear-gradient(90deg, rgba(16, 185, 129, 0.04) 1px, transparent 1px);
    background-size: 64px 64px;
    mask-image: radial-gradient(ellipse 70% 70% at center, black 20%, transparent 70%);
    animation: fadeIn 1.5s ease-out;
  }

  /* 光晕 */
  &::after {
    content: '';
    position: absolute;
    width: 600px;
    height: 600px;
    background: radial-gradient(circle, rgba(16, 185, 129, 0.06) 0%, rgba(6, 182, 212, 0.03) 40%, transparent 70%);
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    pointer-events: none;
    animation: pulse-glow 4s ease-in-out infinite;
  }

  .login-box {
    width: 100%;
    max-width: 460px;
    background: var(--glass-bg);
    backdrop-filter: blur(20px) saturate(1.3);
    -webkit-backdrop-filter: blur(20px) saturate(1.3);
    border-radius: var(--radius-lg);
    padding: 56px 44px;
    border: 1px solid var(--glass-border);
    box-shadow: var(--shadow-lg);
    position: relative;
    z-index: 1;
    animation: fadeInUp 0.7s cubic-bezier(0.22, 1, 0.36, 1);

    .login-header {
      text-align: center;
      margin-bottom: 44px;

      .el-icon {
        margin-bottom: 20px;
        color: var(--accent-green) !important;
        filter: drop-shadow(0 0 16px rgba(16, 185, 129, 0.4));
      }

      h2 {
        margin: 0 0 10px 0;
        font-size: 26px;
        font-weight: 800;
        color: var(--text-primary);
        letter-spacing: -0.8px;
      }

      p {
        margin: 0;
        color: var(--text-muted);
        font-size: 12px;
        font-family: var(--font-mono);
        letter-spacing: 3px;
        text-transform: uppercase;
      }
    }

    :deep(.el-button--primary) {
      font-size: 15px;
      height: 48px;
      letter-spacing: 3px;
      font-weight: 700;
      text-transform: uppercase;
    }

    :deep(.el-checkbox__label) {
      color: var(--text-secondary) !important;
      font-size: 13px;
    }

    .login-footer {
      text-align: center;
      margin-top: 32px;

      p {
        margin: 0;
        color: var(--text-muted);
        font-size: 11px;
        font-family: var(--font-mono);
        letter-spacing: 0.5px;
      }
    }
  }
}

/* 浮动装饰球 */
.login-container .login-box::before {
  content: '';
  position: absolute;
  top: -60px;
  right: -60px;
  width: 120px;
  height: 120px;
  border-radius: 50%;
  background: radial-gradient(circle, rgba(16, 185, 129, 0.15), transparent 70%);
  pointer-events: none;
}

@media (max-width: 768px) {
  .login-container {
    .login-box { padding: 36px 24px; }
  }
}
</style>

