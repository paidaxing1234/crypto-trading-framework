#!/bin/bash

# K线补缺循环执行脚本
# 每次执行完 kline_gap_filler 后等待3秒，然后继续下一轮

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAP_FILLER="${SCRIPT_DIR}/../build/kline_gap_filler"

# 检查程序是否存在
if [ ! -f "$GAP_FILLER" ]; then
    echo "❌ 错误: 找不到 kline_gap_filler 程序"
    echo "   路径: $GAP_FILLER"
    exit 1
fi

# 检查程序是否可执行
if [ ! -x "$GAP_FILLER" ]; then
    echo "❌ 错误: kline_gap_filler 没有执行权限"
    echo "   尝试添加执行权限: chmod +x $GAP_FILLER"
    exit 1
fi

echo "=========================================="
echo "K线补缺循环执行脚本"
echo "=========================================="
echo "程序路径: $GAP_FILLER"
echo "执行间隔: 3秒"
echo "按 Ctrl+C 停止"
echo "=========================================="
echo ""

# 捕获 Ctrl+C 信号，优雅退出
trap 'echo -e "\n\n收到停止信号，退出循环..."; exit 0' SIGINT SIGTERM

# 循环计数器
count=0

# 无限循环
while true; do
    count=$((count + 1))

    # 显示当前时间和执行次数
    timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo "[$timestamp] 第 $count 次执行 kline_gap_filler"
    echo "----------------------------------------"

    # 记录开始时间（秒级时间戳）
    start_time=$(date +%s)

    # 执行 kline_gap_filler
    "$GAP_FILLER"
    exit_code=$?

    # 记录结束时间并计算用时
    end_time=$(date +%s)
    elapsed=$((end_time - start_time))

    # 检查退出状态
    if [ $exit_code -eq 0 ]; then
        echo "✓ 执行完成 (退出码: $exit_code, 用时: ${elapsed}秒)"
    else
        echo "✗ 执行失败 (退出码: $exit_code, 用时: ${elapsed}秒)"
    fi

    echo ""
    echo "等待 3 秒后开始下一轮..."
    sleep 3
    echo ""
done
