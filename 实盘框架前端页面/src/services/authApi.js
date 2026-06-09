/**
 * @file authApi.js
 * @brief 认证API服务 - 与C++后端SecureFrontendHandler对接
 */

// 后端ZMQ服务地址（通过HTTP代理或WebSocket桥接）
const AUTH_API_BASE = import.meta.env.VITE_AUTH_API_URL || 'http://localhost:8080/api/auth'

/**
 * 发送认证请求到后端
 * @param {Object} data - 请求数据
 * @returns {Promise<Object>} - 响应数据
 */
async function sendAuthRequest(data) {
  try {
    const response = await fetch(AUTH_API_BASE, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(data)
    })

    const result = await response.json()

    if (result.code !== 200) {
      throw new Error(result.message || '请求失败')
    }

    return result
  } catch (error) {
    console.error('Auth API Error:', error)
    throw error
  }
}

/**
 * 用户登录
 * @param {string} username - 用户名
 * @param {string} password - 密码
 * @returns {Promise<{token: string, user: Object}>}
 */
export async function login(username, password) {
  const result = await sendAuthRequest({
    type: 'login',
    username,
    password
  })

  return {
    token: result.token,
    user: result.user
  }
}

/**
 * 用户登出
 * @param {string} token - JWT Token
 */
export async function logout(token) {
  await sendAuthRequest({
    type: 'logout',
    token
  })
}

/**
 * 获取用户信息
 * @param {string} token - JWT Token
 * @returns {Promise<Object>} - 用户信息
 */
export async function getUserInfo(token) {
  const result = await sendAuthRequest({
    type: 'get_user_info',
    token
  })

  return result.user
}

/**
 * 修改密码
 * @param {string} token - JWT Token
 * @param {string} oldPassword - 旧密码
 * @param {string} newPassword - 新密码
 */
export async function changePassword(token, oldPassword, newPassword) {
  await sendAuthRequest({
    type: 'change_password',
    token,
    old_password: oldPassword,
    new_password: newPassword
  })
}

/**
 * 添加用户（管理员）
 * @param {string} token - JWT Token
 * @param {string} username - 用户名
 * @param {string} password - 密码
 * @param {string} role - 角色 (SUPER_ADMIN, ADMIN, TRADER, VIEWER)
 */
export async function addUser(token, username, password, role) {
  await sendAuthRequest({
    type: 'add_user',
    token,
    username,
    password,
    role
  })
}

/**
 * 获取用户列表（管理员）
 * @param {string} token - JWT Token
 * @returns {Promise<Array>} - 用户列表
 */
export async function listUsers(token) {
  const result = await sendAuthRequest({
    type: 'list_users',
    token
  })

  return result.users
}

/**
 * 注册交易账户
 * @param {string} token - JWT Token
 * @param {Object} accountData - 账户数据
 */
export async function registerAccount(token, accountData) {
  await sendAuthRequest({
    type: 'register_account',
    token,
    ...accountData
  })
}

/**
 * 注销交易账户
 * @param {string} token - JWT Token
 * @param {string} strategyId - 策略ID
 * @param {string} exchange - 交易所
 */
export async function unregisterAccount(token, strategyId, exchange) {
  await sendAuthRequest({
    type: 'unregister_account',
    token,
    strategy_id: strategyId,
    exchange
  })
}

/**
 * 获取账户列表
 * @param {string} token - JWT Token
 * @returns {Promise<Object>} - 账户统计
 */
export async function listAccounts(token) {
  const result = await sendAuthRequest({
    type: 'list_accounts',
    token
  })

  return {
    okxCount: result.okx_count,
    binanceCount: result.binance_count,
    total: result.total
  }
}

export default {
  login,
  logout,
  getUserInfo,
  changePassword,
  addUser,
  listUsers,
  registerAccount,
  unregisterAccount,
  listAccounts
}
