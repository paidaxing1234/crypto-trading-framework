"""Binance minQty 配置文件热同步工具

从 Binance fapi/v1/exchangeInfo 拉取的 LOT_SIZE.minQty 写入 binancemin.txt，
并调用策略基类的 load_min_order_config() 重载 C++ 内存映射，
让下一次调仓立即用上最新的最小下单单位。
"""

from __future__ import annotations

import os
from datetime import datetime
from typing import Iterable, Mapping


def extract_min_qty_from_exchange_info(exchange_info: Mapping) -> dict:
    """从 fapi/v1/exchangeInfo 响应中抽取 {symbol: minQty}。

    只保留 status=TRADING 且 contractType=PERPETUAL 的合约。
    """
    result: dict = {}
    for s in exchange_info.get("symbols", []) or []:
        if s.get("status") != "TRADING":
            continue
        if s.get("contractType") != "PERPETUAL":
            continue
        sym = s.get("symbol")
        if not sym:
            continue
        for f in s.get("filters", []) or []:
            if f.get("filterType") == "LOT_SIZE":
                try:
                    result[sym] = float(f.get("minQty", 0))
                except (TypeError, ValueError):
                    pass
                break
    return result


def extract_min_notional_from_exchange_info(exchange_info: Mapping) -> dict:
    """从 fapi/v1/exchangeInfo 响应中抽取 {symbol: minNotional}。

    只保留 status=TRADING 且 contractType=PERPETUAL 的合约。
    """
    result: dict = {}
    for s in exchange_info.get("symbols", []) or []:
        if s.get("status") != "TRADING":
            continue
        if s.get("contractType") != "PERPETUAL":
            continue
        sym = s.get("symbol")
        if not sym:
            continue
        for f in s.get("filters", []) or []:
            if f.get("filterType") == "MIN_NOTIONAL":
                try:
                    result[sym] = float(f.get("notional", 0))
                except (TypeError, ValueError):
                    pass
                break
    return result


def _read_existing(min_file: str) -> dict:
    """解析现有 binancemin.txt，返回 {symbol: minQty}。文件不存在或解析失败返回 {}。"""
    out: dict = {}
    if not os.path.exists(min_file):
        return out
    try:
        in_data = False
        with open(min_file, "r", encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line:
                    continue
                if "----" in line:
                    in_data = True
                    continue
                if "====" in line or line.startswith("说明:") or line.startswith("-"):
                    in_data = False
                    continue
                if in_data:
                    parts = line.split()
                    if len(parts) >= 2:
                        try:
                            out[parts[0]] = float(parts[1])
                        except ValueError:
                            pass
    except OSError:
        return {}
    return out


def _read_min_notional(notional_file: str) -> dict:
    """解析现有 binance_min_notional.txt，结构同 binancemin.txt。"""
    return _read_existing(notional_file)


def _write_atomic(min_file: str, latest: Mapping[str, float]) -> None:
    header = (
        "================================================================================\n"
        "Binance USDT永续合约最小下单数量(minQty)\n"
        f"更新时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n"
        "数据来源: https://fapi.binance.com/fapi/v1/exchangeInfo\n"
        f"合约数量: {len(latest)}\n"
        "================================================================================\n"
        f"{'Symbol':<20} {'minQty':<20}\n"
        "--------------------------------------------------------------------------------\n"
    )
    footer = (
        "================================================================================\n"
        "说明:\n"
        "- minQty: 最小下单数量(单位:币,如BTC、ETH等)\n"
        "- 下单数量必须是minQty的整数倍\n"
        "- 实际下单时还需要满足其他过滤器要求(如stepSize、notional等)\n"
        "================================================================================\n"
    )
    tmp = min_file + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(header)
        for sym in sorted(latest):
            f.write(f"{sym:<20} {latest[sym]:<20g}\n")
        f.write(footer)
    os.replace(tmp, min_file)


def _write_min_notional(notional_file: str, latest: Mapping[str, float]) -> None:
    header = (
        "================================================================================\n"
        "Binance USDT永续合约最小下单名义额(MIN_NOTIONAL)\n"
        f"更新时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n"
        "数据来源: https://fapi.binance.com/fapi/v1/exchangeInfo (filters[].MIN_NOTIONAL.notional)\n"
        f"合约数量: {len(latest)}\n"
        "================================================================================\n"
        f"{'Symbol':<20} {'minNotional':<20}\n"
        "--------------------------------------------------------------------------------\n"
    )
    footer = (
        "================================================================================\n"
        "说明:\n"
        "- minNotional: 单笔下单的最小名义价值(USDT)\n"
        "- 不满足时交易所会拒单 (-4164)；reduceOnly 单不受限\n"
        "================================================================================\n"
    )
    tmp = notional_file + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(header)
        for sym in sorted(latest):
            f.write(f"{sym:<20} {latest[sym]:<20g}\n")
        f.write(footer)
    os.replace(tmp, notional_file)


def sync_binance_min_qty(strategy, latest: Mapping[str, float], configs_dir: str) -> bool:
    """对比并热更新 binancemin.txt + C++ 内存映射。

    Args:
        strategy: 持有 log_info / log_error / load_min_order_config 的策略实例
        latest: 本次 exchangeInfo 解析出的 {symbol: minQty}
        configs_dir: cpp/strategies/configs 目录

    Returns:
        True 表示发生了变更并已重载；False 表示无变更或写入失败。
    """
    if not latest:
        return False
    try:
        min_file = os.path.join(configs_dir, "binancemin.txt")
        current = _read_existing(min_file)

        if current == latest:
            return False

        added = set(latest) - set(current)
        removed = set(current) - set(latest)
        changed = {k for k in set(latest) & set(current) if latest[k] != current[k]}

        _write_atomic(min_file, latest)

        strategy.log_info(
            f"[最小下单单位] 已同步 binancemin.txt: 总 {len(latest)} | "
            f"新增 {len(added)} | 移除 {len(removed)} | 变更 {len(changed)}"
        )
        if added:
            preview = sorted(added)[:20]
            suffix = "..." if len(added) > 20 else ""
            strategy.log_info(f"[最小下单单位] 新增: {preview}{suffix}")
            _preset_leverage_for_new_symbols(strategy, sorted(added))

        if strategy.load_min_order_config("binance", configs_dir):
            strategy.log_info("[最小下单单位] 已热重载到内存")
            return True
        strategy.log_error("[最小下单单位] 热重载失败")
        return False
    except Exception as e:
        strategy.log_error(f"[最小下单单位] 同步异常: {e}")
        return False


def sync_binance_min_notional(strategy, latest: Mapping[str, float], configs_dir: str) -> dict:
    """对比并热更新 binance_min_notional.txt，并把最新表存到 strategy.binance_min_notional。

    Args:
        strategy: 持有 log_info / log_error 的策略实例
        latest: 本次 exchangeInfo 解析出的 {symbol: minNotional}
        configs_dir: cpp/strategies/configs 目录

    Returns:
        最新的 {symbol: minNotional} 字典（即使没写盘也返回，调用方便）。
    """
    if not latest:
        return {}
    try:
        notional_file = os.path.join(configs_dir, "binance_min_notional.txt")
        current = _read_min_notional(notional_file)

        # 始终把最新表挂到策略实例上，下单前可直接查
        setattr(strategy, "binance_min_notional", dict(latest))

        if current == latest:
            return latest

        added = set(latest) - set(current)
        removed = set(current) - set(latest)
        changed = {k for k in set(latest) & set(current) if latest[k] != current[k]}

        _write_min_notional(notional_file, latest)

        strategy.log_info(
            f"[最小名义额] 已同步 binance_min_notional.txt: 总 {len(latest)} | "
            f"新增 {len(added)} | 移除 {len(removed)} | 变更 {len(changed)}"
        )
        if changed:
            sample = list(sorted(changed))[:10]
            details = [f"{s}: {current[s]}->{latest[s]}" for s in sample]
            strategy.log_info(f"[最小名义额] 变更样例: {details}")
        return latest
    except Exception as e:
        strategy.log_error(f"[最小名义额] 同步异常: {e}")
        return latest


def sync_from_exchange_info(strategy, exchange_info: Mapping, configs_dir: str) -> bool:
    """便利函数：直接吃 exchangeInfo 响应，同步 minQty 和 MIN_NOTIONAL。"""
    sync_binance_min_notional(
        strategy,
        extract_min_notional_from_exchange_info(exchange_info),
        configs_dir,
    )
    return sync_binance_min_qty(
        strategy,
        extract_min_qty_from_exchange_info(exchange_info),
        configs_dir,
    )


def _preset_leverage_for_new_symbols(strategy, new_symbols: Iterable[str]) -> None:
    """对新增的 Binance 永续把杠杆预设为 1x。

    - 仅对 strategy.exchange == "binance" 生效；其他情况静默跳过。
    - 不阻塞主流程：单个 symbol 失败仅降级 warning。
    - 调用方为 sync_binance_min_qty，运行时机在 _refresh_tradeable_symbols 内、
      调仓下单逻辑之前，因此设置后立刻就被这一轮调仓使用。
    """
    if not new_symbols:
        return
    if getattr(strategy, "exchange", None) != "binance":
        return
    if not hasattr(strategy, "change_leverage"):
        strategy.log_error("[杠杆预设] 策略实例缺少 change_leverage，无法设置新增币种")
        return

    ok = 0
    fail = 0
    failed_examples = []
    for sym in new_symbols:
        try:
            res = strategy.change_leverage(sym, 1, "binance")
        except Exception as e:
            fail += 1
            if len(failed_examples) < 5:
                failed_examples.append(f"{sym}: {e}")
            continue
        if res:
            ok += 1
        else:
            fail += 1
            if len(failed_examples) < 5:
                failed_examples.append(f"{sym}: change_leverage 返回 False")

    strategy.log_info(
        f"[杠杆预设] 新增币种设为 1x: 成功 {ok} | 失败 {fail} | 共 {ok + fail}"
    )
    if failed_examples:
        strategy.log_error(f"[杠杆预设] 失败样例: {failed_examples}")
