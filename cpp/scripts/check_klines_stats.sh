#!/bin/bash
# K线数据统计工具
# 用法: ./check_klines_stats.sh [summary|full|symbol SYMBOL_NAME|gaps]

MODE="${1:-summary}"
SYMBOL="${2}"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 函数：检查单个合约
check_single_symbol() {
    local exchange=$1
    local symbol=$2
    local show_header=${3:-true}
    local show_gaps=${4:-false}  # 是否显示详细gap信息

    if [ "$show_header" = "true" ]; then
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "[$exchange] $symbol"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ""
    fi

    for interval in 1m 5m 15m 30m 1h; do
        local key="kline:${exchange}:${symbol}:${interval}"
        local count=$(redis-cli ZCARD "$key" 2>/dev/null)

        if [ -z "$count" ] || [ "$count" = "0" ]; then
            printf "  %-6s ${YELLOW}无数据${NC}\n" "$interval"
            continue
        fi

        local first=$(redis-cli ZRANGE "$key" 0 0 WITHSCORES 2>/dev/null | tail -1)
        local last=$(redis-cli ZRANGE "$key" -1 -1 WITHSCORES 2>/dev/null | tail -1)

        if [ -n "$first" ] && [ -n "$last" ]; then
            local first_time=$(date -d "@$((first/1000))" "+%Y-%m-%d %H:%M:%S" 2>/dev/null)
            local last_time=$(date -d "@$((last/1000))" "+%Y-%m-%d %H:%M:%S" 2>/dev/null)

            # 计算连续性
            local time_span=$((last - first))
            case $interval in
                1m) interval_ms=60000 ;;
                5m) interval_ms=300000 ;;
                15m) interval_ms=900000 ;;
                30m) interval_ms=1800000 ;;
                1h) interval_ms=3600000 ;;
            esac

            local expected=$((time_span / interval_ms + 1))
            local missing=$((expected - count))

            if [ "$expected" -gt 0 ]; then
                local continuity=$(awk "BEGIN {printf \"%.2f\", ($count / $expected) * 100}")
            else
                local continuity="N/A"
            fi

            # 判断状态
            local status
            if [ "$missing" -eq 0 ]; then
                status="${GREEN}✓${NC}"
            elif [ "$missing" -lt 0 ]; then
                status="${RED}⚠️ 重复${NC}"
            else
                status="${RED}✗${NC}"
            fi

            printf "  %-6s 数量: %-8s 缺失: %-8s 连续性: %6s%%  %b\n" \
                "$interval" "$count" "$missing" "$continuity" "$status"
            printf "         时间: %s ~ %s\n" "$first_time" "$last_time"

            # 如果有缺失且需要显示详细gap信息
            if [ "$show_gaps" = "true" ] && [ "$missing" -gt 0 ]; then
                find_gaps_in_symbol "$exchange" "$symbol" "$interval" 5
            fi
        else
            printf "  %-6s 数量: %-8s\n" "$interval" "$count"
        fi
        echo ""
    done
}

# 函数：汇总统计
show_summary() {
    echo "════════════════════════════════════════════════════════════"
    echo "  所有合约 K线数据统计汇总"
    echo "════════════════════════════════════════════════════════════"
    echo ""

    # 获取所有1m的kline keys
    local keys=$(redis-cli KEYS "kline:*:1m" 2>/dev/null)

    # 提取唯一的交易所和交易对
    declare -A symbols
    for key in $keys; do
        local exchange=$(echo "$key" | cut -d: -f2)
        local symbol=$(echo "$key" | cut -d: -f3)

        # 只处理USDT合约
        if [[ "$exchange" == "okx" && "$symbol" == *"-USDT-SWAP" ]] || \
           [[ "$exchange" == "binance" && "$symbol" == *"USDT" ]]; then
            symbols["$exchange:$symbol"]=1
        fi
    done

    # 统计变量
    declare -A stats
    stats["okx_total"]=0
    stats["binance_total"]=0
    stats["okx_perfect"]=0
    stats["binance_perfect"]=0
    stats["okx_has_duplicate"]=0
    stats["binance_has_duplicate"]=0
    stats["okx_has_missing"]=0
    stats["binance_has_missing"]=0

    # 存储有问题的合约
    declare -a okx_duplicate_list
    declare -a binance_duplicate_list

    # 处理每个交易对
    for key in "${!symbols[@]}"; do
        local exchange=$(echo "$key" | cut -d: -f1)
        local symbol=$(echo "$key" | cut -d: -f2)

        if [ "$exchange" = "okx" ]; then
            ((stats["okx_total"]++))
        else
            ((stats["binance_total"]++))
        fi

        local has_duplicate=0
        local has_missing=0
        local all_perfect=1

        for interval in 1m 5m 15m 30m 1h; do
            local redis_key="kline:${exchange}:${symbol}:${interval}"
            local count=$(redis-cli ZCARD "$redis_key" 2>/dev/null)

            if [ -z "$count" ] || [ "$count" = "0" ]; then
                all_perfect=0
                continue
            fi

            local first=$(redis-cli ZRANGE "$redis_key" 0 0 WITHSCORES 2>/dev/null | tail -1)
            local last=$(redis-cli ZRANGE "$redis_key" -1 -1 WITHSCORES 2>/dev/null | tail -1)

            if [ -n "$first" ] && [ -n "$last" ]; then
                local time_span=$((last - first))
                case $interval in
                    1m) interval_ms=60000 ;;
                    5m) interval_ms=300000 ;;
                    15m) interval_ms=900000 ;;
                    30m) interval_ms=1800000 ;;
                    1h) interval_ms=3600000 ;;
                esac

                local expected=$((time_span / interval_ms + 1))
                local missing=$((expected - count))

                if [ "$missing" -lt 0 ]; then
                    has_duplicate=1
                    all_perfect=0
                elif [ "$missing" -gt 0 ]; then
                    has_missing=1
                    all_perfect=0
                fi
            fi
        done

        # 统计
        if [ "$all_perfect" -eq 1 ]; then
            if [ "$exchange" = "okx" ]; then
                ((stats["okx_perfect"]++))
            else
                ((stats["binance_perfect"]++))
            fi
        fi

        if [ "$has_duplicate" -eq 1 ]; then
            if [ "$exchange" = "okx" ]; then
                ((stats["okx_has_duplicate"]++))
                okx_duplicate_list+=("$symbol")
            else
                ((stats["binance_has_duplicate"]++))
                binance_duplicate_list+=("$symbol")
            fi
        fi

        if [ "$has_missing" -eq 1 ]; then
            if [ "$exchange" = "okx" ]; then
                ((stats["okx_has_missing"]++))
            else
                ((stats["binance_has_missing"]++))
            fi
        fi
    done

    # 输出汇总
    echo "【OKX 统计】"
    echo "  总合约数: ${stats["okx_total"]}"
    echo "  完美合约: ${stats["okx_perfect"]} (所有周期100%连续)"
    echo "  有重复数据: ${stats["okx_has_duplicate"]}"
    echo "  有缺失数据: ${stats["okx_has_missing"]}"
    echo ""

    if [ ${stats["okx_has_duplicate"]} -gt 0 ]; then
        echo "  重复数据合约列表:"
        for i in "${okx_duplicate_list[@]}"; do
            echo "    - $i"
        done
        echo ""
    fi

    echo "【Binance 统计】"
    echo "  总合约数: ${stats["binance_total"]}"
    echo "  完美合约: ${stats["binance_perfect"]} (所有周期100%连续)"
    echo "  有重复数据: ${stats["binance_has_duplicate"]}"
    echo "  有缺失数据: ${stats["binance_has_missing"]}"
    echo ""

    if [ ${stats["binance_has_duplicate"]} -gt 0 ]; then
        echo "  重复数据合约列表:"
        for i in "${binance_duplicate_list[@]}"; do
            echo "    - $i"
        done
        echo ""
    fi

    echo "════════════════════════════════════════════════════════════"
}

# 函数：显示所有合约详细信息
show_full() {
    echo "════════════════════════════════════════════════════════════"
    echo "  所有合约 K线数据详细统计"
    echo "════════════════════════════════════════════════════════════"
    echo ""

    local keys=$(redis-cli KEYS "kline:*:1m" 2>/dev/null)

    declare -A symbols
    for key in $keys; do
        local exchange=$(echo "$key" | cut -d: -f2)
        local symbol=$(echo "$key" | cut -d: -f3)

        if [[ "$exchange" == "okx" && "$symbol" == *"-USDT-SWAP" ]] || \
           [[ "$exchange" == "binance" && "$symbol" == *"USDT" ]]; then
            symbols["$exchange:$symbol"]=1
        fi
    done

    local total_count=0
    for key in "${!symbols[@]}"; do
        ((total_count++))
    done

    echo "找到 $total_count 个USDT合约"
    echo ""

    local current=0
    for key in "${!symbols[@]}"; do
        ((current++))
        local exchange=$(echo "$key" | cut -d: -f1)
        local symbol=$(echo "$key" | cut -d: -f2)

        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "[$current/$total_count] $exchange : $symbol"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ""

        check_single_symbol "$exchange" "$symbol" false
    done

    echo "════════════════════════════════════════════════════════════"
}

# 函数：检测单个合约的缺失数据段
find_gaps_in_symbol() {
    local exchange=$1
    local symbol=$2
    local interval=$3
    local max_gaps=${4:-10}  # 最多显示多少个gap

    local key="kline:${exchange}:${symbol}:${interval}"

    # 获取所有时间戳（只要score）
    local timestamps=$(redis-cli ZRANGE "$key" 0 -1 WITHSCORES 2>/dev/null | awk 'NR%2==0')

    if [ -z "$timestamps" ]; then
        return
    fi

    # 计算间隔毫秒数
    case $interval in
        1m) interval_ms=60000 ;;
        5m) interval_ms=300000 ;;
        15m) interval_ms=900000 ;;
        30m) interval_ms=1800000 ;;
        1h) interval_ms=3600000 ;;
    esac

    # 转换为数组
    local ts_array=($timestamps)
    local gap_count=0
    local total_missing=0

    # 检查连续性
    for ((i=0; i<${#ts_array[@]}-1; i++)); do
        local current=${ts_array[$i]}
        local next=${ts_array[$i+1]}
        local diff=$((next - current))

        # 如果差值大于一个周期，说明有gap
        if [ $diff -gt $interval_ms ]; then
            local missing_count=$(( (diff / interval_ms) - 1 ))

            if [ $missing_count -gt 0 ]; then
                ((gap_count++))
                ((total_missing += missing_count))

                # 只显示前N个gap
                if [ $gap_count -le $max_gaps ]; then
                    local gap_start=$(date -d "@$((current/1000 + interval_ms/1000))" "+%Y-%m-%d %H:%M:%S" 2>/dev/null)
                    local gap_end=$(date -d "@$((next/1000 - interval_ms/1000))" "+%Y-%m-%d %H:%M:%S" 2>/dev/null)

                    printf "    Gap #%-3d: %s ~ %s (缺失 %d 根)\n" \
                        "$gap_count" "$gap_start" "$gap_end" "$missing_count"
                fi
            fi
        fi
    done

    if [ $gap_count -gt $max_gaps ]; then
        printf "    ... 还有 %d 个缺失段未显示\n" $((gap_count - max_gaps))
    fi

    if [ $gap_count -gt 0 ]; then
        printf "    ${RED}总计: %d 个缺失段，共缺失 %d 根K线${NC}\n" "$gap_count" "$total_missing"
    fi

    return $gap_count
}

# 函数：显示所有有缺失数据的合约
show_gaps() {
    echo "════════════════════════════════════════════════════════════"
    echo "  缺失数据详细分析"
    echo "════════════════════════════════════════════════════════════"
    echo ""

    local keys=$(redis-cli KEYS "kline:*:1m" 2>/dev/null)

    # 提取唯一的交易所和交易对
    declare -A symbols
    for key in $keys; do
        local exchange=$(echo "$key" | cut -d: -f2)
        local symbol=$(echo "$key" | cut -d: -f3)

        if [[ "$exchange" == "okx" && "$symbol" == *"-USDT-SWAP" ]] || \
           [[ "$exchange" == "binance" && "$symbol" == *"USDT" ]]; then
            symbols["$exchange:$symbol"]=1
        fi
    done

    local total_symbols_with_gaps=0

    # 检查每个交易对
    for key in "${!symbols[@]}"; do
        local exchange=$(echo "$key" | cut -d: -f1)
        local symbol=$(echo "$key" | cut -d: -f2)

        local has_gaps=0

        # 先快速检查是否有缺失
        for interval in 1m 5m 15m 30m 1h; do
            local redis_key="kline:${exchange}:${symbol}:${interval}"
            local count=$(redis-cli ZCARD "$redis_key" 2>/dev/null)

            if [ -z "$count" ] || [ "$count" = "0" ]; then
                continue
            fi

            local first=$(redis-cli ZRANGE "$redis_key" 0 0 WITHSCORES 2>/dev/null | tail -1)
            local last=$(redis-cli ZRANGE "$redis_key" -1 -1 WITHSCORES 2>/dev/null | tail -1)

            if [ -n "$first" ] && [ -n "$last" ]; then
                local time_span=$((last - first))
                case $interval in
                    1m) interval_ms=60000 ;;
                    5m) interval_ms=300000 ;;
                    15m) interval_ms=900000 ;;
                    30m) interval_ms=1800000 ;;
                    1h) interval_ms=3600000 ;;
                esac

                local expected=$((time_span / interval_ms + 1))
                local missing=$((expected - count))

                if [ "$missing" -gt 0 ]; then
                    has_gaps=1
                    break
                fi
            fi
        done

        # 如果有缺失，详细分析
        if [ $has_gaps -eq 1 ]; then
            ((total_symbols_with_gaps++))

            echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
            echo "[$exchange] $symbol"
            echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
            echo ""

            for interval in 1m 5m 15m 30m 1h; do
                local redis_key="kline:${exchange}:${symbol}:${interval}"
                local count=$(redis-cli ZCARD "$redis_key" 2>/dev/null)

                if [ -z "$count" ] || [ "$count" = "0" ]; then
                    continue
                fi

                local first=$(redis-cli ZRANGE "$redis_key" 0 0 WITHSCORES 2>/dev/null | tail -1)
                local last=$(redis-cli ZRANGE "$redis_key" -1 -1 WITHSCORES 2>/dev/null | tail -1)

                if [ -n "$first" ] && [ -n "$last" ]; then
                    local time_span=$((last - first))
                    case $interval in
                        1m) interval_ms=60000 ;;
                        5m) interval_ms=300000 ;;
                        15m) interval_ms=900000 ;;
                        30m) interval_ms=1800000 ;;
                        1h) interval_ms=3600000 ;;
                    esac

                    local expected=$((time_span / interval_ms + 1))
                    local missing=$((expected - count))

                    if [ "$missing" -gt 0 ]; then
                        printf "  ${YELLOW}%-6s${NC} 缺失 %d 根 (连续性: %.2f%%)\n" \
                            "$interval" "$missing" $(awk "BEGIN {printf \"%.2f\", ($count / $expected) * 100}")

                        # 查找具体的gap
                        find_gaps_in_symbol "$exchange" "$symbol" "$interval" 5
                        echo ""
                    fi
                fi
            done
        fi
    done

    echo "════════════════════════════════════════════════════════════"
    echo "总计: $total_symbols_with_gaps 个合约有缺失数据"
    echo "════════════════════════════════════════════════════════════"
}

# 主程序
case "$MODE" in
    summary)
        show_summary
        ;;
    full)
        show_full
        ;;
    gaps)
        show_gaps
        ;;
    symbol)
        if [ -z "$SYMBOL" ]; then
            echo "错误: 请指定合约名称"
            echo "用法: $0 symbol SYMBOL_NAME [--gaps]"
            echo "示例: $0 symbol BTCUSDT"
            echo "      $0 symbol BTC-USDT-SWAP"
            echo "      $0 symbol BTCUSDT --gaps  # 显示详细缺失段"
            exit 1
        fi

        # 检查是否需要显示gaps
        show_gaps_flag=false
        if [ "$3" = "--gaps" ]; then
            show_gaps_flag=true
        fi

        # 尝试查找合约
        found=0
        for exchange in okx binance; do
            # 尝试不同的格式
            if [ "$exchange" = "okx" ]; then
                # OKX格式: BTC-USDT-SWAP
                test_symbol="$SYMBOL"
                if [[ ! "$test_symbol" == *"-USDT-SWAP" ]]; then
                    test_symbol="${SYMBOL}-USDT-SWAP"
                fi
            else
                # Binance格式: BTCUSDT
                test_symbol="$SYMBOL"
                if [[ ! "$test_symbol" == *"USDT" ]]; then
                    test_symbol="${SYMBOL}USDT"
                fi
            fi

            # 检查是否存在
            key="kline:${exchange}:${test_symbol}:1m"
            count=$(redis-cli ZCARD "$key" 2>/dev/null)

            if [ -n "$count" ] && [ "$count" -gt 0 ]; then
                check_single_symbol "$exchange" "$test_symbol" true "$show_gaps_flag"
                found=1
            fi
        done

        if [ "$found" -eq 0 ]; then
            echo "错误: 未找到合约 $SYMBOL"
            echo "请检查合约名称是否正确"
        fi
        ;;
    *)
        echo "K线数据统计工具"
        echo ""
        echo "用法:"
        echo "  $0 [summary|full|gaps|symbol SYMBOL_NAME]"
        echo ""
        echo "模式:"
        echo "  summary        - 显示汇总统计 (默认)"
        echo "  full           - 显示所有合约详细信息"
        echo "  gaps           - 显示所有有缺失数据的合约及具体缺失段"
        echo "  symbol NAME    - 显示指定合约的详细信息"
        echo ""
        echo "示例:"
        echo "  $0                      # 显示汇总"
        echo "  $0 summary              # 显示汇总"
        echo "  $0 full                 # 显示所有合约详细信息"
        echo "  $0 gaps                 # 显示缺失数据详情"
        echo "  $0 symbol BTCUSDT       # 显示BTC合约信息"
        echo "  $0 symbol TRX           # 显示TRX合约信息"
        ;;
esac
