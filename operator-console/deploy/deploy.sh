#!/bin/bash

# ============================================
# 实盘交易管理系统 - Linux部署脚本
# ============================================

set -e  # 遇到错误立即退出

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 配置变量
PROJECT_NAME="trading-system"
DEPLOY_DIR="/var/www/${PROJECT_NAME}"
NGINX_CONF="/etc/nginx/sites-available/${PROJECT_NAME}"
NGINX_ENABLED="/etc/nginx/sites-enabled/${PROJECT_NAME}"
BACKUP_DIR="/var/backups/${PROJECT_NAME}"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}实盘交易管理系统 - 部署脚本${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# 检查是否为root用户
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}请使用root权限运行此脚本${NC}"
    echo "使用: sudo ./deploy.sh"
    exit 1
fi

# 1. 检查依赖
echo -e "${YELLOW}[1/7] 检查系统依赖...${NC}"
command -v nginx >/dev/null 2>&1 || {
    echo -e "${YELLOW}未检测到Nginx，正在安装...${NC}"
    apt-get update
    apt-get install -y nginx
}
echo -e "${GREEN}✓ Nginx已安装${NC}"

# 2. 创建目录
echo -e "${YELLOW}[2/7] 创建部署目录...${NC}"
mkdir -p ${DEPLOY_DIR}
mkdir -p ${BACKUP_DIR}
echo -e "${GREEN}✓ 目录创建完成${NC}"

# 3. 备份旧版本（如果存在）
if [ -d "${DEPLOY_DIR}/dist" ]; then
    echo -e "${YELLOW}[3/7] 备份旧版本...${NC}"
    BACKUP_FILE="${BACKUP_DIR}/backup_$(date +%Y%m%d_%H%M%S).tar.gz"
    tar -czf ${BACKUP_FILE} -C ${DEPLOY_DIR} dist
    echo -e "${GREEN}✓ 备份保存至: ${BACKUP_FILE}${NC}"
else
    echo -e "${YELLOW}[3/7] 跳过备份（首次部署）${NC}"
fi

# 4. 复制构建文件
echo -e "${YELLOW}[4/7] 部署前端文件...${NC}"
if [ ! -d "./dist" ]; then
    echo -e "${RED}错误: 未找到dist目录${NC}"
    echo "请先运行: npm run build"
    exit 1
fi

rm -rf ${DEPLOY_DIR}/dist
cp -r ./dist ${DEPLOY_DIR}/
chown -R www-data:www-data ${DEPLOY_DIR}
chmod -R 755 ${DEPLOY_DIR}
echo -e "${GREEN}✓ 前端文件部署完成${NC}"

# 5. 配置Nginx
echo -e "${YELLOW}[5/7] 配置Nginx...${NC}"
if [ -f "./deploy/nginx.conf" ]; then
    cp ./deploy/nginx.conf ${NGINX_CONF}
    
    # 创建软链接
    ln -sf ${NGINX_CONF} ${NGINX_ENABLED}
    
    # 测试Nginx配置
    nginx -t
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ Nginx配置有效${NC}"
    else
        echo -e "${RED}错误: Nginx配置无效${NC}"
        exit 1
    fi
else
    echo -e "${YELLOW}⚠ 未找到nginx.conf，跳过Nginx配置${NC}"
fi

# 6. 重启Nginx
echo -e "${YELLOW}[6/7] 重启Nginx...${NC}"
systemctl restart nginx
systemctl status nginx --no-pager
echo -e "${GREEN}✓ Nginx重启成功${NC}"

# 7. 验证部署
echo -e "${YELLOW}[7/7] 验证部署...${NC}"
if [ -f "${DEPLOY_DIR}/dist/index.html" ]; then
    echo -e "${GREEN}✓ index.html存在${NC}"
else
    echo -e "${RED}错误: index.html不存在${NC}"
    exit 1
fi

# 显示部署信息
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}部署完成！${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "部署目录: ${DEPLOY_DIR}"
echo "Nginx配置: ${NGINX_CONF}"
echo "备份目录: ${BACKUP_DIR}"
echo ""
echo -e "${YELLOW}注意事项:${NC}"
echo "1. 请修改Nginx配置中的域名: ${NGINX_CONF}"
echo "2. 请配置SSL证书（推荐使用Let's Encrypt）"
echo "3. 请确保后端API服务正在运行"
echo "4. 请配置防火墙允许80和443端口"
echo ""
echo -e "${GREEN}访问地址: http://your-domain.com${NC}"
echo ""

