#!/bin/bash

# ============================================
# SSL证书配置脚本（使用Let's Encrypt）
# ============================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}SSL证书配置（Let's Encrypt）${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# 检查root权限
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}请使用root权限运行此脚本${NC}"
    exit 1
fi

# 获取域名
read -p "请输入你的域名（例如: trading.example.com）: " DOMAIN
read -p "请输入你的邮箱（用于证书通知）: " EMAIL

if [ -z "$DOMAIN" ] || [ -z "$EMAIL" ]; then
    echo -e "${RED}域名和邮箱不能为空${NC}"
    exit 1
fi

# 安装certbot
echo -e "${YELLOW}[1/4] 安装Certbot...${NC}"
apt-get update
apt-get install -y certbot python3-certbot-nginx

# 获取证书
echo -e "${YELLOW}[2/4] 申请SSL证书...${NC}"
certbot --nginx -d ${DOMAIN} --email ${EMAIL} --agree-tos --non-interactive

# 测试自动续期
echo -e "${YELLOW}[3/4] 测试证书自动续期...${NC}"
certbot renew --dry-run

# 设置自动续期
echo -e "${YELLOW}[4/4] 配置自动续期...${NC}"
# Certbot会自动添加cron任务，我们只需确认
if systemctl list-timers | grep -q certbot; then
    echo -e "${GREEN}✓ 自动续期已配置${NC}"
else
    echo -e "${YELLOW}⚠ 请手动配置自动续期${NC}"
fi

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}SSL证书配置完成！${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "域名: ${DOMAIN}"
echo "证书位置: /etc/letsencrypt/live/${DOMAIN}/"
echo "访问地址: https://${DOMAIN}"
echo ""
echo -e "${YELLOW}证书有效期为90天，Certbot会自动续期${NC}"
echo ""

