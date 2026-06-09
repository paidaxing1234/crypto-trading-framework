<template>
  <div class="not-found">
    <div class="glitch-text">404</div>
    <div class="not-found-sub">PAGE_NOT_FOUND</div>
    <el-button type="primary" @click="goHome">返回首页</el-button>
  </div>
</template>

<script setup>
import { useRouter } from 'vue-router'

const router = useRouter()

const goHome = () => {
  router.push('/dashboard')
}
</script>

<style scoped>
.not-found {
  display: flex;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  min-height: 100vh;
  background: var(--bg-base);
  position: relative;
  overflow: hidden;
}

.not-found::before {
  content: '';
  position: absolute;
  inset: 0;
  background-image:
    linear-gradient(rgba(16, 185, 129, 0.025) 1px, transparent 1px),
    linear-gradient(90deg, rgba(16, 185, 129, 0.025) 1px, transparent 1px);
  background-size: 80px 80px;
  mask-image: radial-gradient(ellipse at center, black 20%, transparent 60%);
}

.not-found::after {
  content: '';
  position: absolute;
  width: 500px;
  height: 500px;
  background: radial-gradient(circle, rgba(16, 185, 129, 0.06) 0%, transparent 70%);
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  pointer-events: none;
  animation: pulse-glow 4s ease-in-out infinite;
}

.glitch-text {
  font-family: var(--font-mono);
  font-size: 140px;
  font-weight: 900;
  color: var(--text-primary);
  position: relative;
  z-index: 1;
  text-shadow: 0 0 60px rgba(16, 185, 129, 0.12);
  animation: glitch 2.5s infinite;
  letter-spacing: -8px;
  line-height: 1;
}

.not-found-sub {
  color: var(--text-muted);
  font-size: 14px;
  margin: 20px 0 40px;
  z-index: 1;
  font-family: var(--font-mono);
  letter-spacing: 4px;
  font-weight: 500;
}

@keyframes glitch {
  0%, 90%, 100% { text-shadow: 0 0 60px rgba(16, 185, 129, 0.12); }
  92% { text-shadow: -3px 0 var(--accent-red), 3px 0 var(--accent-cyan); }
  94% { text-shadow: 3px 0 var(--accent-red), -3px 0 var(--accent-cyan); }
  96% { text-shadow: 0 0 60px rgba(16, 185, 129, 0.12); }
}
</style>
