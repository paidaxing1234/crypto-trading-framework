#!/bin/bash
# 实时监控 BTC K线写入情况（包含起止时间、连续性检测和重复检测）

echo "========================================="
echo "  实时监控 BTC K线数据"
echo "  按 Ctrl+C 停止"
echo "========================================="
echo ""

# 检查连续性和重复的函数（返回格式：gaps|duplicates|gap_ranges|dup_ranges）
check_continuity_and_duplicates() {
    local key=$1
    local interval_seconds=$2

    # 获取所有时间戳
    local timestamps=$(redis-cli ZRANGE "$key" 0 -1 WITHSCORES 2>/dev/null | awk 'NR%2==0')

    if [ -z "$timestamps" ]; then
        echo "0|0||"
        return
    fi

    # 转换为数组
    local ts_array=($timestamps)
    local gaps=0
    local duplicates=0
    local gap_ranges=""
    local dup_ranges=""

    # 检查连续性和重复
    for ((i=1; i<${#ts_array[@]}; i++)); do
        local prev=${ts_array[$((i-1))]}
        local curr=${ts_array[$i]}
        local expected=$((prev + interval_seconds * 1000))

        if [ $curr -gt $expected ]; then
            # 发现缺失
            local gap_count=$(( (curr - expected) / (interval_seconds * 1000) ))
            gaps=$((gaps + gap_count))

            # 记录缺失时间段（只记录前5个）
            if [ $(echo "$gap_ranges" | grep -o "~" | wc -l) -lt 5 ]; then
                local gap_start=$(date -d "@$((expected / 1000))" '+%H:%M:%S' 2>/dev/null)
                local gap_end=$(date -d "@$(((curr - interval_seconds * 1000) / 1000))" '+%H:%M:%S' 2>/dev/null)
                if [ -n "$gap_ranges" ]; then
                    gap_ranges="${gap_ranges}; ${gap_start}~${gap_end}"
                else
                    gap_ranges="${gap_start}~${gap_end}"
                fi
            fi
        elif [ $curr -eq $prev ]; then
            # 发现重复
            duplicates=$((duplicates + 1))

            # 记录重复时间点（只记录前5个）
            if [ $(echo "$dup_ranges" | grep -o "," | wc -l) -lt 5 ]; then
                local dup_time=$(date -d "@$((curr / 1000))" '+%H:%M:%S' 2>/dev/null)
                if [ -n "$dup_ranges" ]; then
                    dup_ranges="${dup_ranges}, ${dup_time}"
                else
                    dup_ranges="${dup_time}"
                fi
            fi
        fi
    done

    # 如果有更多缺失/重复，添加省略号
    if [ $gaps -gt 5 ]; then
        gap_ranges="${gap_ranges}..."
    fi
    if [ $duplicates -gt 5 ]; then
        dup_ranges="${dup_ranges}..."
    fi

    echo "${gaps}|${duplicates}|${gap_ranges}|${dup_ranges}"
}

# 获取起止时间的函数
get_time_range() {
    local key=$1

    # 获取第一个和最后一个时间戳
    local first_ts=$(redis-cli ZRANGE "$key" 0 0 WITHSCORES 2>/dev/null | tail -1)
    local last_ts=$(redis-cli ZRANGE "$key" -1 -1 WITHSCORES 2>/dev/null | tail -1)

    if [ -z "$first_ts" ] || [ -z "$last_ts" ]; then
        echo "N/A ~ N/A"
        return
    fi

    # 转换为可读时间（毫秒转秒）
    local first_time=$(date -d "@$((first_ts / 1000))" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "N/A")
    local last_time=$(date -d "@$((last_ts / 1000))" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "N/A")

    echo "$first_time ~ $last_time"
}

# 显示K线统计信息的函数
show_kline_stats() {
    local exchange=$1
    local symbol=$2
    local display_name=$3

    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  📊 $display_name"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    for interval in 1m 5m 15m 30m 1h 4h 8h 1d; do
        key="kline:${exchange}:${symbol}:${interval}"
        count=$(redis-cli ZCARD "$key" 2>/dev/null || echo "0")

        if [ "$count" -gt 0 ]; then
            # 获取时间范围
            time_range=$(get_time_range "$key")

            # 获取连续性和重复（根据周期计算间隔秒数）
            case $interval in
                1m) interval_sec=60 ;;
                5m) interval_sec=300 ;;
                15m) interval_sec=900 ;;
                30m) interval_sec=1800 ;;
                1h) interval_sec=3600 ;;
                4h) interval_sec=14400 ;;
                8h) interval_sec=28800 ;;
                1d) interval_sec=86400 ;;
            esac

            result=$(check_continuity_and_duplicates "$key" $interval_sec)
            IFS='|' read -r gaps duplicates gap_ranges dup_ranges <<< "$result"

            # 计算连续性百分比
            if [ $gaps -eq 0 ] && [ $duplicates -eq 0 ]; then
                continuity="100.00%"
                status="✓"
            else
                total=$((count + gaps))
                continuity=$(awk "BEGIN {printf \"%.2f%%\", ($count / $total) * 100}")
                if [ $gaps -lt 10 ] && [ $duplicates -lt 10 ]; then
                    status="⚠"
                else
                    status="✗"
                fi
            fi

            printf "  %-6s 数量: %-8s 缺失: %-6s 重复: %-6s 连续性: %-10s %s\n" \
                "$interval" "$count" "$gaps" "$duplicates" "$continuity" "$status"
            printf "         时间: %s\n" "$time_range"

            # 1d K线 ts 按 UTC 0:00 对齐 (上方"时间"已是 UTC+8 显示, 这里补 UTC+0 对照)
            if [ "$interval" = "1d" ]; then
                first_ts=$(redis-cli ZRANGE "$key" 0 0 WITHSCORES 2>/dev/null | tail -1)
                last_ts=$(redis-cli ZRANGE "$key" -1 -1 WITHSCORES 2>/dev/null | tail -1)
                if [ -n "$first_ts" ] && [ -n "$last_ts" ]; then
                    utc_first=$(date -u -d "@$((first_ts / 1000))" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)
                    utc_last=$(date -u -d "@$((last_ts / 1000))" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)
                    printf "         \033[36m(对应 UTC+0: %s ~ %s)\033[0m\n" "$utc_first" "$utc_last"
                fi
            fi

            # 显示缺失时间段
            if [ $gaps -gt 0 ] && [ -n "$gap_ranges" ]; then
                printf "         \033[33m缺失时段: %s\033[0m\n" "$gap_ranges"
            fi

            # 显示重复时间点
            if [ $duplicates -gt 0 ] && [ -n "$dup_ranges" ]; then
                printf "         \033[31m重复时间: %s\033[0m\n" "$dup_ranges"
            fi
        else
            printf "  %-6s 暂无数据\n" "$interval"
        fi
    done

    echo ""
}

while true; do
    clear
    echo "╔════════════════════════════════════════════════════════════════════════════════╗"
    echo "║                        BTC K线实时监控                                          ║"
    echo "║                    时间: $(date '+%Y-%m-%d %H:%M:%S')                          ║"
    echo "╚════════════════════════════════════════════════════════════════════════════════╝"
    echo ""

    # OKX BTC-USDT-SWAP
    show_kline_stats "okx" "BTC-USDT-SWAP" "OKX BTC-USDT-SWAP"

    # Binance BTCUSDT
    show_kline_stats "binance" "BTCUSDT" "Binance BTCUSDT"

    # 最新K线数据
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  📈 最新1min K线"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # OKX 最新K线
    okx_latest=$(redis-cli ZRANGE "kline:okx:BTC-USDT-SWAP:1m" -1 -1 2>/dev/null)
    if [ -n "$okx_latest" ]; then
        echo "  OKX:"
        echo "$okx_latest" | jq -r '
            "    时间: \(.timestamp / 1000 | localtime | strftime("%Y-%m-%d %H:%M:%S"))",
            "    开: \(.open)  高: \(.high)  低: \(.low)  收: \(.close)",
            "    量: \(.volume)"
        ' 2>/dev/null || echo "    $okx_latest"
    else
        echo "  OKX: 暂无数据"
    fi

    echo ""

    # Binance 最新K线
    binance_latest=$(redis-cli ZRANGE "kline:binance:BTCUSDT:1m" -1 -1 2>/dev/null)
    if [ -n "$binance_latest" ]; then
        echo "  Binance:"
        echo "$binance_latest" | jq -r '
            "    时间: \(.timestamp / 1000 | localtime | strftime("%Y-%m-%d %H:%M:%S"))",
            "    开: \(.open)  高: \(.high)  低: \(.low)  收: \(.close)",
            "    量: \(.volume)",
            "    额: \(.amount // "N/A")  主买额: \(.buy_amount // "N/A")"
        ' 2>/dev/null || echo "    $binance_latest"
    else
        echo "  Binance: 暂无数据"
    fi

    echo ""

    # 全市场统计
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  📊 全市场统计"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    total_1m_klines=0
    kline_1m_keys=$(redis-cli KEYS "kline:*:1m" 2>/dev/null)
    if [ -n "$kline_1m_keys" ]; then
        for key in $kline_1m_keys; do
            count=$(redis-cli ZCARD "$key" 2>/dev/null || echo "0")
            total_1m_klines=$((total_1m_klines + count))
        done
    fi
    echo "  全市场1min K线总数: $(printf "%'d" $total_1m_klines) 根"
    echo ""

    # data_recorder 日志
    if [ -f /tmp/data_recorder.log ]; then
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "  📝 data_recorder 日志（最近3行）"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        tail -n 3 /tmp/data_recorder.log 2>/dev/null | sed 's/^/  /'
        echo ""
    fi

    echo "════════════════════════════════════════════════════════════════════════════════"
    echo "  刷新间隔: 3秒 | 图例: ✓=完美 ⚠=少量问题 ✗=较多问题"
    echo "  \033[33m黄色\033[0m=缺失时段 \033[31m红色\033[0m=重复时间"
    echo "════════════════════════════════════════════════════════════════════════════════"

    sleep 3
done
