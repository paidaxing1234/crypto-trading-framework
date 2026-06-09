#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
拉取OKX所有USDT永续合约的最小下单张数和每张面值

使用OKX Public API获取所有USDT永续合约的交易规则,
提取每个合约的最小下单张数(minSz)和每张面值(ctVal),并保存到配置文件中。

ctVal = 每张合约代表多少币, 例如 BTC-USDT-SWAP 的 ctVal=0.01 表示1张=0.01BTC
下单金额换算: 张数 = 下单金额 / (当前价格 * ctVal)

API文档: https://www.okx.com/docs-v5/zh/#public-data-rest-api-get-instruments
"""

import json
import time
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry
from datetime import datetime


def fetch_okx_min_sz(max_retries=3):
    """
    从OKX Public API获取所有USDT永续合约的最小下单张数和每张面值

    Args:
        max_retries: 最大重试次数

    Returns:
        dict: {symbol: {"minSz": float, "ctVal": float}} 的字典
    """
    # OKX API: 获取永续合约信息
    api_url = "https://www.okx.com/api/v5/public/instruments?instType=SWAP"

    print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] 正在从OKX API获取合约信息...")
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

            if data.get("code") != "0":
                error_msg = data.get("msg", "未知错误")
                print(f"错误: API返回错误 - {error_msg}")
                return {}

            if "data" not in data:
                print("错误: API响应中没有data字段")
                return {}

            # 提取所有USDT永续合约的最小下单张数和每张面值
            min_sz_dict = {}
            usdt_swap_count = 0

            for inst_info in data["data"]:
                inst_id = inst_info.get("instId", "")
                settle_ccy = inst_info.get("settleCcy", "")
                state = inst_info.get("state", "")

                # 只处理USDT结算的永续合约且状态为live的
                if settle_ccy == "USDT" and state == "live" and "-SWAP" in inst_id:
                    usdt_swap_count += 1

                    try:
                        # minSz: 最小下单数量(张)
                        min_sz = float(inst_info.get("minSz", "0"))
                        # ctVal: 每张合约面值(币), 例如BTC-USDT-SWAP的ctVal=0.01表示1张=0.01BTC
                        ct_val = float(inst_info.get("ctVal", "0"))

                        if min_sz > 0 and ct_val > 0:
                            min_sz_dict[inst_id] = {
                                "minSz": min_sz,
                                "ctVal": ct_val
                            }
                        else:
                            print(f"警告: {inst_id} 的 minSz({min_sz}) 或 ctVal({ct_val}) 无效")

                    except (ValueError, TypeError) as e:
                        print(f"警告: {inst_id} 数据解析失败 - {e}")

            print(f"✓ 成功获取 {len(min_sz_dict)} 个USDT永续合约的最小下单张数和面值信息")
            print(f"  (共有 {usdt_swap_count} 个USDT永续合约)")

            return min_sz_dict

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


def save_to_file(min_sz_dict, output_path):
    """
    将最小下单张数和每张面值信息保存到文件

    Args:
        min_sz_dict: {symbol: {"minSz": float, "ctVal": float}} 的字典
        output_path: 输出文件路径
    """
    if not min_sz_dict:
        print("错误: 没有数据可保存")
        return False

    try:
        with open(output_path, 'w', encoding='utf-8') as f:
            # 写入文件头
            f.write("=" * 80 + "\n")
            f.write("OKX USDT永续合约最小下单张数(minSz)及每张面值(ctVal)\n")
            f.write(f"更新时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"数据来源: https://www.okx.com/api/v5/public/instruments?instType=SWAP\n")
            f.write(f"合约数量: {len(min_sz_dict)}\n")
            f.write("=" * 80 + "\n\n")

            # 按symbol排序后写入
            sorted_items = sorted(min_sz_dict.items(), key=lambda x: x[0])

            f.write(f"{'Symbol':<25} {'最小下单张数':<20} {'每张面值(币)':<20}\n")
            f.write("-" * 80 + "\n")

            for symbol, info in sorted_items:
                min_sz = info["minSz"]
                ct_val = info["ctVal"]
                # 格式化,去除不必要的小数点后的0
                min_sz_str = f"{min_sz:.8f}".rstrip('0').rstrip('.')
                ct_val_str = f"{ct_val:.8f}".rstrip('0').rstrip('.')
                f.write(f"{symbol:<25} {min_sz_str:<20} {ct_val_str:<20}\n")

            f.write("\n" + "=" * 80 + "\n")
            f.write("说明:\n")
            f.write("- minSz: 最小下单数量,单位为张\n")
            f.write("- ctVal: 每张合约面值,单位为币(如BTC、ETH等)\n")
            f.write("- 例如: BTC-USDT-SWAP 的 minSz=0.01张, ctVal=0.01\n")
            f.write("  表示最小下单0.01张, 每张=0.01BTC\n")
            f.write("- 下单张数计算: 张数 = 下单金额 / (当前价格 × ctVal)\n")
            f.write("=" * 80 + "\n")

        print(f"✓ 数据已保存到: {output_path}")
        return True

    except Exception as e:
        print(f"错误: 保存文件失败 - {e}")
        return False


def main():
    """主函数"""
    print("\n" + "=" * 80)
    print("OKX USDT永续合约最小下单张数及每张面值获取工具")
    print("=" * 80 + "\n")

    # 输出文件路径
    script_dir = os.path.dirname(os.path.abspath(__file__))
    output_path = os.path.join(script_dir, "..", "strategies", "configs", "okxmin.txt")

    # 获取数据
    min_sz_dict = fetch_okx_min_sz()

    if not min_sz_dict:
        print("\n✗ 获取数据失败")
        return 1

    # 保存到文件
    print()
    if save_to_file(min_sz_dict, output_path):
        print("\n✓ 完成!")

        # 显示一些示例数据
        print("\n示例数据(前10个):")
        print("-" * 80)
        for i, (symbol, info) in enumerate(sorted(min_sz_dict.items())[:10]):
            min_sz_str = f"{info['minSz']:.8f}".rstrip('0').rstrip('.')
            ct_val_str = f"{info['ctVal']:.8f}".rstrip('0').rstrip('.')
            print(f"  {symbol:<25} minSz={min_sz_str:<10} ctVal={ct_val_str}")
        print("-" * 80)

        return 0
    else:
        print("\n✗ 保存失败")
        return 1


if __name__ == "__main__":
    exit(main())
