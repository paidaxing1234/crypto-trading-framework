import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { resolve } from 'path'

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@': resolve(__dirname, 'src')
    }
  },
  server: {
    port: 3000,
    strictPort: false, // 如果端口被占用，自动尝试下一个
    host: true, // 允许外部访问
    proxy: {
      '/ws': {
        target: 'ws://127.0.0.1:8002',
        ws: true
      },
      // 统计 API (stats_api.py, 默认 :8003) —— 同源代理, 避免 CORS / https 混合内容。
      // 生产环境由 nginx 加一段: location /stats-api/ { proxy_pass http://127.0.0.1:8003/; }
      '/stats-api': {
        target: 'http://127.0.0.1:8003',
        changeOrigin: true,
        rewrite: (p) => p.replace(/^\/stats-api/, '')
      }
    }
  },
  build: {
    outDir: 'dist',
    assetsDir: 'assets',
    sourcemap: false
  }
  // 注意：VITE_WS_URL 由 .env.development 或 .env.local 自动加载
  // 不需要在 define 中手动设置
})

