#!/bin/bash
# 查看策略日志文件

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$SCRIPT_DIR/../strategies/logs"

echo "╔════════════════════════════════════════════════════════════╗"
echo "║        策略日志查看器                                        ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

if [ ! -d "$LOG_DIR" ]; then
    echo "❌ 日志目录不存在: $LOG_DIR"
    exit 1
fi

echo "日志目录: $LOG_DIR"
echo ""

# 列出所有日志文件
log_files=("$LOG_DIR"/*.txt)

if [ ! -e "${log_files[0]}" ]; then
    echo "暂无日志文件"
    exit 0
fi

echo "可用的日志文件:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
i=1
for log_file in "${log_files[@]}"; do
    if [ -f "$log_file" ]; then
        filename=$(basename "$log_file")
        size=$(du -h "$log_file" | cut -f1)
        lines=$(wc -l < "$log_file")
        modified=$(stat -c %y "$log_file" | cut -d'.' -f1)
        printf "  %d) %-30s | 大小: %-8s | 行数: %-8s | 修改: %s\n" "$i" "$filename" "$size" "$lines" "$modified"
        ((i++))
    fi
done
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

echo "选择操作:"
echo "  1) 查看最近100行"
echo "  2) 查看最近500行"
echo "  3) 实时跟踪 (tail -f)"
echo "  4) 查看全部"
echo "  5) 搜索关键词"
echo "  q) 退出"
echo ""
read -p "请选择 [1-5/q]: " choice

case $choice in
    q|Q)
        echo "退出"
        exit 0
        ;;
esac

# 选择日志文件
if [ ${#log_files[@]} -eq 1 ]; then
    selected_log="${log_files[0]}"
    echo "自动选择: $(basename "$selected_log")"
else
    echo ""
    read -p "请选择日志文件编号 [1-$((i-1))]: " file_num
    if [ "$file_num" -ge 1 ] && [ "$file_num" -lt "$i" ]; then
        selected_log="${log_files[$((file_num-1))]}"
    else
        echo "无效选择"
        exit 1
    fi
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "日志文件: $(basename "$selected_log")"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

case $choice in
    1)
        tail -100 "$selected_log"
        ;;
    2)
        tail -500 "$selected_log"
        ;;
    3)
        echo "实时跟踪日志 (按 Ctrl+C 退出)..."
        echo ""
        tail -f "$selected_log"
        ;;
    4)
        cat "$selected_log"
        ;;
    5)
        read -p "输入搜索关键词: " keyword
        grep -i "$keyword" "$selected_log" | tail -100
        ;;
    *)
        echo "无效选择"
        exit 1
        ;;
esac
