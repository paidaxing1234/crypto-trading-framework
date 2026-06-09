<template>
  <el-config-provider :locale="locale">
    <router-view />
  </el-config-provider>
</template>

<script setup>
import { ref } from 'vue'
import zhCn from 'element-plus/es/locale/lang/zh-cn'

const locale = ref(zhCn)
</script>

<style lang="scss">
:root {
  /* ── 白天模式（默认） ── */
  --bg-base: #f0f2f5;
  --bg-surface: #ffffff;
  --bg-elevated: #ffffff;
  --bg-card: #ffffff;
  --bg-card-hover: #f7f8fa;
  --bg-input: #f5f6f8;

  --border-color: #e2e5ea;
  --border-glow: rgba(16, 185, 129, 0.25);
  --border-active: rgba(16, 185, 129, 0.5);

  --text-primary: #111827;
  --text-secondary: #4b5563;
  --text-muted: #9ca3af;

  --accent-green: #10b981;
  --accent-red: #ef4444;
  --accent-blue: #3b82f6;
  --accent-orange: #f59e0b;
  --accent-purple: #8b5cf6;
  --accent-cyan: #06b6d4;

  --primary-color: #10b981;
  --success-color: #10b981;
  --danger-color: #ef4444;
  --warning-color: #f59e0b;
  --info-color: #3b82f6;

  --shadow-sm: 0 1px 3px rgba(0, 0, 0, 0.06), 0 1px 2px rgba(0, 0, 0, 0.04);
  --shadow-md: 0 4px 24px rgba(0, 0, 0, 0.08);
  --shadow-lg: 0 8px 40px rgba(0, 0, 0, 0.1);
  --shadow-glow: 0 0 24px rgba(16, 185, 129, 0.06);
  --shadow-card: 0 1px 3px rgba(0,0,0,0.04);

  --font-ui: 'Outfit', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  --font-mono: 'IBM Plex Mono', 'Consolas', 'Monaco', monospace;
  --radius: 12px;
  --radius-sm: 8px;
  --radius-lg: 16px;
  --log-bg: #1a1b26;
  --log-text: #c0caf5;

  --glass-bg: rgba(255, 255, 255, 0.72);
  --glass-border: rgba(255, 255, 255, 0.3);
  --noise-opacity: 0.015;
}

/* ── 暗色模式 ── */
html.dark {
  --bg-base: #0b0e14;
  --bg-surface: #111620;
  --bg-elevated: #161d2b;
  --bg-card: #111620;
  --bg-card-hover: #161d2b;
  --bg-input: #0d1019;

  --border-color: #1e2736;
  --border-glow: rgba(52, 211, 153, 0.25);
  --border-active: rgba(52, 211, 153, 0.5);

  --text-primary: #e5e7eb;
  --text-secondary: #9ca3af;
  --text-muted: #6b7280;

  --accent-green: #34d399;
  --accent-red: #f87171;
  --accent-blue: #60a5fa;
  --accent-orange: #fbbf24;
  --accent-purple: #a78bfa;
  --accent-cyan: #22d3ee;

  --primary-color: #34d399;
  --success-color: #34d399;
  --danger-color: #f87171;
  --warning-color: #fbbf24;
  --info-color: #60a5fa;

  --shadow-sm: 0 2px 8px rgba(0, 0, 0, 0.5);
  --shadow-md: 0 4px 24px rgba(0, 0, 0, 0.6);
  --shadow-lg: 0 8px 48px rgba(0, 0, 0, 0.7);
  --shadow-glow: 0 0 32px rgba(52, 211, 153, 0.08);
  --shadow-card: 0 2px 12px rgba(0,0,0,0.3);

  --log-bg: #0b0e14;
  --log-text: #e5e7eb;
  --glass-bg: rgba(17, 22, 32, 0.75);
  --glass-border: rgba(30, 39, 54, 0.6);
  --noise-opacity: 0.03;
}

* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

html,
body,
#app {
  height: 100%;
  font-family: var(--font-ui);
  background: var(--bg-base);
  color: var(--text-primary);
  -webkit-font-smoothing: antialiased;
  -moz-osx-font-smoothing: grayscale;
}

/* ── 滚动条 ── */
::-webkit-scrollbar { width: 5px; height: 5px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: var(--text-muted); border-radius: 4px; opacity: 0.25; }
::-webkit-scrollbar-thumb:hover { opacity: 0.4; }

/* ── 通用文本色 ── */
.text-success { color: var(--accent-green) !important; }
.text-danger { color: var(--accent-red) !important; }
.text-warning { color: var(--accent-orange) !important; }
.text-info { color: var(--accent-blue) !important; }

/* ── 数字字体 ── */
.stat-value, .price, [class*="stat-value"] {
  font-family: var(--font-mono);
  font-variant-numeric: tabular-nums;
}

/* ── Element Plus 覆写（使用 CSS 变量，自动适应明暗） ── */
.el-card {
  --el-card-bg-color: var(--bg-card);
  background: var(--bg-card) !important;
  border: 1px solid var(--border-color) !important;
  border-radius: var(--radius) !important;
  color: var(--text-primary) !important;
  box-shadow: var(--shadow-card) !important;
  transition: box-shadow 0.3s, border-color 0.3s, transform 0.3s;
}
.el-card__header {
  border-bottom: 1px solid var(--border-color) !important;
  color: var(--text-primary) !important;
  font-weight: 600;
}

.el-table {
  --el-table-bg-color: var(--bg-card);
  --el-table-tr-bg-color: var(--bg-card);
  --el-table-header-bg-color: var(--bg-base);
  --el-table-row-hover-bg-color: var(--bg-card-hover);
  --el-table-border-color: var(--border-color);
  --el-table-text-color: var(--text-primary);
  --el-table-header-text-color: var(--text-secondary);
  color: var(--text-primary);
  font-size: 13px;
}
.el-table th.el-table__cell {
  background: var(--bg-base) !important;
  font-weight: 600;
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 0.8px;
  color: var(--text-muted) !important;
}

.el-menu {
  background-color: transparent !important;
  border-right: none !important;
}
.el-menu-item {
  color: var(--text-secondary) !important;
  font-weight: 500;
  font-size: 13.5px;
  letter-spacing: 0.2px;
  &:hover, &:focus {
    background: rgba(16, 185, 129, 0.06) !important;
    color: var(--accent-green) !important;
  }
  &.is-active {
    color: var(--accent-green) !important;
    background: linear-gradient(90deg, rgba(16, 185, 129, 0.12) 0%, transparent 100%) !important;
    border-right: 2.5px solid var(--accent-green) !important;
    font-weight: 600;
  }
}

.el-input__wrapper {
  background: var(--bg-input) !important;
  border: 1px solid var(--border-color) !important;
  box-shadow: none !important;
  border-radius: var(--radius-sm) !important;
  transition: border-color 0.25s, box-shadow 0.25s;
  &:hover { border-color: var(--border-glow) !important; }
  &.is-focus { border-color: var(--accent-green) !important; box-shadow: 0 0 0 3px rgba(16,185,129,0.1) !important; }
}
.el-input__inner { color: var(--text-primary) !important; }
.el-input__prefix .el-icon { color: var(--text-muted) !important; }
.el-input .el-input__wrapper .el-input__inner::placeholder { color: var(--text-muted) !important; }

.el-select .el-select__wrapper {
  background: var(--bg-input) !important;
  border: 1px solid var(--border-color) !important;
  box-shadow: none !important;
  color: var(--text-primary) !important;
  border-radius: var(--radius-sm) !important;
}

.el-button--primary {
  --el-button-bg-color: var(--accent-green);
  --el-button-border-color: var(--accent-green);
  --el-button-hover-bg-color: var(--accent-green);
  --el-button-hover-border-color: var(--accent-green);
  background: linear-gradient(135deg, var(--accent-green), #059669) !important;
  border-color: transparent !important;
  color: #fff !important;
  font-weight: 600;
  border-radius: var(--radius-sm) !important;
  transition: all 0.25s;
  &:hover { filter: brightness(1.12); box-shadow: 0 4px 16px rgba(16, 185, 129, 0.3); transform: translateY(-1px); }
}

.el-tag {
  font-family: var(--font-mono);
  font-size: 11px;
  font-weight: 500;
  border-radius: 6px !important;
  padding: 0 10px;
}
.el-tag--success { background: rgba(16, 185, 129, 0.1) !important; color: var(--accent-green) !important; border-color: transparent !important; }
.el-tag--danger { background: rgba(239, 68, 68, 0.1) !important; color: var(--accent-red) !important; border-color: transparent !important; }
.el-tag--warning { background: rgba(245, 158, 11, 0.1) !important; color: var(--accent-orange) !important; border-color: transparent !important; }
.el-tag--info { background: rgba(107,114,128,0.08) !important; color: var(--text-secondary) !important; border-color: transparent !important; }
.el-tag--primary { background: rgba(16, 185, 129, 0.1) !important; color: var(--accent-green) !important; border-color: transparent !important; }

.el-dialog {
  --el-dialog-bg-color: var(--bg-elevated);
  background: var(--bg-elevated) !important;
  border: 1px solid var(--border-color) !important;
  border-radius: var(--radius-lg) !important;
  box-shadow: var(--shadow-lg) !important;
}
.el-dialog__title { color: var(--text-primary) !important; font-weight: 700; }
.el-dialog__headerbtn .el-dialog__close { color: var(--text-muted) !important; }

.el-form-item__label { color: var(--text-primary) !important; font-weight: 500; }
.el-breadcrumb__inner { color: var(--text-secondary) !important; }
.el-breadcrumb__separator { color: var(--text-muted) !important; }

.el-dropdown-menu {
  background: var(--bg-elevated) !important;
  border: 1px solid var(--border-color) !important;
  border-radius: var(--radius) !important;
  box-shadow: var(--shadow-md) !important;
}
.el-dropdown-menu__item {
  color: var(--text-primary) !important;
  &:hover { background: var(--bg-card-hover) !important; color: var(--accent-green) !important; }
}

.el-switch.is-checked .el-switch__core { background-color: var(--accent-green) !important; border-color: var(--accent-green) !important; }
.el-checkbox__label { color: var(--text-primary) !important; }
.el-empty__description p { color: var(--text-secondary) !important; }
.el-loading-mask { background: rgba(255,255,255,0.6) !important; backdrop-filter: blur(4px); }
html.dark .el-loading-mask { background: rgba(11,14,20,0.7) !important; }

.el-popper.is-light { background: var(--bg-elevated) !important; border: 1px solid var(--border-color) !important; color: var(--text-primary) !important; border-radius: var(--radius) !important; }

.el-select-dropdown {
  background: var(--bg-elevated) !important;
  border: 1px solid var(--border-color) !important;
  border-radius: var(--radius) !important;
}
.el-select-dropdown__item {
  color: var(--text-primary) !important;
  &:hover, &.is-hovering { background: var(--bg-card-hover) !important; }
  &.is-selected { color: var(--accent-green) !important; font-weight: 600; }
}

.el-badge__content { background-color: var(--accent-red) !important; }

.el-pagination {
  --el-pagination-bg-color: transparent;
  --el-pagination-button-bg-color: var(--bg-input);
  --el-pagination-text-color: var(--text-secondary);
  --el-pagination-hover-color: var(--accent-green);
}

/* ── 暗色模式 tag 颜色加强 ── */
html.dark {
  .el-tag--success { background: rgba(52, 211, 153, 0.12) !important; }
  .el-tag--danger { background: rgba(248, 113, 113, 0.12) !important; }
  .el-tag--warning { background: rgba(251, 191, 36, 0.12) !important; }
  .el-tag--info { background: rgba(107, 114, 128, 0.12) !important; }
  .el-tag--primary { background: rgba(52, 211, 153, 0.12) !important; }
}

/* ── 动画 ── */
@keyframes pulse-glow {
  0%, 100% { box-shadow: 0 0 8px rgba(52, 211, 153, 0.1); }
  50% { box-shadow: 0 0 20px rgba(52, 211, 153, 0.25); }
}
@keyframes fadeInUp {
  from { opacity: 0; transform: translateY(16px); }
  to { opacity: 1; transform: translateY(0); }
}
@keyframes fadeIn {
  from { opacity: 0; }
  to { opacity: 1; }
}
@keyframes slideInLeft {
  from { opacity: 0; transform: translateX(-20px); }
  to { opacity: 1; transform: translateX(0); }
}
@keyframes shimmer {
  0% { background-position: -200% 0; }
  100% { background-position: 200% 0; }
}
@keyframes price-flash-up {
  0% { background: rgba(52, 211, 153, 0.25); }
  100% { background: transparent; }
}
@keyframes price-flash-down {
  0% { background: rgba(248, 113, 113, 0.25); }
  100% { background: transparent; }
}
@keyframes breathe {
  0%, 100% { opacity: 0.4; }
  50% { opacity: 1; }
}
.animate-in {
  animation: fadeInUp 0.5s cubic-bezier(0.22, 1, 0.36, 1) both;
}

/* ── 毛玻璃卡片 ── */
.glass-card {
  background: var(--glass-bg);
  backdrop-filter: blur(16px) saturate(1.4);
  -webkit-backdrop-filter: blur(16px) saturate(1.4);
  border: 1px solid var(--glass-border);
  border-radius: var(--radius);
}
</style>

