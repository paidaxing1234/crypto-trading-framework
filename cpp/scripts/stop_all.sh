#!/bin/bash
# 停止所有交易系统进程（包括 tmux 会话和策略子进程）

echo "========================================="
echo "  停止交易系统"
echo "========================================="
echo ""

SESSION_NAME="trading_system"
stopped=0

# 1. 停止 tmux 会话
if tmux has-session -t $SESSION_NAME 2>/dev/null; then
    echo "[1/3] 关闭 tmux 会话 '$SESSION_NAME'..."
    tmux kill-session -t $SESSION_NAME
    echo "  ✓ tmux 会话已关闭"
    stopped=1
else
    echo "[1/3] tmux 会话 '$SESSION_NAME' 不存在，跳过"
fi

# 2. 停止 trading_server_full（会触发其清理逻辑，自动杀子进程）
server_pids=$(pgrep -f "trading_server_full" 2>/dev/null)
if [ -n "$server_pids" ]; then
    echo "[2/3] 停止 trading_server_full (PID: $server_pids)..."
    kill $server_pids 2>/dev/null
    # 等待最多 5 秒让 server 完成清理
    for i in $(seq 1 10); do
        if ! pgrep -f "trading_server_full" > /dev/null 2>&1; then
            break
        fi
        sleep 0.5
    done
    # 如果还在运行，强杀
    remaining=$(pgrep -f "trading_server_full" 2>/dev/null)
    if [ -n "$remaining" ]; then
        echo "  trading_server_full 未在 5 秒内退出，强制杀死..."
        kill -9 $remaining 2>/dev/null
    fi
    echo "  ✓ trading_server_full 已停止"
    stopped=1
else
    echo "[2/3] trading_server_full 未运行，跳过"
fi

# 3. 清理残留的策略子进程
echo "[3/3] 检查残留策略进程..."
orphans=$(pgrep -f "strategies/implementations.*\.py" 2>/dev/null)
if [ -n "$orphans" ]; then
    echo "  发现残留策略进程:"
    ps -p $(echo "$orphans" | tr '\n' ',') -o pid,ppid,etime,args --no-headers 2>/dev/null | while read line; do
        echo "    $line"
    done
    echo "  发送 SIGTERM..."
    kill $orphans 2>/dev/null
    sleep 2
    # 检查是否还存活
    remaining=$(pgrep -f "strategies/implementations.*\.py" 2>/dev/null)
    if [ -n "$remaining" ]; then
        echo "  仍有进程存活，发送 SIGKILL..."
        kill -9 $remaining 2>/dev/null
    fi
    echo "  ✓ 残留策略进程已清理"
    stopped=1
else
    echo "  无残留策略进程"
fi

# 4. 同时清理 data_recorder
dr_pids=$(pgrep -f "data_recorder" 2>/dev/null)
if [ -n "$dr_pids" ]; then
    echo "[额外] 停止 data_recorder (PID: $dr_pids)..."
    kill $dr_pids 2>/dev/null
    sleep 1
    remaining=$(pgrep -f "data_recorder" 2>/dev/null)
    if [ -n "$remaining" ]; then
        kill -9 $remaining 2>/dev/null
    fi
    echo "  ✓ data_recorder 已停止"
    stopped=1
fi

echo ""
if [ $stopped -eq 1 ]; then
    echo "✓ 交易系统已停止"
else
    echo "交易系统未在运行"
fi
