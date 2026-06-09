/**
 * 权限控制指令
 * v-permission="'permission:name'"  单个权限
 * v-permission="['permission:name1', 'permission:name2']"  多个权限（任一满足）
 * v-permission.all="['permission:name1', 'permission:name2']"  多个权限（全部满足）
 */

import { useUserStore } from '@/stores/user'

export const permission = {
  mounted(el, binding) {
    const { value, modifiers } = binding
    const userStore = useUserStore()
    
    if (!value) {
      throw new Error('v-permission需要指定权限值')
    }
    
    let hasPermission = false
    
    if (Array.isArray(value)) {
      // 多个权限
      if (modifiers.all) {
        // 需要全部权限
        hasPermission = userStore.hasAllPermissions(value)
      } else {
        // 任一权限即可
        hasPermission = userStore.hasAnyPermission(value)
      }
    } else {
      // 单个权限
      hasPermission = userStore.hasPermission(value)
    }
    
    if (!hasPermission) {
      // 没有权限，移除元素
      el.parentNode?.removeChild(el)
    }
  }
}

/**
 * 角色控制指令
 * v-role="'super_admin'"  单个角色
 * v-role="['super_admin', 'viewer']"  多个角色
 */
export const role = {
  mounted(el, binding) {
    const { value } = binding
    const userStore = useUserStore()
    
    if (!value) {
      throw new Error('v-role需要指定角色值')
    }
    
    const userRole = userStore.userRole
    let hasRole = false
    
    if (Array.isArray(value)) {
      hasRole = value.includes(userRole)
    } else {
      hasRole = userRole === value
    }
    
    if (!hasRole) {
      el.parentNode?.removeChild(el)
    }
  }
}

