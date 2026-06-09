#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
拉取Binance所有USDT合约的最小下单数量(minQty)

使用Binance Futures API获取所有USDT永续合约的交易规则,
提取每个合约的最小下单数量(minQty),并保存到配置文件中。

API文档: https://binance-docs.github.io/apidocs/futures/cn/#0f3f2d5ee7
"""

import json
import time
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry
from datetime import datetime


def fetch_binance_min_qty(max_retries=3):
    """
    从Binance Futures API获取所有USDT合约的minQty

    Args:
        max_retries: 最大重试次数

    Returns:
        dict: {symbol: minQty} 的字典
    """
    api_url = "https://fapi.binance.com/fapi/v1/exchangeInfo"

    print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] 正在从Binance API获取合约信息...")
    print(f"API地址: {api_url}")

    # 创建session并配置重试策略
    session = requests.Session()
    retry_strategy = Retry(
        total=max_retries,
        backoff_factor=2,
        status_forcelist=[429, 500, 502, 503, 504],
    )
    adapter = HTTPAdapter(max_retries=retry_strategy)
    session.mount("https://", adapter)
    session.mount("http://", adapter)

    headers = {
        'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
        'Accept': 'application/json',
        'Accept-Language': 'en-US,en;q=0.9',
        'Accept-Encoding': 'gzip, deflate, br',
        'Connection': 'keep-alive',
    }

    for attempt in range(max_retries):
        try:
            if attempt > 0:
                wait_time = 2 ** attempt
                print(f"等待 {wait_time} 秒后重试...")
                time.sleep(wait_time)
                print(f"第 {attempt + 1}/{max_retries} 次尝试...")

            response = session.get(api_url, headers=headers, timeout=30)
            response.raise_for_status()
            data = response.json()

            if "symbols" not in data:
                print("错误: API响应中没有symbols字段")
                return {}

            # 提取所有USDT永续合约的minQty
            min_qty_dict = {}
            usdt_perpetual_count = 0

            for symbol_info in data["symbols"]:
                symbol = symbol_info.get("symbol", "")
                contract_type = symbol_info.get("contractType", "")
                quote_asset = symbol_info.get("quoteAsset", "")
                status = symbol_info.get("status", "")

                # 只处理USDT永续合约且状态为TRADING的
                if quote_asset == "USDT" and contract_type == "PERPETUAL" and status == "TRADING":
                    usdt_perpetual_count += 1

                    # 从filters中提取LOT_SIZE的minQty
                    filters = symbol_info.get("filters", [])
                    min_qty = None

                    for filter_item in filters:
                        if filter_item.get("filterType") == "LOT_SIZE":
                            min_qty = filter_item.get("minQty")
                            break

                    if min_qty:
                        min_qty_dict[symbol] = float(min_qty)
                    else:
                        print(f"警告: {symbol} 没有找到minQty")

            print(f"✓ 成功获取 {len(min_qty_dict)} 个USDT永续合约的minQty信息")
            print(f"  (共有 {usdt_perpetual_count} 个USDT永续合约)")

            return min_qty_dict

        except requests.exceptions.RequestException as e:
            print(f"错误: 请求API失败 - {e}")
            if attempt == max_retries - 1:
                print(f"已达到最大重试次数({max_retries}),放弃")
                return {}
        except json.JSONDecodeError as e:
            print(f"错误: 解析JSON失败 - {e}")
            return {}
        except Exception as e:
            print(f"错误: {e}")
            return {}

    return {}


def save_to_file(min_qty_dict, output_path):
    """
    将minQty信息保存到文件

    Args:
        min_qty_dict: {symbol: minQty} 的字典
        output_path: 输出文件路径
    """
    if not min_qty_dict:
        print("错误: 没有数据可保存")
        return False

    try:
        with open(output_path, 'w', encoding='utf-8') as f:
            # 写入文件头
            f.write("=" * 80 + "\n")
            f.write("Binance USDT永续合约最小下单数量(minQty)\n")
            f.write(f"更新时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"数据来源: https://fapi.binance.com/fapi/v1/exchangeInfo\n")
            f.write(f"合约数量: {len(min_qty_dict)}\n")
            f.write("=" * 80 + "\n\n")

            # 按symbol排序后写入
            sorted_items = sorted(min_qty_dict.items(), key=lambda x: x[0])

            f.write(f"{'Symbol':<20} {'minQty':<20}\n")
            f.write("-" * 80 + "\n")

            for symbol, min_qty in sorted_items:
                # 格式化minQty,去除不必要的小数点后的0
                min_qty_str = f"{min_qty:.8f}".rstrip('0').rstrip('.')
                f.write(f"{symbol:<20} {min_qty_str:<20}\n")

            f.write("\n" + "=" * 80 + "\n")
            f.write("说明:\n")
            f.write("- minQty: 最小下单数量(单位:币,如BTC、ETH等)\n")
            f.write("- 下单数量必须是minQty的整数倍\n")
            f.write("- 实际下单时还需要满足其他过滤器要求(如stepSize、notional等)\n")
            f.write("=" * 80 + "\n")

        print(f"✓ 数据已保存到: {output_path}")
        return True

    except Exception as e:
        print(f"错误: 保存文件失败 - {e}")
        return False


def main():
    """主函数"""
    print("\n" + "=" * 80)
    print("Binance USDT合约minQty获取工具")
    print("=" * 80 + "\n")

    # 输出文件路径
    script_dir = os.path.dirname(os.path.abspath(__file__))
    output_path = os.path.join(script_dir, "..", "strategies", "configs", "binancemin.txt")

    # 获取数据
    min_qty_dict = fetch_binance_min_qty()

    if not min_qty_dict:
        print("\n✗ 获取数据失败")
        return 1

    # 保存到文件
    print()
    if save_to_file(min_qty_dict, output_path):
        print("\n✓ 完成!")

        # 显示一些示例数据
        print("\n示例数据(前10个):")
        print("-" * 80)
        for i, (symbol, min_qty) in enumerate(sorted(min_qty_dict.items())[:10]):
            min_qty_str = f"{min_qty:.8f}".rstrip('0').rstrip('.')
            print(f"  {symbol:<20} {min_qty_str}")
        print("-" * 80)

        return 0
    else:
        print("\n✗ 保存失败")
        return 1


if __name__ == "__main__":
    exit(main())
