<template>
  <slot v-if="hasPermission"></slot>
</template>

<script setup>
import { computed } from 'vue'
import { useUserStore } from '@/stores/user'

const props = defineProps({
  // 权限值
  permission: {
    type: [String, Array],
    required: true
  },
  // 是否需要全部权限（仅当permission为数组时有效）
  requireAll: {
    type: Boolean,
    default: false
  }
})

const userStore = useUserStore()

const hasPermission = computed(() => {
  if (Array.isArray(props.permission)) {
    if (props.requireAll) {
      return userStore.hasAllPermissions(props.permission)
    } else {
      return userStore.hasAnyPermission(props.permission)
    }
  } else {
    return userStore.hasPermission(props.permission)
  }
})
</script>

