#!/bin/bash

# 实盘框架前端 - WSL启动脚本
# 使用方法: ./start-wsl.sh

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印带颜色的信息
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 打印标题
echo -e "${GREEN}"
echo "╔══════════════════════════════════════════╗"
echo "║   实盘交易框架前端 - WSL启动脚本        ║"
echo "╚══════════════════════════════════════════╝"
echo -e "${NC}"

# 检查Node.js
print_info "检查Node.js环境..."
if ! command -v node &> /dev/null; then
    print_error "Node.js未安装！"
    echo ""
    echo "请先安装Node.js:"
    echo "  curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.0/install.sh | bash"
    echo "  source ~/.bashrc"
    echo "  nvm install 18"
    exit 1
fi

NODE_VERSION=$(node -v)
print_success "Node.js版本: $NODE_VERSION"

# 检查npm
if ! command -v npm &> /dev/null; then
    print_error "npm未安装！"
    exit 1
fi

NPM_VERSION=$(npm -v)
print_success "npm版本: $NPM_VERSION"

# 获取脚本所在目录
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
print_info "项目路径: $SCRIPT_DIR"

# 进入项目目录
cd "$SCRIPT_DIR"

# 检查package.json
if [ ! -f "package.json" ]; then
    print_error "package.json不存在！请检查项目路径。"
    exit 1
fi

# 检查node_modules
if [ ! -d "node_modules" ]; then
    print_warning "node_modules不存在，开始安装依赖..."
    echo ""
    print_info "正在安装依赖，这可能需要几分钟..."
    
    # 询问是否使用国内镜像
    read -p "是否使用国内npm镜像加速？(y/n, 默认:y): " use_mirror
    use_mirror=${use_mirror:-y}
    
    if [ "$use_mirror" = "y" ] || [ "$use_mirror" = "Y" ]; then
        print_info "配置npm镜像..."
        npm config set registry https://registry.npmmirror.com
    fi
    
    npm install
    
    if [ $? -ne 0 ]; then
        print_error "依赖安装失败！"
        exit 1
    fi
    
    print_success "依赖安装完成！"
else
    print_success "依赖已安装"
fi

# 检查.env.development
if [ ! -f ".env.development" ]; then
    print_warning ".env.development不存在，创建默认配置..."
    cat > .env.development << 'EOF'
# WebSocket服务器地址
VITE_WS_URL=ws://localhost:8002
EOF
    print_success "已创建 .env.development"
fi

# 检查C++后端连接
print_info "检查C++后端连接(ws://localhost:8002)..."

# 检查端口8002
if command -v nc &> /dev/null; then
    if nc -z localhost 8002 2>/dev/null; then
        print_success "C++后端已运行 ✓"
    else
        print_warning "C++后端(8002端口)未检测到"
        echo ""
        echo "请先启动C++后端:"
        echo "  cd ../cpp/build"
        echo "  ./ui_server"
        echo ""
        read -p "是否继续启动前端？(y/n): " continue_start
        if [ "$continue_start" != "y" ] && [ "$continue_start" != "Y" ]; then
            exit 0
        fi
    fi
else
    print_warning "无法检测端口状态(nc命令不可用)"
fi

# 显示访问信息
echo ""
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}准备启动开发服务器...${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo "📍 前端地址: http://localhost:3000"
echo "🔌 WebSocket: ws://localhost:8002"
echo "   (如果3000被占用会自动换端口)"
echo ""
echo "💡 提示:"
echo "  • 按 Ctrl+C 停止服务器"
echo "  • 按 o + Enter 在浏览器中打开"
echo "  • 按 h + Enter 查看帮助"
echo ""
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# 等待2秒
sleep 2

# 启动开发服务器
print_info "启动开发服务器..."
npm run dev

# 如果开发服务器退出
echo ""
print_info "开发服务器已停止"

