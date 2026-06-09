# 性能优化清单 - Python 策略层

> 本文档供 Claude 逐项执行优化。每个条目包含：问题描述、涉及文件和行号、具体修改方法。

---

## PERF-P1: TickerDataLive 滚动窗口计算反复转 list（最大热点）

**文件**: `cpp/strategies/implementations/5_mom_factor_cs/live/five_mom_factor_live.py`
**行号**: 约 164-194（`get_rolling_mean`, `get_rolling_min`, `get_rolling_max` 等方法）

**问题**: 每次计算滚动窗口指标时，将整个 deque 转为 list 再切片，产生大量临时内存分配。每根 K 线触发 ~12 次此类调用，乘以 ~550 个币种。

**当前代码**:
```python
def get_rolling_mean(self, data: deque, window: int) -> Optional[float]:
    if len(data) < window:
        return None
    return np.mean(list(data)[-window:])  # deque->list->slice->np.array
```

**修改方法**: 使用 `itertools.islice` 避免完整 list 转换：
```python
from itertools import islice

def get_rolling_mean(self, data: deque, window: int) -> Optional[float]:
    if len(data) < window:
        return None
    arr = np.fromiter(islice(data, len(data) - window, len(data)), dtype=float, count=window)
    return arr.mean()

def get_rolling_min(self, data: deque, window: int) -> Optional[float]:
    if len(data) < window:
        return None
    return min(islice(data, len(data) - window, len(data)))

def get_rolling_max(self, data: deque, window: int) -> Optional[float]:
    if len(data) < window:
        return None
    return max(islice(data, len(data) - window, len(data)))
```

**预期收益**: 每 tick 节省 5-10% 的 CPU 时间。

---

## PERF-P2: _zscore() 重复转 list

**文件**: `cpp/strategies/implementations/5_mom_factor_cs/live/five_mom_factor_live.py`
**行号**: 约 379-392

**问题**: `_zscore()` 每次对 dict values 做两次 `list()` 转换分别算 mean 和 std。

**当前代码**:
```python
mean_val = np.mean(list(valid_values.values()))  # list() 第1次
std_val = np.std(list(valid_values.values()))     # list() 第2次
```

**修改方法**: 只转一次：
```python
arr = np.array(list(valid_values.values()))  # 只转一次
mean_val = arr.mean()
std_val = arr.std()
```

---

## PERF-P3: 持仓查询轮询 sleep(0.1) 过长

**文件**: `cpp/strategies/implementations/5_mom_factor_cs/live/five_mom_factor_live.py`
**行号**: `_fetch_current_positions()` 方法内的轮询循环

**问题**: C++ 通常在几百毫秒内返回持仓结果，但 Python 每次 sleep 0.1s 才检查一次，白白浪费 0~100ms。

**修改方法**: 缩短 sleep 到 0.02s：
```python
for i in range(1500):  # 1500 * 0.02s = 30s 最大等待
    self.poll_messages()
    if self.is_position_query_done():
        break
    time.sleep(0.02)
```

---

## PERF-P4: _refresh_tradeable_symbols() 阻塞式重试

**文件**: `cpp/strategies/implementations/5_mom_factor_cs/live/five_mom_factor_live.py`
**行号**: 约 400-438

**问题**: HTTP 请求失败后 `time.sleep(2 * attempt)` 阻塞策略线程，最差 12 秒。

**修改方法**: 缩短重试间隔：
```python
for attempt in range(1, max_retries + 1):
    try:
        resp = requests.get(f"{base_url}/fapi/v1/exchangeInfo", timeout=5, proxies=proxies)
    except Exception as e:
        self.log_error(f"[币种检查] 第{attempt}次请求异常: {e}")
        if attempt < max_retries:
            time.sleep(min(1 * (2 ** (attempt - 1)), 4))  # 1s, 2s, 4s
            continue
        self.log_error("[币种检查] 重试耗尽，保留上次缓存")
        return
```

---

## PERF-P5: fast_kline_filler.py dedup_klines 用 defaultdict

**文件**: `cpp/scripts/fast_kline_filler.py`
**行号**: 163-169

**当前代码**:
```python
ts_groups = {}
for val, score in raw:
    ts = int(score)
    if ts not in ts_groups:
        ts_groups[ts] = []
    ts_groups[ts].append(val)
```

**修改方法**:
```python
from collections import defaultdict
ts_groups = defaultdict(list)
for val, score in raw:
    ts_groups[int(score)].append(val)
```

---

## PERF-P6: aggregate_and_write() 对无变化的币种做无意义查询

**文件**: `cpp/scripts/fast_kline_filler.py`
**行号**: 341-393 和 process_exchange_batch 中的步骤4

**问题**: process_exchange_batch 内对每个有缺失的币种都执行 6 个周期的聚合检查，即使该币种只缺 1 根 1m 且实际没补到数据。

**修改方法**: 仅当步骤2实际补全了 1m 数据时才执行步骤3和步骤4：
```python
for ex, symbol, _ in symbols_with_gaps:
    dedup_klines(r, ex, symbol, "1m", start_ms, end_ms)
    gaps = detect_1m_gaps(r, ex, symbol, start_ms, end_ms)
    filled = 0
    if gaps:
        # ... 补全逻辑 ...

    # 只有实际补全了数据才需要重新聚合
    if filled > 0:
        for ti in AGGREGATION:
            dedup_klines(r, ex, symbol, ti, start_ms, end_ms)
        for ti in AGGREGATION:
            count = aggregate_and_write(r, ex, symbol, ti, start_ms, end_ms)
            if count > 0:
                agg_info[ti] = count
                total_agg[ti] = total_agg.get(ti, 0) + count
```

---

## 优先级排序

| 编号 | 预期收益 | 改动复杂度 | 建议优先级 |
|------|---------|-----------|-----------|
| PERF-P1 | 5-10% tick 延迟降低 | 低 | 第一优先 |
| PERF-P3 | 持仓查询响应快 50-80ms | 极低 | 第一优先 |
| PERF-P2 | 调仓计算加速 | 极低 | 第二优先 |
| PERF-P4 | 最差情况减少 8s 阻塞 | 极低 | 第二优先 |
| PERF-P6 | 减少无效 Redis 查询 | 低 | 第三优先 |
| PERF-P5 | 微小 | 极低 | 随手改 |
