#!/bin/bash
# 一键启动脚本：在四个独立终端中启动 trading_server_full、data_recorder、kline_gap_filler 和监控脚本

# 获取脚本所在目录的绝对路径
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
MONITOR_SCRIPT="$SCRIPT_DIR/monitor_btc_klines.sh"

# 交易服务器环境：默认直连交易所，不走本机代理端口。
export USE_PROXY=0
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY
unset PROXY_HOST PROXY_PORT

# 检查可执行文件是否存在
if [ ! -f "$BUILD_DIR/trading_server_full" ]; then
    echo "错误: trading_server_full 不存在，请先编译"
    exit 1
fi

if [ ! -f "$BUILD_DIR/data_recorder" ]; then
    echo "错误: data_recorder 不存在，请先编译"
    exit 1
fi

if [ ! -f "$SCRIPT_DIR/fast_kline_filler.py" ]; then
    echo "错误: fast_kline_filler.py 不存在"
    exit 1
fi

if [ ! -f "$MONITOR_SCRIPT" ]; then
    echo "错误: monitor_btc_klines.sh 不存在"
    exit 1
fi

echo "======================================22222==="
echo "  启动交易系统"
echo "========================================="
echo ""

# 使用 tmux 启动四个窗口
if ! command -v tmux &> /dev/null; then
    echo "错误: 未找到 tmux"
    echo "请安装 tmux："
    echo "  sudo apt install tmux"
    exit 1
fi

echo "使用 tmux 启动四个窗口..."

# 创建新的 tmux 会话
SESSION_NAME="trading_system"

# 检查会话是否已存在
if tmux has-session -t $SESSION_NAME 2>/dev/null; then
    echo "警告: tmux 会话 '$SESSION_NAME' 已存在"
    read -p "是否关闭现有会话并重新启动? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        tmux kill-session -t $SESSION_NAME
    else
        echo "取消启动"
        exit 0
    fi
fi

# 创建新会话并启动 trading_server_full
tmux new-session -d -s $SESSION_NAME -n "trading_server" "cd '$BUILD_DIR' && ./trading_server_full; read -p '按回车键关闭...'"

# 等待1秒
sleep 1

# 创建新窗口并启动 data_recorder
tmux new-window -t $SESSION_NAME -n "data_recorder" "cd '$BUILD_DIR' && ./data_recorder; read -p '按回车键关闭...'"

# 等待2秒
sleep 2

# 创建新窗口并启动 fast_kline_filler.py
tmux new-window -t $SESSION_NAME -n "gap_filler" "cd '$SCRIPT_DIR' && python fast_kline_filler.py --loop --interval 90; read -p '按回车键关闭...'"

# 创建新窗口并启动监控脚本
tmux new-window -t $SESSION_NAME -n "monitor" "cd '$SCRIPT_DIR' && ./monitor_btc_klines.sh"

# 选择第一个窗口
tmux select-window -t $SESSION_NAME:0

echo "✓ 已在 tmux 会话中启动所有服务"
echo ""
echo "使用说明："
echo "  - 连接到会话: tmux attach -t $SESSION_NAME"
echo "  - 切换窗口: Ctrl+b 然后按 0/1/2/3"
echo "  - 分离会话: Ctrl+b 然后按 d"
echo "  - 关闭会话: tmux kill-session -t $SESSION_NAME"
echo ""
echo "窗口列表："
echo "  0: trading_server - 交易服务器"
echo "  1: data_recorder - 数据记录器"
echo "  2: gap_filler - K线补全工具 (Python)"
echo "  3: monitor - K线实时监控"
echo ""

# 自动连接到会话
tmux attach -t $SESSION_NAME
