<template>
  <div class="user-management-page">
    <!-- 页面头部 -->
    <div class="page-header">
      <div>
        <h2>用户管理</h2>
        <p>管理系统用户和权限</p>
      </div>
      <el-button type="primary" :icon="Plus" @click="openCreateDialog">
        添加用户
      </el-button>
    </div>

    <!-- 用户统计 -->
    <el-row :gutter="20" class="stats-row">
      <el-col :span="8">
        <el-card class="stat-card">
          <el-statistic title="总用户数" :value="users.length">
            <template #prefix>
              <el-icon><User /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>

      <el-col :span="8">
        <el-card class="stat-card">
          <el-statistic title="超级管理员" :value="adminCount">
            <template #prefix>
              <el-icon style="color: #409eff;"><UserFilled /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>

      <el-col :span="8">
        <el-card class="stat-card">
          <el-statistic title="策略管理员" :value="managerCount">
            <template #prefix>
              <el-icon style="color: #67c23a;"><Setting /></el-icon>
            </template>
          </el-statistic>
        </el-card>
      </el-col>
    </el-row>

    <!-- 用户列表 -->
    <el-card>
      <template #header>
        <div class="card-header">
          <span>用户列表</span>
          <el-input
            v-model="searchText"
            placeholder="搜索用户名"
            :prefix-icon="Search"
            clearable
            style="width: 250px"
          />
        </div>
      </template>

      <el-table :data="filteredUsers" v-loading="loading">
        <el-table-column prop="username" label="用户名" width="150">
          <template #default="{ row }">
            <div class="user-name">
              <el-avatar :size="32">{{ (row.username || 'U').charAt(0).toUpperCase() }}</el-avatar>
              <span>{{ row.username }}</span>
            </div>
          </template>
        </el-table-column>

        <el-table-column prop="role" label="角色" width="130">
          <template #default="{ row }">
            <el-tag :type="row.role === UserRole.SUPER_ADMIN ? '' : 'success'">
              {{ row.roleName }}
            </el-tag>
          </template>
        </el-table-column>

        <el-table-column prop="allowed_strategies" label="可管理策略" min-width="250">
          <template #default="{ row }">
            <template v-if="row.role === UserRole.STRATEGY_MANAGER && row.allowed_strategies?.length">
              <el-tag
                v-for="s in row.allowed_strategies"
                :key="s"
                size="small"
                type="info"
                style="margin: 2px 4px 2px 0;"
              >
                {{ s }}
              </el-tag>
            </template>
            <span v-else-if="row.role === UserRole.SUPER_ADMIN" style="color: #909399;">全部权限</span>
            <span v-else style="color: #909399;">无</span>
          </template>
        </el-table-column>

        <el-table-column prop="active" label="状态" width="100">
          <template #default="{ row }">
            <el-tag :type="row.active ? 'success' : 'info'">
              {{ row.active ? '激活' : '禁用' }}
            </el-tag>
          </template>
        </el-table-column>

        <el-table-column prop="last_login" label="最后登录" width="180">
          <template #default="{ row }">
            {{ row.last_login ? formatTimestamp(row.last_login) : '从未登录' }}
          </template>
        </el-table-column>

        <el-table-column label="操作" width="200" fixed="right">
          <template #default="{ row }">
            <el-button
              v-if="row.role === UserRole.STRATEGY_MANAGER"
              type="primary"
              size="small"
              @click="handleEdit(row)"
            >
              编辑
            </el-button>
            <el-button
              v-if="row.role !== UserRole.SUPER_ADMIN"
              type="danger"
              size="small"
              @click="handleDelete(row)"
            >
              删除
            </el-button>
            <span v-if="row.role === UserRole.SUPER_ADMIN" style="color: #909399; font-size: 12px;">
              超级管理员不可操作
            </span>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- 创建/编辑用户对话框 -->
    <el-dialog
      v-model="showDialog"
      :title="editingUser ? '编辑策略管理员' : '创建策略管理员'"
      width="550px"
      :close-on-click-modal="false"
    >
      <el-form
        ref="formRef"
        :model="form"
        :rules="rules"
        label-width="110px"
      >
        <el-form-item label="用户名" prop="username">
          <el-input
            v-model="form.username"
            placeholder="请输入用户名"
            :disabled="!!editingUser"
          />
        </el-form-item>

        <el-form-item label="密码" prop="password" v-if="!editingUser">
          <el-input
            v-model="form.password"
            type="password"
            placeholder="请输入密码（至少6位）"
            show-password
          />
        </el-form-item>

        <el-form-item label="可管理策略" prop="allowed_strategies">
          <el-select
            v-model="form.allowed_strategies"
            multiple
            filterable
            placeholder="选择可管理的策略"
            style="width: 100%"
          >
            <el-option
              v-for="s in availableStrategies"
              :key="s.strategy_id"
              :label="s.strategy_id"
              :value="s.strategy_id"
            />
          </el-select>
          <div class="form-tip">策略管理员只能查看和操作（启动/停止）被分配的策略</div>
        </el-form-item>
      </el-form>

      <template #footer>
        <el-button @click="showDialog = false">取消</el-button>
        <el-button type="primary" @click="handleSubmit" :loading="submitting">
          {{ editingUser ? '保存' : '创建' }}
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, reactive } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useUserStore, UserRole } from '@/stores/user'
import { strategyApi } from '@/api/strategy'
import {
  Plus,
  Search,
  User,
  UserFilled,
  Setting
} from '@element-plus/icons-vue'

const userStore = useUserStore()

const searchText = ref('')
const showDialog = ref(false)
const editingUser = ref(null)
const submitting = ref(false)
const formRef = ref(null)
const availableStrategies = ref([])

const form = reactive({
  username: '',
  password: '',
  allowed_strategies: []
})

const rules = {
  username: [
    { required: true, message: '请输入用户名', trigger: 'blur' },
    { min: 3, max: 20, message: '用户名长度为3-20个字符', trigger: 'blur' }
  ],
  password: [
    { required: true, message: '请输入密码', trigger: 'blur' },
    { min: 6, message: '密码长度不能少于6位', trigger: 'blur' }
  ]
}

const loading = computed(() => userStore.loading)
const users = computed(() => userStore.users)

const adminCount = computed(() =>
  users.value.filter(u => u.role === UserRole.SUPER_ADMIN).length
)

const managerCount = computed(() =>
  users.value.filter(u => u.role === UserRole.STRATEGY_MANAGER).length
)

const filteredUsers = computed(() => {
  if (!searchText.value) return users.value
  const search = searchText.value.toLowerCase()
  return users.value.filter(u => u.username.toLowerCase().includes(search))
})

function formatTimestamp(ts) {
  if (!ts) return ''
  const d = new Date(ts)
  return d.toLocaleString('zh-CN')
}

function openCreateDialog() {
  editingUser.value = null
  form.username = ''
  form.password = ''
  form.allowed_strategies = []
  showDialog.value = true
}

function handleEdit(row) {
  editingUser.value = row
  form.username = row.username
  form.password = ''
  form.allowed_strategies = [...(row.allowed_strategies || [])]
  showDialog.value = true
}

async function handleDelete(row) {
  if (row.role === UserRole.SUPER_ADMIN) {
    ElMessage.warning('不能删除超级管理员')
    return
  }
  try {
    await ElMessageBox.confirm(
      `确定要删除用户 "${row.username}" 吗？此操作不可恢复，对应的用户配置文件也会被删除。`,
      '删除用户',
      { confirmButtonText: '确定', cancelButtonText: '取消', type: 'warning' }
    )
    const result = await userStore.deleteUser(row.username)
    if (result.success) {
      await userStore.fetchUsers()
    }
  } catch (error) {
    // 取消删除
  }
}

async function handleSubmit() {
  try {
    await formRef.value.validate()
    submitting.value = true

    if (editingUser.value) {
      // 编辑：只更新 allowed_strategies
      const result = await userStore.updateUser(editingUser.value.username, {
        allowed_strategies: form.allowed_strategies
      })
      if (result.success) {
        await userStore.fetchUsers()
        showDialog.value = false
      }
    } else {
      // 创建用户
      const result = await userStore.createUser({
        username: form.username,
        password: form.password,
        role: 'STRATEGY_MANAGER',
        allowed_strategies: form.allowed_strategies
      })
      if (result.success) {
        await userStore.fetchUsers()
        showDialog.value = false
      }
    }
  } catch (error) {
    // 表单验证失败
  } finally {
    submitting.value = false
  }
}

// 加载可选策略列表
async function loadStrategies() {
  try {
    const res = await strategyApi.getStrategies()
    if (res.success && res.data) {
      availableStrategies.value = res.data.map(s => ({
        strategy_id: s.strategy_id || s.id || ''
      })).filter(s => s.strategy_id)
    }
  } catch (e) {
    console.warn('加载策略列表失败:', e)
  }
}

onMounted(async () => {
  await userStore.fetchUsers()
  await loadStrategies()
})
</script>

<style lang="scss" scoped>
.user-management-page {
  .page-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 28px;
    animation: fadeInUp 0.4s cubic-bezier(0.22, 1, 0.36, 1);

    h2 { margin: 0 0 6px 0; color: var(--text-primary); font-weight: 800; font-size: 22px; letter-spacing: -0.5px; }
    p { margin: 0; color: var(--text-muted); font-size: 13px; }
  }

  .stats-row {
    margin-bottom: 24px;
    .stat-card {
      :deep(.el-card__body) { padding: 24px; }
      :deep(.el-statistic__head) { color: var(--text-muted) !important; font-size: 11px; text-transform: uppercase; letter-spacing: 1px; font-weight: 600; }
      :deep(.el-statistic__content) { font-family: var(--font-mono); }
      :deep(.el-statistic__number) { color: var(--text-primary) !important; font-weight: 800; letter-spacing: -1px; }
    }
  }

  .card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--text-primary);
    font-weight: 600;
  }

  .user-name {
    display: flex;
    align-items: center;
    gap: 12px;
    color: var(--text-primary);
    font-weight: 500;

    .el-avatar {
      background: linear-gradient(135deg, var(--accent-green), var(--accent-cyan)) !important;
      color: #fff !important;
      font-weight: 700;
      font-family: var(--font-ui);
    }
  }

  .form-tip {
    font-size: 12px;
    color: var(--text-muted);
    margin-top: 6px;
    line-height: 1.5;
  }
}
</style>
