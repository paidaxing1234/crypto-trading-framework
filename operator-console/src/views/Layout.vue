<template>
  <el-container class="layout-container">
    <!-- 侧边栏 -->
    <el-aside :width="sidebarWidth" class="layout-aside">
      <div class="logo">
        <el-icon v-if="!collapsed" size="32"><TrendCharts /></el-icon>
        <span v-if="!collapsed" class="logo-text">实盘交易系统</span>
      </div>
      
      <el-menu
        :default-active="currentRoute"
        :collapse="collapsed"
        router
        class="sidebar-menu"
      >
        <el-menu-item index="/dashboard">
          <el-icon><DataAnalysis /></el-icon>
          <template #title>仪表板</template>
        </el-menu-item>
        
        <el-menu-item index="/strategy" v-permission="'strategy:view'">
          <el-icon><SetUp /></el-icon>
          <template #title>策略管理</template>
        </el-menu-item>

        <el-menu-item index="/strategy-logs">
          <el-icon><Notebook /></el-icon>
          <template #title>策略日志</template>
        </el-menu-item>

        <el-menu-item index="/account" v-permission="'account:view'">
          <el-icon><Wallet /></el-icon>
          <template #title>账户管理</template>
        </el-menu-item>
        
        <el-menu-item index="/users" v-permission="'user:view'">
          <el-icon><User /></el-icon>
          <template #title>用户管理</template>
        </el-menu-item>
        

        <el-menu-item index="/logs">
          <el-icon><Document /></el-icon>
          <template #title>系统日志</template>
        </el-menu-item>
      </el-menu>
      
      <div class="sidebar-footer">
        <el-button
          circle
          :icon="collapsed ? 'Expand' : 'Fold'"
          @click="toggleSidebar"
        />
      </div>
    </el-aside>
    
    <!-- 主内容区 -->
    <el-container class="main-container">
      <!-- 顶部栏 -->
      <el-header class="layout-header">
        <div class="header-left">
          <el-breadcrumb separator="/">
            <el-breadcrumb-item :to="{ path: '/' }">首页</el-breadcrumb-item>
            <el-breadcrumb-item>{{ currentPageTitle }}</el-breadcrumb-item>
          </el-breadcrumb>
        </div>
        
        <div class="header-right">
          <!-- WebSocket 连接状态 -->
          <div class="ws-status">
            <el-tooltip :content="wsConnected ? '已连接' : '未连接'">
              <el-icon :color="wsConnected ? '#67c23a' : '#f56c6c'">
                <Connection />
              </el-icon>
            </el-tooltip>
          </div>
          
          <!-- 通知 -->
          <el-badge :value="notifications.length" :hidden="notifications.length === 0">
            <el-button circle :icon="Bell" @click="showNotifications" />
          </el-badge>
          
          <!-- 主题切换 -->
          <el-switch
            v-model="isDark"
            inline-prompt
            :active-icon="Moon"
            :inactive-icon="Sunny"
            @change="toggleTheme"
          />
          
          <!-- 用户菜单 -->
          <el-dropdown @command="handleUserCommand">
            <div class="user-dropdown">
              <el-avatar :size="32" :icon="UserFilled" />
              <div class="user-info" v-if="!collapsed">
                <div class="user-name">{{ userStore.userInfo?.name }}</div>
                <div class="user-role">{{ userStore.userRoleName }}</div>
              </div>
            </div>
            <template #dropdown>
              <el-dropdown-menu>
                <el-dropdown-item disabled>
                  <div style="text-align: center;">
                    <div style="font-weight: bold;">{{ userStore.userInfo?.username }}</div>
                    <el-tag size="small" style="margin-top: 5px;">
                      {{ userStore.userRoleName }}
                    </el-tag>
                  </div>
                </el-dropdown-item>
                <el-dropdown-item divided command="changePassword">
                  <el-icon><Lock /></el-icon>
                  修改密码
                </el-dropdown-item>
                <el-dropdown-item command="logout">
                  <el-icon><SwitchButton /></el-icon>
                  退出登录
                </el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
        </div>
      </el-header>
      
      <!-- 主内容 -->
      <el-main class="layout-main">
        <router-view v-slot="{ Component }">
          <transition name="fade" mode="out-in">
            <component :is="Component" />
          </transition>
        </router-view>
      </el-main>
    </el-container>
  </el-container>
</template>

<script setup>
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessageBox } from 'element-plus'
import { useAppStore } from '@/stores/app'
import { useUserStore } from '@/stores/user'
import {
  DataAnalysis,
  SetUp,
  Wallet,
  User,
  Document,
  Notebook,
  TrendCharts,
  Connection,
  Bell,
  Moon,
  Sunny,
  UserFilled,
  Lock,
  SwitchButton,
  EditPen
} from '@element-plus/icons-vue'

const route = useRoute()
const router = useRouter()
const appStore = useAppStore()
const userStore = useUserStore()

const collapsed = computed(() => appStore.sidebarCollapsed)
const sidebarWidth = computed(() => collapsed.value ? '64px' : '200px')
const currentRoute = computed(() => route.path)
const currentPageTitle = computed(() => route.meta.title || '')
const wsConnected = computed(() => appStore.wsConnected)
const notifications = computed(() => appStore.notifications)

const isDark = ref(appStore.theme === 'dark')

function toggleSidebar() {
  appStore.toggleSidebar()
}

function toggleTheme(value) {
  appStore.setTheme(value ? 'dark' : 'light')
}

function showNotifications() {
  // TODO: 显示通知列表
  console.log('显示通知')
}

async function handleUserCommand(command) {
  if (command === 'logout') {
    try {
      await ElMessageBox.confirm(
        '确定要退出登录吗？',
        '提示',
        {
          confirmButtonText: '确定',
          cancelButtonText: '取消',
          type: 'warning'
        }
      )
      
      await userStore.logout()
      router.push('/login')
    } catch (error) {
      // 用户取消
    }
  } else if (command === 'changePassword') {
    // TODO: 打开修改密码对话框
    console.log('修改密码')
  }
}
</script>

<style lang="scss" scoped>
.layout-container {
  height: 100vh;
  background: var(--bg-base);

  &::before {
    content: '';
    position: absolute;
    inset: 0;
    background-image: url('data:image/svg+xml;utf8,%3Csvg viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg"%3E%3Cfilter id="noiseFilter"%3E%3CfeTurbulence type="fractalNoise" baseFrequency="0.65" numOctaves="3" stitchTiles="stitch"/%3E%3C/filter%3E%3Crect width="100%25" height="100%25" filter="url(%23noiseFilter)"/%3E%3C/svg%3E');
    opacity: var(--noise-opacity);
    pointer-events: none;
    z-index: 9999;
  }

  .layout-aside {
    background: var(--bg-surface);
    border-right: 1px solid var(--border-color);
    display: flex;
    flex-direction: column;
    transition: width 0.4s cubic-bezier(0.22, 1, 0.36, 1);
    position: relative;
    z-index: 10;
    box-shadow: 1px 0 10px rgba(0,0,0,0.02);

    .logo {
      height: 72px;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 12px;
      border-bottom: 1px solid var(--border-color);
      background: linear-gradient(180deg, rgba(255,255,255,0.03) 0%, transparent 100%);

      .el-icon {
        color: var(--accent-green);
        filter: drop-shadow(0 0 10px rgba(16, 185, 129, 0.4));
      }

      .logo-text {
        font-size: 16px;
        font-weight: 800;
        color: var(--text-primary);
        letter-spacing: -0.5px;
        text-transform: uppercase;
      }
    }

    .sidebar-menu {
      flex: 1;
      border-right: none;
      padding: 12px;

      :deep(.el-menu-item) {
        border-radius: var(--radius-sm);
        margin-bottom: 4px;
        height: 48px;
        line-height: 48px;
        transition: all 0.3s ease;

        .el-icon { font-size: 18px; margin-right: 12px; }
      }
    }

    .sidebar-footer {
      height: 60px;
      display: flex;
      align-items: center;
      justify-content: center;
      border-top: 1px solid var(--border-color);

      .el-button {
        color: var(--text-muted) !important;
        border-color: var(--border-color) !important;
        background: var(--bg-input) !important;
        transition: all 0.3s;
        &:hover {
          color: var(--accent-green) !important;
          border-color: var(--accent-green) !important;
          transform: rotate(180deg);
        }
      }
    }
  }

  .main-container {
    background: transparent;

    .layout-header {
      background: var(--glass-bg);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border-bottom: 1px solid var(--border-color);
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 0 24px;
      height: 64px;
      position: sticky;
      top: 0;
      z-index: 5;

      .header-right {
        display: flex;
        align-items: center;
        gap: 24px;

        .ws-status {
          display: flex;
          align-items: center;

          .el-icon {
            font-size: 18px;
            animation: breathe 2s infinite ease-in-out;
            filter: drop-shadow(0 0 8px currentColor);
          }
        }

        .user-dropdown {
          display: flex;
          align-items: center;
          gap: 12px;
          cursor: pointer;
          padding: 6px 12px;
          border-radius: var(--radius-sm);
          transition: background 0.3s;
          border: 1px solid transparent;

          &:hover {
            background: var(--bg-card-hover);
            border-color: var(--border-color);
          }

          .user-info {
            .user-name {
              font-size: 14px;
              font-weight: 600;
              color: var(--text-primary);
            }

            .user-role {
              font-size: 11px;
              color: var(--text-muted);
              font-family: var(--font-mono);
              text-transform: uppercase;
              letter-spacing: 0.5px;
            }
          }
        }
      }
    }

    .layout-main {
      padding: 24px;
      overflow-y: auto;
      scroll-behavior: smooth;
    }
  }
}

.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.4s ease, transform 0.4s ease;
}

.fade-enter-from {
  opacity: 0;
  transform: translateY(10px);
}
.fade-leave-to {
  opacity: 0;
  transform: translateY(-10px);
}
</style>

