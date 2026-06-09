import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { ElMessage } from 'element-plus'
import { wsClient } from '@/services/WebSocketClient'

// ========== WebSocket 请求工具（复用 strategy.js 模式） ==========
const pendingUserRequests = new Map()

wsClient.on('response', (response) => {
  const { requestId, success, message, data } = response
  if (pendingUserRequests.has(requestId)) {
    const { resolve, reject } = pendingUserRequests.get(requestId)
    pendingUserRequests.delete(requestId)
    if (success) {
      resolve({ data: data?.data ?? data, success: true, message })
    } else {
      reject(new Error(message || '操作失败'))
    }
  }
})

function sendUserRequest(action, data = {}, timeout = 10000) {
  return new Promise((resolve, reject) => {
    const requestId = `user_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`
    const timer = setTimeout(() => {
      pendingUserRequests.delete(requestId)
      reject(new Error('请求超时'))
    }, timeout)

    pendingUserRequests.set(requestId, {
      resolve: (result) => { clearTimeout(timer); resolve(result) },
      reject: (error) => { clearTimeout(timer); reject(error) }
    })

    wsClient.send(action, { requestId, ...data })
  })
}

// 用户角色枚举
export const UserRole = {
  SUPER_ADMIN: 'super_admin',      // 超级管理员
  STRATEGY_MANAGER: 'strategy_manager'  // 策略管理员
}

// 用户角色中文名称
export const UserRoleNames = {
  [UserRole.SUPER_ADMIN]: '超级管理员',
  [UserRole.STRATEGY_MANAGER]: '策略管理员'
}

// 权限定义
export const Permissions = {
  // 策略权限
  STRATEGY_VIEW: 'strategy:view',
  STRATEGY_CREATE: 'strategy:create',
  STRATEGY_START: 'strategy:start',
  STRATEGY_STOP: 'strategy:stop',
  STRATEGY_DELETE: 'strategy:delete',

  // 账户权限
  ACCOUNT_VIEW: 'account:view',
  ACCOUNT_CREATE: 'account:create',
  ACCOUNT_EDIT: 'account:edit',
  ACCOUNT_DELETE: 'account:delete',
  ACCOUNT_SYNC: 'account:sync',

  // 订单权限
  ORDER_VIEW: 'order:view',
  ORDER_CREATE: 'order:create',
  ORDER_CANCEL: 'order:cancel',

  // 持仓权限
  POSITION_VIEW: 'position:view',
  POSITION_CLOSE: 'position:close',

  // 用户管理权限
  USER_VIEW: 'user:view',
  USER_CREATE: 'user:create',
  USER_EDIT: 'user:edit',
  USER_DELETE: 'user:delete'
}

// 角色权限映射
const RolePermissions = {
  [UserRole.SUPER_ADMIN]: [
    // 所有权限
    ...Object.values(Permissions)
  ],
  [UserRole.STRATEGY_MANAGER]: [
    // 查看 + 启动/停止策略 + 查看订单和持仓
    Permissions.STRATEGY_VIEW,
    Permissions.STRATEGY_START,
    Permissions.STRATEGY_STOP,
    Permissions.ORDER_VIEW,
    Permissions.POSITION_VIEW
  ]
}

export const useUserStore = defineStore('user', () => {
  // 状态
  const token = ref(localStorage.getItem('token') || '')
  const userInfo = ref(null)
  const users = ref([])
  const loading = ref(false)
  const allowedStrategies = ref([])  // 策略管理员被分配的策略列表

  // 计算属性
  const isLoggedIn = computed(() => !!token.value)

  const isSuperAdmin = computed(() =>
    userInfo.value?.role === UserRole.SUPER_ADMIN
  )

  const isStrategyManager = computed(() =>
    userInfo.value?.role === UserRole.STRATEGY_MANAGER
  )

  const userRole = computed(() => userInfo.value?.role || '')

  const userRoleName = computed(() =>
    UserRoleNames[userRole.value] || '未知'
  )

  const permissions = computed(() => {
    if (!userInfo.value) return []
    return RolePermissions[userInfo.value.role] || []
  })

  // 权限检查方法
  function hasPermission(permission) {
    return permissions.value.includes(permission)
  }

  function hasAnyPermission(permissionList) {
    return permissionList.some(p => hasPermission(p))
  }

  function hasAllPermissions(permissionList) {
    return permissionList.every(p => hasPermission(p))
  }

  // 登录
  async function login(credentials) {
    try {
      loading.value = true

      // 通过WebSocket调用后端认证
      return new Promise((resolve, reject) => {
        // 设置响应监听器
        const handleResponse = (event) => {
          const responseData = event.data || {}
          const isLoginResponse = responseData.type === 'login_response' || event.success !== undefined

          if (!isLoginResponse) {
            return // 不是登录响应，忽略
          }

          wsClient.off('response', handleResponse)
          wsClient.off('login_response', handleResponse)

          if (event.success || responseData.success) {
            // 登录成功
            const backendRole = responseData.user?.role || 'STRATEGY_MANAGER'
            // 映射后端角色到前端角色
            const roleMap = {
              'SUPER_ADMIN': UserRole.SUPER_ADMIN,
              'STRATEGY_MANAGER': UserRole.STRATEGY_MANAGER
            }

            const user = {
              id: Date.now(),
              username: responseData.user?.username || credentials.username,
              name: responseData.user?.username || credentials.username,
              role: roleMap[backendRole] || UserRole.STRATEGY_MANAGER,
              email: '',
              avatar: '',
              createdAt: new Date()
            }

            token.value = responseData.token || 'ws_token_' + Date.now()
            userInfo.value = user
            // 保存策略管理员的 allowed_strategies
            if (responseData.user?.allowed_strategies) {
              allowedStrategies.value = responseData.user.allowed_strategies
              localStorage.setItem('allowedStrategies', JSON.stringify(allowedStrategies.value))
            } else {
              allowedStrategies.value = []
              localStorage.removeItem('allowedStrategies')
            }
            localStorage.setItem('token', token.value)
            localStorage.setItem('userInfo', JSON.stringify(user))

            ElMessage.success('登录成功')
            loading.value = false
            resolve({ success: true, user })
          } else {
            loading.value = false
            const error = new Error(event.message || responseData.message || '登录失败')
            ElMessage.error(error.message)
            reject(error)
          }
        }

        // 监听响应
        wsClient.on('response', handleResponse)
        wsClient.on('login_response', handleResponse)

        // 发送登录请求
        const sent = wsClient.ws && wsClient.ws.readyState === WebSocket.OPEN
        if (sent) {
          wsClient.ws.send(JSON.stringify({
            type: 'login',
            username: credentials.username,
            password: credentials.password
          }))
        } else {
          // WebSocket未连接，使用Mock模式
          console.warn('WebSocket未连接，使用Mock模式')
          wsClient.off('response', handleResponse)
          wsClient.off('login_response', handleResponse)

          let mockUser
          if (credentials.username === 'admin' && credentials.password === 'admin') {
            mockUser = {
              id: 1,
              username: 'admin',
              name: '超级管理员',
              role: UserRole.SUPER_ADMIN,
              email: '',
              avatar: '',
              createdAt: new Date('2024-01-01')
            }
          } else {
            loading.value = false
            const error = new Error('用户名或密码错误')
            ElMessage.error(error.message)
            reject(error)
            return
          }

          const mockToken = 'mock_token_' + Date.now()
          token.value = mockToken
          userInfo.value = mockUser
          allowedStrategies.value = []
          localStorage.setItem('token', mockToken)
          localStorage.setItem('userInfo', JSON.stringify(mockUser))

          ElMessage.success('登录成功 (Mock模式)')
          loading.value = false
          resolve({ success: true, user: mockUser })
        }

        // 超时处理
        setTimeout(() => {
          wsClient.off('response', handleResponse)
          wsClient.off('login_response', handleResponse)
          if (loading.value) {
            loading.value = false
            const error = new Error('登录超时，请检查后端服务')
            ElMessage.error(error.message)
            reject(error)
          }
        }, 10000)
      })
    } catch (error) {
      loading.value = false
      ElMessage.error(error.message || '登录失败')
      throw error
    }
  }

  // 登出
  async function logout() {
    try {
      // 通过WebSocket通知后端登出
      if (wsClient.ws && wsClient.ws.readyState === WebSocket.OPEN && token.value) {
        wsClient.ws.send(JSON.stringify({
          type: 'logout',
          token: token.value
        }))
      }

      token.value = ''
      userInfo.value = null
      allowedStrategies.value = []

      localStorage.removeItem('token')
      localStorage.removeItem('userInfo')
      localStorage.removeItem('allowedStrategies')

      ElMessage.success('已退出登录')
    } catch (error) {
      console.error('登出失败:', error)
    }
  }

  // 获取用户信息
  async function fetchUserInfo() {
    try {
      if (import.meta.env.DEV) {
        // 开发环境从本地存储恢复
        const savedUserInfo = localStorage.getItem('userInfo')
        if (savedUserInfo) {
          userInfo.value = JSON.parse(savedUserInfo)
          return
        }
      }

      const res = await userApi.getUserInfo()
      userInfo.value = res.data
      localStorage.setItem('userInfo', JSON.stringify(res.data))
    } catch (error) {
      console.error('获取用户信息失败:', error)
      // 如果获取失败，清除登录状态
      await logout()
    }
  }

  // 获取用户列表（仅超级管理员，通过后端WebSocket）
  async function fetchUsers() {
    if (!hasPermission(Permissions.USER_VIEW)) {
      ElMessage.error('无权限查看用户列表')
      return
    }

    loading.value = true
    try {
      const res = await sendUserRequest('list_users', {})
      const list = Array.isArray(res.data) ? res.data : []
      users.value = list.map(u => ({
        username: u.username,
        role: u.role === 'SUPER_ADMIN' ? UserRole.SUPER_ADMIN : UserRole.STRATEGY_MANAGER,
        roleName: u.role === 'SUPER_ADMIN' ? '超级管理员' : '策略管理员',
        active: u.active,
        created_at: u.created_at,
        last_login: u.last_login,
        allowed_strategies: u.allowed_strategies || []
      }))
    } catch (error) {
      console.error('获取用户列表失败:', error)
      ElMessage.error(error.message || '获取用户列表失败')
    } finally {
      loading.value = false
    }
  }

  // 创建用户（仅超级管理员）
  async function createUser(data) {
    if (!hasPermission(Permissions.USER_CREATE)) {
      ElMessage.error('无权限创建用户')
      return { success: false }
    }

    try {
      const res = await sendUserRequest('add_user', {
        username: data.username,
        password: data.password,
        role: data.role || 'STRATEGY_MANAGER',
        allowed_strategies: data.allowed_strategies || []
      })
      ElMessage.success(res.message || '用户创建成功')
      return { success: true }
    } catch (error) {
      ElMessage.error(error.message || '创建失败')
      return { success: false }
    }
  }

  // 更新用户（仅超级管理员）
  async function updateUser(username, data) {
    if (!hasPermission(Permissions.USER_EDIT)) {
      ElMessage.error('无权限编辑用户')
      return { success: false }
    }

    try {
      const res = await sendUserRequest('update_user', {
        username: username,
        allowed_strategies: data.allowed_strategies || []
      })
      ElMessage.success(res.message || '用户更新成功')
      return { success: true }
    } catch (error) {
      ElMessage.error(error.message || '更新失败')
      return { success: false }
    }
  }

  // 删除用户（仅超级管理员）
  async function deleteUser(username) {
    if (!hasPermission(Permissions.USER_DELETE)) {
      ElMessage.error('无权限删除用户')
      return { success: false }
    }

    try {
      const res = await sendUserRequest('delete_user', { username })
      ElMessage.success(res.message || '用户已删除')
      return { success: true }
    } catch (error) {
      ElMessage.error(error.message || '删除失败')
      return { success: false }
    }
  }

  // 修改密码
  async function changePassword(data) {
    try {
      const res = await sendUserRequest('change_password', {
        old_password: data.oldPassword,
        new_password: data.newPassword
      })
      ElMessage.success('密码修改成功，请重新登录')
      logout()
      return { success: true }
    } catch (error) {
      ElMessage.error(error.message || '密码修改失败')
      return { success: false }
    }
  }

  // 初始化（从本地存储恢复）
  function init() {
    const savedToken = localStorage.getItem('token')
    const savedUserInfo = localStorage.getItem('userInfo')
    const savedAllowed = localStorage.getItem('allowedStrategies')

    if (savedToken && savedUserInfo) {
      token.value = savedToken
      userInfo.value = JSON.parse(savedUserInfo)
      if (savedAllowed) {
        allowedStrategies.value = JSON.parse(savedAllowed)
      }
    }
  }

  return {
    // 状态
    token,
    userInfo,
    users,
    loading,
    allowedStrategies,

    // 计算属性
    isLoggedIn,
    isSuperAdmin,
    isStrategyManager,
    userRole,
    userRoleName,
    permissions,

    // 方法
    hasPermission,
    hasAnyPermission,
    hasAllPermissions,
    login,
    logout,
    fetchUserInfo,
    fetchUsers,
    createUser,
    updateUser,
    deleteUser,
    changePassword,
    init
  }
})
