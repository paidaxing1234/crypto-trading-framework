# 部署架构建议

> 从开发环境迁移到生产服务器的完整步骤。

---

## 一、目标架构

```
                        公网
                         |
                    [防火墙 UFW]
                    仅放行 22,443
                         |
                   [nginx :443]
                   SSL/TLS终结
                   /          \
           静态文件           WebSocket反代
          (前端dist/)        proxy_pass :8002
                              |
                   [trading_server :8002]
                   bind 127.0.0.1
                         |
                   [Redis :6379]
                   bind 127.0.0.1
                   requirepass
                         |
              [data_recorder / fast_kline_filler]
              [策略进程 five_mom_factor / ret_skew]
```

---

## 二、迁移步骤

### 步骤1: 基础环境

```bash
# 系统更新
sudo apt update && sudo apt upgrade -y

# 安装依赖
sudo apt install -y build-essential cmake redis-server nginx certbot python3-certbot-nginx

# 创建专用用户（不用 root 跑服务）
sudo useradd -m -s /bin/bash trading
sudo su - trading
```

### 步骤2: 部署代码

```bash
# 克隆代码（不要把 strategy_configs 带上，手动配置）
git clone <repo_url> ~/trading-framework
cd ~/trading-framework

# 创建环境变量文件
cat > ~/.trading_env << 'EOF'
export REDIS_PASSWORD="$(openssl rand -hex 16)"
export JWT_SECRET="$(openssl rand -hex 32)"
export ADMIN_PASSWORD="<设置强密码>"
export BINANCE_MAIN_API_KEY="<key>"
export BINANCE_MAIN_SECRET_KEY="<secret>"
EOF
chmod 600 ~/.trading_env
```

### 步骤3: 按 01_security_checklist.md 逐项加固

### 步骤4: 按 02_performance_cpp.md 和 03_performance_python.md 逐项优化

### 步骤5: 配置 nginx

```nginx
server {
    listen 443 ssl;
    server_name your-domain.com;

    ssl_certificate /etc/letsencrypt/live/your-domain.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/your-domain.com/privkey.pem;

    # 前端静态文件
    location / {
        root /home/trading/trading-framework/实盘框架前端页面/dist;
        try_files $uri $uri/ /index.html;
    }

    # WebSocket 反代
    location /ws {
        proxy_pass http://127.0.0.1:8002;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_read_timeout 86400;
    }
}

server {
    listen 80;
    server_name your-domain.com;
    return 301 https://$host$request_uri;
}
```

### 步骤6: 构建前端

```bash
cd 实盘框架前端页面
# 创建生产环境配置
echo 'VITE_WS_URL=wss://your-domain.com/ws' > .env.production
npm install && npm run build
```

### 步骤7: 进程管理（systemd）

为每个服务创建 systemd unit，替代 tmux 手动管理：

```ini
# /etc/systemd/system/trading-server.service
[Unit]
Description=Trading Server
After=redis.service

[Service]
User=trading
WorkingDirectory=/home/trading/trading-framework/cpp
EnvironmentFile=/home/trading/.trading_env
ExecStart=/home/trading/trading-framework/cpp/build/trading_server_full
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

类似地为 data_recorder、fast_kline_filler、每个策略进程创建 unit。

---

## 三、监控建议

| 项目 | 方法 |
|------|------|
| 进程存活 | systemd 自动重启 + 飞书告警 |
| Redis 内存 | `redis-cli info memory`，设置 maxmemory |
| 磁盘空间 | 日志文件 logrotate 配置 |
| 网络连通 | 已有 network_monitor，确保告警邮件能发出 |
| 策略异常 | 日志中 ERROR 关键字监控 |
