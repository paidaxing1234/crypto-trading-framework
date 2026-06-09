#!/bin/bash
# 修复K线重复数据的完整流程

echo "========================================"
echo "  K线数据去重和智能加载工具"
echo "========================================"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 步骤1: 全面去重所有现有数据
echo "步骤1: 全面去重所有现有数据"
echo "----------------------------------------"
python3 "$SCRIPT_DIR/preload_klines_to_redis.py" --mode deduplicate --exchange all --interval all --workers 5

echo ""
echo "========================================"
echo "步骤2: 智能加载缺失数据"
echo "========================================"
echo ""

# 步骤2: 对每个周期进行智能加载
for interval in 1m 5m 15m 30m 1h; do
    echo ""
    echo "----------------------------------------"
    echo "处理 $interval K线"
    echo "----------------------------------------"

    if [ "$interval" = "1h" ]; then
        days=180  # 1h周期加载6个月
    else
        days=60   # 其他周期加载2个月
    fi

    python3 "$SCRIPT_DIR/preload_klines_to_redis.py" \
        --mode current \
        --exchange all \
        --interval "$interval" \
        --workers 5
done

echo ""
echo "========================================"
echo "  完成！"
echo "========================================"
echo ""
echo "建议运行以下命令验证数据："
echo "  python3 $SCRIPT_DIR/check_klines_stats.py"
