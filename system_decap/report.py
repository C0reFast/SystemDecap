"""Render a standalone, offline, data-dense HTML laboratory report."""

from __future__ import annotations

import html
import json
import math
from collections import defaultdict
from datetime import datetime
from typing import Any, Callable


PALETTE = ["#ff6b35", "#00a6a6", "#e3b341", "#4f7cff", "#ce5a9e", "#78b159", "#8f72d8"]

CONFIDENCE_ZH = {
    "high": "高", "medium": "中", "low": "低", "unavailable": "不可用", "unknown": "未知",
}
RELATION_ZH = {
    "smt-sibling": "SMT 同核线程",
    "same-llc-different-core": "共享 LLC 的不同核心",
    "cross-llc-same-numa": "同 NUMA 跨 LLC",
    "same-numa-different-core": "同 NUMA 不同核心",
    "cross-numa-same-socket": "同插槽跨 NUMA",
    "cross-socket": "跨插槽",
    "unknown": "未知关系",
}
OPERATION_ZH = {"read": "读取", "write": "写入", "copy": "复制", "triad": "三元运算"}
PATTERN_ZH = {"always-taken": "始终跳转", "alternating": "交替跳转", "random": "随机跳转"}
KERNEL_ZH = {
    "nop_frontend": "NOP 前端流",
    "integer_add_dependency": "整数加法·依赖链",
    "integer_add_parallel4": "整数加法·4 条并行链",
    "integer_add_parallel8": "整数加法·8 条并行链",
    "integer_mul_dependency": "整数乘法·依赖链",
    "integer_mul_parallel4": "整数乘法·4 条并行链",
    "fp64_add_dependency": "FP64 加法·依赖链",
    "fp64_add_parallel4": "FP64 加法·4 条并行链",
    "fp64_mul_dependency": "FP64 乘法·依赖链",
    "fp64_mul_parallel4": "FP64 乘法·4 条并行链",
}
GROUP_ZH = {
    "timer": "计时器", "cache_latency": "缓存延迟", "tlb_latency": "TLB 延迟",
    "memory_bandwidth": "内存带宽", "cache_bandwidth": "缓存带宽",
    "memory_access": "内存访问", "memory_parallelism": "内存级并行",
    "page_policy": "页面策略", "loaded_memory_latency": "带载内存延迟",
    "store_forwarding": "存储转发", "instruction_fetch": "指令侧带宽",
    "core_latency": "核间延迟", "coherence": "缓存一致性", "numa": "NUMA",
    "pipeline": "核心流水线", "branch": "分支预测", "reorder_window": "乱序窗口",
    "compute_scaling": "计算扩展", "branch_structure": "分支结构",
    "os_overhead": "操作系统开销",
}
METRIC_ZH = {
    "steady_clock_call": "单调时钟读取开销",
    "cycle_counter_min": "周期计数器最小读取开销",
    "cycle_counter_mean": "周期计数器平均读取开销",
    "counter_frequency": "平台计数器频率",
    "random_load_latency": "随机依赖加载延迟",
    "page_random_load_latency": "逐页随机加载延迟",
    "stream_bandwidth": "流式有效载荷带宽",
    "working_set_bandwidth": "工作集带宽",
    "stride_access_rate": "固定步长访问速率",
    "stride_payload_bandwidth": "固定步长有效载荷带宽",
    "effective_load_latency": "有效加载延迟",
    "random_load_rate": "随机加载速率",
    "random_load_latency_under_load": "带载随机加载延迟",
    "concurrent_read_bandwidth": "并发读取带宽",
    "store_load_latency": "Store/load 对延迟",
    "store_load_counter_ticks": "Store/load 对计数器 tick",
    "code_delivery_bandwidth": "代码输送吞吐",
    "instruction_rate": "指令执行速率",
    "cacheline_handoff_latency": "缓存行核间传递延迟",
    "atomic_update_rate": "原子更新速率",
    "load_latency": "加载延迟",
    "read_bandwidth": "读取带宽",
    "operation_throughput": "微内核操作吞吐",
    "counter_tick_efficiency": "平台计数器效率",
    "effective_core_frequency": "有效核心频率",
    "operations_per_cycle": "每周期操作数",
    "ipc": "每周期退休指令数（IPC）",
    "cycles_per_operation": "每操作周期数",
    "cache_miss_rate": "缓存未命中率",
    "time_per_branch": "单分支耗时",
    "miss_rate": "分支误预测率",
    "cold_load_overlap_penalty": "冷加载重叠损失",
    "rob_capacity_proxy": "ROB/乱序窗口容量代理值",
    "integer_add_throughput": "整数加法聚合吞吐",
    "btb_branch_latency": "BTB 直接跳转耗时",
    "btb_counter_ticks": "BTB 直接跳转计数器 tick",
    "btb_miss_rate": "BTB 压力下分支未命中率",
    "history_period_latency": "方向历史周期分支耗时",
    "history_period_miss_rate": "方向历史周期未命中率",
    "return_stack_latency": "返回地址栈压力下返回耗时",
    "return_stack_miss_rate": "返回地址栈压力下分支未命中率",
    "indirect_call_latency": "间接调用目标切换耗时",
    "indirect_call_miss_rate": "间接调用目标未命中率",
    "getpid_syscall": "getpid 系统调用",
    "anonymous_page_first_touch": "匿名页首次触碰",
    "minor_faults": "次缺页次数",
    "scheduler_pipe_handoff": "线程调度/管道交接",
}
CATEGORY_ZH = {
    "identity": "平台身份", "topology": "拓扑", "cache": "缓存", "latency": "延迟",
    "bandwidth": "带宽", "memory": "内存访问", "coherence": "缓存一致性",
    "core": "核心", "branch": "分支", "os": "操作系统", "environment": "运行环境",
}
KIND_ZH = {
    "inventory": "系统清点", "measured": "直接测量", "inferred": "推断",
    "measured with perf": "使用 perf 测量", "inferred on x86": "x86 平台推断",
    "measured proxy": "直接测量的代理曲线",
}
CACHE_TYPE_ZH = {"Data": "数据", "Instruction": "指令", "Unified": "统一"}
METHOD_ZH = {
    "back-to-back steady_clock calls": "连续调用 steady_clock",
    "serialized back-to-back counter reads": "串行化连续读取平台计数器",
    "counter delta over monotonic wall interval": "单调墙钟区间内的平台计数器增量",
    "random dependent pointer chase": "随机依赖指针追逐",
    "one random dependent access per base page": "每个基础页一次随机依赖访问",
    "parallel pinned streaming kernel; payload bytes": "固定线程并行流式内核；只统计有效载荷字节",
    "one-core repeated streaming kernel by working-set size": "单核按工作集大小重复执行流式内核",
    "one-core sequential fixed-stride load sweep": "单核顺序固定步长加载扫描",
    "requested 8-byte payload only; cache-line traffic excluded": "只统计请求的 8 字节有效载荷；不含缓存行流量",
    "independent random dependent-load chains": "多条独立的随机依赖加载链",
    "interleaved independent random dependent-load chains": "交错执行多条独立随机依赖加载链",
    "same pointer chase with MADV_NOHUGEPAGE versus MADV_HUGEPAGE": "同一指针追逐工作集对比 MADV_NOHUGEPAGE 与 MADV_HUGEPAGE",
    "dependent pointer chase while pinned cores generate streaming read traffic": "固定核心生成流式读取压力时执行依赖指针追逐",
    "payload generated concurrently with the dependent-load latency probe": "与依赖加载延迟探针并发产生的读取有效载荷",
    "dependent store followed by overlapping load": "依赖 store 后紧跟存在覆盖关系的 load",
    "platform counter ticks per dependent store/load pair": "每个依赖 store/load 对的平台计数器 tick",
    "W^X generated NOP body scanned repeatedly": "以 W^X 方式生成并重复扫描 NOP 指令体",
    "W^X generated A64 NOP body scanned repeatedly": "以 W^X 方式生成并重复扫描 A64 NOP 指令体",
    "known generated NOP count divided by wall time": "已知生成 NOP 数除以墙钟时间",
    "known generated A64 NOP count divided by wall time": "已知生成 A64 NOP 数除以墙钟时间",
    "perf retired instructions / core cycles for generated code": "生成代码的 perf 退休指令数除以核心周期",
    "parallel pinned independent integer-add chains": "固定线程并行执行相互独立的整数加法链",
    "median perf core cycles per wall second across active workers": "活动工作线程的 perf 核心周期/墙钟时间中位数",
    "generated taken direct branches with independently varied footprint": "独立改变代码足迹的已跳转直接分支序列",
    "platform counter ticks per generated taken branch": "每条生成的已跳转直接分支的平台计数器 tick",
    "generic perf branch misses during BTB footprint sweep": "BTB 足迹扫描期间的通用 perf 分支未命中",
    "scalar conditional branch over a repeating pseudo-random direction period": "重复伪随机方向周期上的标量条件分支",
    "generic perf branch misses during history-period sweep": "方向历史周期扫描期间的通用 perf 分支未命中",
    "generated nested calls with a unique return address at each depth": "每层具有唯一返回地址的生成式嵌套调用",
    "generic perf branch misses during nested call/return chain": "嵌套调用/返回链期间的通用 perf 分支未命中",
    "one indirect call site visits a shuffled set of generated return targets": "一个间接调用点按打乱顺序访问生成的返回目标集合",
    "generic perf branch misses during indirect target sweep": "间接目标扫描期间的通用 perf 分支未命中",
    "release/acquire cache-line ping-pong": "release/acquire 缓存行乒乓",
    "two independent atomics at varying byte separation": "两个独立原子变量使用不同字节间距",
    "NUMA-placed random pointer chase": "在指定 NUMA 节点放置页面后执行随机指针追逐",
    "pinned NUMA aggregate read payload": "固定线程的 NUMA 聚合读取有效载荷",
    "pinned NUMA aggregate read payload with LLC coverage metadata": "固定线程的 NUMA 聚合读取有效载荷，并记录读取节点 LLC 覆盖证据",
    "architecture-specific scalar assembly microkernel": "架构专用标量汇编微内核",
    "operations divided by invariant/platform counter ticks": "操作数除以不变/平台计数器 tick",
    "perf core cycles divided by wall time during kernel": "微内核期间 perf 核心周期除以墙钟时间",
    "known operations divided by perf core cycles": "已知操作数除以 perf 核心周期",
    "perf retired instructions / core cycles": "perf 退休指令数除以核心周期",
    "perf core cycles divided by known operations": "perf 核心周期除以已知操作数",
    "generic perf cache misses/references": "通用 perf 缓存未命中数/访问数",
    "forced scalar data-dependent branch loop": "强制保留的标量数据依赖分支循环",
    "perf branch misses / branch instructions": "perf 分支未命中数除以分支指令数",
    "two flushed loads separated by a dynamic independent-uop window": "两次已刷新的加载之间插入动态独立 µop 窗口",
    "first loss of overlap; loop body is approximately two fused-domain uops": "首次失去重叠；循环体近似为两个融合域 µop",
    "two flushed loads separated by an exact static one-uop instruction window": "两次已刷新的加载之间插入精确静态展开的一微操作指令窗口",
    "first sustained loss of overlap in an exact static one-uop filler sequence": "精确静态一微操作填充序列中首次连续失去加载重叠",
    "direct SYS_getpid loop": "直接调用 SYS_getpid 的循环",
    "write one byte per anonymous base page": "每个匿名基础页写入一个字节",
    "getrusage delta around anonymous first touch": "匿名页首次触碰前后的 getrusage 差值",
    "blocking pipe ping-pong between pinned threads": "固定线程之间使用阻塞管道乒乓交接",
}


def _value_zh(key: str, value: Any) -> str:
    text = str(value)
    mappings = {
        "operation": OPERATION_ZH, "relation": RELATION_ZH,
        "pattern": PATTERN_ZH, "kernel": KERNEL_ZH,
        "policy": {"base-page-advised": "基础页建议", "thp-advised": "透明大页建议"},
        "scope": {"physical-cores": "物理核心", "smt-siblings": "SMT 同核线程"},
        "branch_type": {"unconditional": "无条件跳转", "conditional-taken": "始终跳转条件分支"},
        "case": {
            "exact-8-to-8": "同址 8→8", "partial-4-to-8": "部分覆盖 4→8",
            "overlap-offset-1": "错位重叠 +1", "split-cache-line": "跨缓存行",
        },
        "matrix_scope": {
            "physical-core": "物理核心矩阵", "logical-cpu": "逻辑 CPU 矩阵",
            "representative": "代表性采样",
        },
        "local": {"true": "本地", "false": "远端"},
        "bound": {"frontend": "前端约束", "backend": "后端约束", "dependency": "依赖链约束"},
    }
    return mappings.get(key, {}).get(text, text)


def _label_text(labels: dict[str, Any]) -> str:
    return " · ".join(f"{key}={_value_zh(key, value)}" for key, value in sorted(labels.items()))


def _method_zh(value: Any) -> str:
    text = str(value or "")
    return METHOD_ZH.get(text, text)


def _h(value: Any) -> str:
    return html.escape(str(value), quote=True)


def _human_bytes(value: float | int | None) -> str:
    if value is None:
        return "—"
    number = float(value)
    for suffix in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(number) < 1024 or suffix == "TiB":
            return f"{number:.0f} {suffix}" if suffix == "B" else f"{number:.2f} {suffix}"
        number /= 1024
    return f"{number:.2f} TiB"


def _number(value: Any, unit: str = "") -> str:
    if value is None:
        return "—"
    if isinstance(value, str):
        return value
    number = float(value)
    if unit == "bytes":
        return _human_bytes(number)
    if abs(number) >= 1000:
        result = f"{number:,.0f}"
    elif abs(number) >= 100:
        result = f"{number:.1f}"
    elif abs(number) >= 10:
        result = f"{number:.2f}"
    else:
        result = f"{number:.3f}"
    return f"{result} {unit}".strip()


def _estimate_map(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {item["key"]: item for item in report.get("estimates", [])}


def _obs(report: dict[str, Any], group: str, metric: str) -> list[dict[str, Any]]:
    return [
        item for item in report.get("observations", [])
        if item.get("group") == group and item.get("metric") == metric
    ]


def _empty_chart(title: str, note: str) -> str:
    return f"""
    <div class="chart empty-chart">
      <div class="chart-heading"><h3>{_h(title)}</h3></div>
      <div class="empty-mark">∅</div><p>{_h(note)}</p>
    </div>"""


def _line_chart(
    title: str,
    items: list[dict[str, Any]],
    x_value: Callable[[dict[str, Any]], float],
    series_value: Callable[[dict[str, Any]], str],
    x_display: Callable[[float], str],
    unit: str,
    note: str,
    log_x: bool = False,
) -> str:
    if not items:
        return _empty_chart(title, note)
    width, height = 860, 330
    left, right, top, bottom = 76, 24, 40, 58
    plot_w, plot_h = width - left - right, height - top - bottom
    grouped: dict[str, list[tuple[float, float, dict[str, Any]]]] = defaultdict(list)
    for item in items:
        try:
            x = float(x_value(item))
            y = float(item["value"])
            if (x > 0 if log_x else x >= 0) and math.isfinite(x) and math.isfinite(y):
                grouped[series_value(item)].append((x, y, item))
        except (KeyError, TypeError, ValueError):
            continue
    if not grouped:
        return _empty_chart(title, note)
    all_points = [point for points in grouped.values() for point in points]
    raw_x = [point[0] for point in all_points]
    ys = [point[1] for point in all_points]
    transformed_x = [math.log2(x) if log_x else x for x in raw_x]
    x_min, x_max = min(transformed_x), max(transformed_x)
    y_min, y_max = min(0.0, min(ys)), max(ys)
    if x_min == x_max:
        x_max += 1
    if y_min == y_max:
        y_max += 1
    y_max *= 1.08

    def px(value: float) -> float:
        transformed = math.log2(value) if log_x else value
        return left + (transformed - x_min) / (x_max - x_min) * plot_w

    def py(value: float) -> float:
        return top + plot_h - (value - y_min) / (y_max - y_min) * plot_h

    elements = [
        f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{_h(title)}">',
        f'<rect class="plot-bg" x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" rx="5"/>',
    ]
    for tick in range(6):
        value = y_min + (y_max - y_min) * tick / 5
        y = py(value)
        elements.append(f'<line class="grid" x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}"/>')
        elements.append(f'<text class="axis" x="{left - 10}" y="{y + 4:.2f}" text-anchor="end">{value:.2f}</text>')
    unique_x = sorted(set(raw_x))
    tick_x = unique_x if len(unique_x) <= 8 else [unique_x[round(i * (len(unique_x) - 1) / 7)] for i in range(8)]
    for value in tick_x:
        x = px(value)
        elements.append(f'<line class="tick" x1="{x:.2f}" y1="{top + plot_h}" x2="{x:.2f}" y2="{top + plot_h + 6}"/>')
        elements.append(f'<text class="axis" x="{x:.2f}" y="{top + plot_h + 24}" text-anchor="middle">{_h(x_display(value))}</text>')
    for series_index, (series, points) in enumerate(sorted(grouped.items())):
        color = PALETTE[series_index % len(PALETTE)]
        points.sort(key=lambda point: point[0])
        coords = " ".join(f"{px(x):.2f},{py(y):.2f}" for x, y, _ in points)
        elements.append(f'<polyline class="series-line" points="{coords}" stroke="{color}"/>')
        for x, y, item in points:
            label_detail = _label_text(item.get("labels", {}))
            elements.append(
                f'<circle class="point" cx="{px(x):.2f}" cy="{py(y):.2f}" r="4.5" fill="{color}">'
                f'<title>{_h(series)} · {_h(label_detail)} · {y:.4f} {_h(unit)}</title></circle>'
            )
    elements.append(f'<text class="axis-label" x="18" y="{top + plot_h / 2}" transform="rotate(-90 18 {top + plot_h / 2})" text-anchor="middle">{_h(unit)}</text>')
    elements.append("</svg>")
    legend = "".join(
        f'<span><i style="--swatch:{PALETTE[index % len(PALETTE)]}"></i>{_h(series)}</span>'
        for index, series in enumerate(sorted(grouped))
    )
    return f"""
    <div class="chart reveal">
      <div class="chart-heading"><h3>{_h(title)}</h3><div class="legend">{legend}</div></div>
      {''.join(elements)}
      <p class="chart-note">{_h(note)}</p>
    </div>"""


def _bar_chart(title: str, rows: list[tuple[str, float, str]], note: str) -> str:
    if not rows:
        return _empty_chart(title, note)
    width = 860
    row_height = 36
    top, left, right, bottom = 26, 210, 180, 28
    height = top + bottom + row_height * len(rows)
    maximum = max((value for _, value, _ in rows), default=1) or 1
    plot_w = width - left - right
    elements = [f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{_h(title)}">']
    for index, (label, value, unit) in enumerate(rows):
        y = top + index * row_height
        bar_w = max(2, value / maximum * plot_w)
        color = PALETTE[index % len(PALETTE)]
        elements.append(f'<text class="bar-label" x="{left - 12}" y="{y + 20}" text-anchor="end">{_h(label)}</text>')
        elements.append(f'<rect class="bar-track" x="{left}" y="{y + 7}" width="{plot_w}" height="18" rx="3"/>')
        elements.append(f'<rect class="bar" x="{left}" y="{y + 7}" width="{bar_w:.2f}" height="18" rx="3" fill="{color}"/>')
        elements.append(f'<text class="bar-value" x="{min(width - 4, left + bar_w + 8):.2f}" y="{y + 21}">{value:.3f} {_h(unit)}</text>')
    elements.append("</svg>")
    return f"""<div class="chart reveal"><div class="chart-heading"><h3>{_h(title)}</h3></div>
    {''.join(elements)}<p class="chart-note">{_h(note)}</p></div>"""


def _heatmap(
    title: str,
    items: list[dict[str, Any]],
    row_key: str,
    column_key: str,
    unit: str,
    note: str,
    row_prefix: str = "CPU",
    column_prefix: str = "MEM",
    zoomable: bool = False,
) -> str:
    if not items:
        return _empty_chart(title, note)
    rows = sorted({item.get("labels", {}).get(row_key, "?") for item in items}, key=lambda x: int(x) if str(x).isdigit() else str(x))
    columns = sorted({item.get("labels", {}).get(column_key, "?") for item in items}, key=lambda x: int(x) if str(x).isdigit() else str(x))
    values = {(item["labels"].get(row_key, "?"), item["labels"].get(column_key, "?")): float(item["value"]) for item in items}
    present = list(values.values())
    minimum, maximum = min(present), max(present)

    def color(value: float) -> str:
        ratio = 0.5 if maximum == minimum else (value - minimum) / (maximum - minimum)
        # Deep teal -> amber -> signal orange.
        if ratio < 0.5:
            local = ratio * 2
            start, end = (0, 122, 128), (227, 179, 65)
        else:
            local = (ratio - 0.5) * 2
            start, end = (227, 179, 65), (255, 107, 53)
        rgb = tuple(round(a + (b - a) * local) for a, b in zip(start, end))
        return f"rgb{rgb}"

    header = "<th></th>" + "".join(f"<th>{_h(column_prefix)} { _h(column) }</th>" for column in columns)
    body = []
    for row in rows:
        cells = []
        for column in columns:
            value = values.get((row, column))
            if value is None:
                cells.append('<td class="missing">—</td>')
            else:
                cells.append(
                    f'<td class="measured" style="--cell:{color(value)}" '
                    f'title="{_h(row_prefix)} {_h(row)} → {_h(column_prefix)} {_h(column)}: {value:.4f} {_h(unit)}">'
                    f'<strong>{value:.2f}</strong><small>{_h(unit)}</small></td>'
                )
        body.append(f"<tr><th>{_h(row_prefix)} { _h(row) }</th>{''.join(cells)}</tr>")
    table = (
        f'<table class="heatmap" data-rows="{len(rows)}" data-columns="{len(columns)}" '
        f'style="--columns:{len(columns)}"><thead><tr>{header}</tr></thead>'
        f'<tbody>{"".join(body)}</tbody></table>'
    )
    if zoomable:
        content = f"""
        <div class="matrix-toolbar" role="toolbar" aria-label="核间延迟矩阵缩放工具">
          <button type="button" data-zoom-action="out" aria-label="缩小矩阵">−</button>
          <input type="range" min="1" max="200" step="1" value="100" data-zoom-range aria-label="矩阵缩放比例">
          <output data-zoom-output>100%</output>
          <button type="button" data-zoom-action="in" aria-label="放大矩阵">＋</button>
          <button type="button" class="text-button" data-zoom-action="fit">适配视口</button>
          <button type="button" class="text-button" data-zoom-action="actual">100%</button>
          <span>{len(rows)} × {len(columns)} · 拖动平移 · Ctrl/⌘＋滚轮缩放</span>
        </div>
        <div class="matrix-viewport" data-matrix-viewer tabindex="0"
             aria-label="可缩放的 {_h(title)}">
          <div class="matrix-stage">{table}</div>
        </div>"""
    else:
        content = f'<div class="table-scroll">{table}</div>'
    return f"""
    <div class="chart reveal heatmap-card{' zoomable-heatmap' if zoomable else ''}">
      <div class="chart-heading"><h3>{_h(title)}</h3>
      <span class="range">{minimum:.2f} → {maximum:.2f} {_h(unit)}</span></div>
      {content}
      <p class="chart-note">{_h(note)}</p>
    </div>"""


def _metric_card(item: dict[str, Any] | None, eyebrow: str) -> str:
    if not item or not item.get("available"):
        basis = item.get("basis", "未采集或当前平台不适用") if item else "未采集或当前平台不适用"
        caveat = item.get("caveat", "") if item else ""
        detail = f'<div class="metric-detail">{_h(caveat)}</div>' if caveat else ""
        return (
            f'<article class="metric unavailable"><span>{_h(eyebrow)}</span>'
            f'<strong>不可用</strong><small>{_h(_method_zh(basis))}</small>{detail}</article>'
        )
    confidence = item.get("confidence", "unknown")
    return f"""
    <article class="metric reveal">
      <span>{_h(eyebrow)}</span>
      <strong>{_h(_number(item.get('value'), item.get('unit', '')))}</strong>
      <small><i class="confidence {confidence}"></i>置信度：{_h(CONFIDENCE_ZH.get(confidence, confidence))}</small>
      <div class="metric-detail"><b>{_h(item.get('name'))}</b><br>{_h(_method_zh(item.get('basis')))}<br>{_h(item.get('caveat', ''))}</div>
    </article>"""


def _topology_visual(system: dict[str, Any]) -> str:
    topology = system.get("topology", {})
    cpus = [cpu for cpu in topology.get("cpus", []) if cpu.get("allowed") and cpu.get("online")]
    by_node: dict[Any, list[dict[str, Any]]] = defaultdict(list)
    for cpu in cpus:
        by_node[cpu.get("node", 0)].append(cpu)
    nodes = []
    for node, node_cpus in sorted(by_node.items()):
        by_core: dict[tuple[Any, Any, Any], list[int]] = defaultdict(list)
        for cpu in node_cpus:
            by_core[(cpu.get("socket"), cpu.get("die"), cpu.get("core"))].append(cpu["cpu"])
        core_html = "".join(
            f'<span class="core" title="插槽 {key[0]}，die {key[1]}，核心 {key[2]}">'
            f'<b>C{key[2]}</b><em>{"/".join(map(str, threads))}</em></span>'
            for key, threads in sorted(by_core.items())
        )
        nodes.append(
            f'<div class="node"><header><span>NUMA</span><strong>{_h(node)}</strong>'
            f'<small>{len(by_core)} 个核心 · {len(node_cpus)} 个线程</small></header><div class="cores">{core_html}</div></div>'
        )
    return f'<div class="topology-map">{"".join(nodes)}</div>' if nodes else "<p>没有可访问的 CPU 拓扑数据。</p>"


def _inventory_tables(system: dict[str, Any]) -> str:
    caches = system.get("caches", [])
    cache_rows = "".join(
        f"<tr><td>L{_h(c.get('level'))} {_h(CACHE_TYPE_ZH.get(c.get('type'), c.get('type')))}</td><td>{_h(_human_bytes(c.get('size_bytes')))}</td>"
        f"<td>{_h(c.get('line_bytes'))}</td><td>{_h(c.get('ways'))}</td><td>{_h(c.get('sets'))}</td>"
        f"<td class='mono'>{_h(','.join(map(str, c.get('shared_cpus', []))))}</td></tr>"
        for c in caches
    ) or '<tr><td colspan="6">没有可用的缓存清点数据</td></tr>'
    numa_rows = "".join(
        f"<tr><td>{_h(n.get('node'))}</td><td class='mono'>{_h(','.join(map(str, n.get('cpus', []))))}</td>"
        f"<td>{_h(_human_bytes(n.get('memory', {}).get('MemTotal')))}</td>"
        f"<td class='mono'>{_h(' '.join(map(str, n.get('distance', []))))}</td></tr>"
        for n in system.get("numa", [])
    ) or '<tr><td colspan="4">单 NUMA 节点，或 NUMA sysfs 信息不可用</td></tr>'
    return f"""
    <div class="split inventory-grid">
      <div><h3>缓存实例</h3><div class="table-scroll"><table class="data-table compact"><thead><tr>
        <th>层级/类型</th><th>容量</th><th>缓存行</th><th>路数</th><th>组数</th><th>共享 CPU</th>
      </tr></thead><tbody>{cache_rows}</tbody></table></div></div>
      <div><h3>NUMA 清点</h3><div class="table-scroll"><table class="data-table compact"><thead><tr>
        <th>节点</th><th>CPU</th><th>内存</th><th>距离表行</th>
      </tr></thead><tbody>{numa_rows}</tbody></table></div></div>
    </div>"""


def _raw_table(report: dict[str, Any]) -> str:
    rows = []
    for item in report.get("observations", []):
        labels = _label_text(item.get("labels", {}))
        search = " ".join([item.get("group", ""), item.get("metric", ""), labels, item.get("method", "")])
        group = item.get("group", "")
        metric = item.get("metric", "")
        confidence = item.get("confidence", "unknown")
        default_hidden = group == "core_latency"
        hidden_attributes = ' data-default-hidden="true" hidden' if default_hidden else ""
        row_attributes = (
            f'data-search="{_h(search.lower())}" data-group="{_h(group)}"'
            f'{hidden_attributes}'
        )
        rows.append(
            f'<tr {row_attributes}><td><span class="group-tag">{_h(GROUP_ZH.get(group, group))}</span>'
            f'<small class="identifier">{_h(group)}</small></td>'
            f'<td><b>{_h(METRIC_ZH.get(metric, metric))}</b><small class="identifier">{_h(metric)}</small>'
            f'<small>方法：{_h(_method_zh(item.get("method")))}</small></td>'
            f'<td class="numeric">{_h(_number(item.get("value"), item.get("unit", "")))}</td>'
            f'<td><i class="confidence {_h(confidence)}"></i>{_h(CONFIDENCE_ZH.get(confidence, confidence))}</td>'
            f'<td class="mono labels">{_h(labels)}</td></tr>'
        )
    return "".join(rows)


def _coverage(report: dict[str, Any]) -> str:
    groups = {item.get("group") for item in report.get("observations", [])}
    mapping = {
        "identity": True, "topology": True, "cache": bool(_obs(report, "cache_latency", "random_load_latency")),
        "latency": bool(groups & {"cache_latency", "tlb_latency", "page_policy", "loaded_memory_latency", "numa"}),
        "bandwidth": bool(groups & {"memory_bandwidth", "cache_bandwidth", "instruction_fetch", "numa"}),
        "memory": bool(groups & {"memory_access", "memory_parallelism", "store_forwarding"}),
        "coherence": bool(groups & {"core_latency", "coherence"}),
        "core": bool(groups & {"pipeline", "compute_scaling", "reorder_window"}),
        "branch": bool(groups & {"branch", "branch_structure"}),
        "os": True, "environment": True,
    }
    rows = []
    for item in report.get("metric_catalog", []):
        covered = mapping.get(item.get("category"), False)
        rows.append(
            f'<tr><td><i class="coverage {"yes" if covered else "no"}"></i></td>'
            f'<td>{_h(CATEGORY_ZH.get(item.get("category"), item.get("category")))}</td><td>{_h(item.get("metric"))}</td>'
            f'<td>{_h(KIND_ZH.get(item.get("kind"), item.get("kind")))}</td>'
            f'<td>{_h(CONFIDENCE_ZH.get(item.get("nominal_confidence"), item.get("nominal_confidence")))}</td></tr>'
        )
    return "".join(rows)


def render_report(report: dict[str, Any]) -> str:
    """Render report data as a single HTML file with no network dependencies."""
    system = report.get("system", {})
    cpu = system.get("cpu", {})
    topology = system.get("topology", {})
    estimates = _estimate_map(report)
    run = report.get("run", {})

    cache_curve = _line_chart(
        "内存层级 · 依赖加载延迟",
        _obs(report, "cache_latency", "random_load_latency"),
        lambda item: int(item["labels"]["working_set_bytes"]), lambda _: "随机加载",
        lambda x: _human_bytes(x), "ns/access",
        "工作集每次扩大 2 倍。曲线台阶通常对应缓存容量边界；结果同时包含地址转换开销。", True,
    )
    tlb_curve = _line_chart(
        "地址转换 · 每页一次随机访问",
        _obs(report, "tlb_latency", "page_random_load_latency"),
        lambda item: int(item["labels"]["pages"]), lambda _: "基础页",
        lambda x: f"{int(x)} 页", "ns/access",
        "每页只访问一条缓存行，用于突出 TLB 容量边界与页表遍历拐点。", True,
    )
    bandwidth_curve = _line_chart(
        "可持续内存有效载荷带宽",
        _obs(report, "memory_bandwidth", "stream_bandwidth"),
        lambda item: int(item["labels"]["threads"]), lambda item: OPERATION_ZH.get(item["labels"]["operation"], item["labels"]["operation"]),
        lambda x: str(int(x)), "GB/s",
        "线程固定在不同物理核心。复制/三元运算只统计有效载荷，不包含一致性与写分配流量。",
    )
    cache_bandwidth_curve = _line_chart(
        "单核带宽随工作集变化",
        _obs(report, "cache_bandwidth", "working_set_bandwidth"),
        lambda item: int(item["labels"]["working_set_bytes"]), lambda item: OPERATION_ZH.get(item["labels"]["operation"], item["labels"]["operation"]),
        lambda x: _human_bytes(x), "GB/s",
        "这是刻意包含缓存效应的重复读/复制测试：32 KiB 等小工作集命中 L1/L2，出现数百 GB/s 属于缓存带宽；工作集越过 L2、L3 并最终落到 DRAM 后，带宽会逐级下降。它不能单独作为内存总带宽。", True,
    )
    stride_curve = _line_chart(
        "访问步长与硬件预取敏感度",
        _obs(report, "memory_access", "stride_access_rate"),
        lambda item: int(item["labels"]["stride_bytes"]), lambda _: "固定步长",
        lambda x: f"{int(x)}B", "Gaccess/s",
        "只统计请求的 8 字节加载。更大步长会降低缓存行利用率，并逐步超出硬件预取器能力。", True,
    )
    mlp_curve = _line_chart(
        "内存级并行度（MLP）",
        _obs(report, "memory_parallelism", "random_load_rate"),
        lambda item: int(item["labels"]["chains"]), lambda _: "独立依赖链",
        lambda x: str(int(x)), "Gaccess/s",
        "多条相互独立、链内依赖的随机访问链，用于测量单核可维持的并发缺失数下界。", True,
    )
    page_policy_rows = [
        (_value_zh("policy", item["labels"].get("policy", "unknown")), item["value"], item["unit"])
        for item in _obs(report, "page_policy", "random_load_latency")
    ]
    page_policy_chart = _bar_chart(
        "基础页与透明大页策略对比", page_policy_rows,
        "madvise 只是建议；每个原始点的 anon_huge_bytes 标签给出该映射实际使用的匿名大页字节数。",
    )
    loaded_latency_curve = _line_chart(
        "带宽压力下的随机内存延迟",
        _obs(report, "loaded_memory_latency", "random_load_latency_under_load"),
        lambda item: float(item["labels"].get("measured_load_gbps", 0)),
        lambda _: "依赖加载延迟", lambda x: f"{x:.1f}", "ns/access",
        "横轴为并发读取线程实际产生的有效载荷 GB/s；零点是不施加带宽压力的基线。",
    )
    loaded_bandwidth_curve = _line_chart(
        "延迟探针并发负载强度",
        _obs(report, "loaded_memory_latency", "concurrent_read_bandwidth"),
        lambda item: int(item["labels"].get("load_threads", 0)), lambda _: "并发读取",
        lambda x: str(int(x)), "GB/s",
        "该曲线记录带载延迟实验每个压力线程数下真正达到的有效读取带宽。",
    )
    forwarding_rows = [
        (_value_zh("case", item["labels"].get("case", "unknown")), item["value"], item["unit"])
        for item in _obs(report, "store_forwarding", "store_load_latency")
    ]
    forwarding_chart = _bar_chart(
        "Store-to-load forwarding 对齐代价", forwarding_rows,
        "同址同宽是基线；部分覆盖、错位重叠和跨缓存行可暴露转发失败或重放路径。",
    )
    instruction_fetch_curve = _line_chart(
        "指令侧代码输送吞吐与足迹",
        _obs(report, "instruction_fetch", "code_delivery_bandwidth"),
        lambda item: int(item["labels"]["working_set_bytes"]),
        lambda item: f"{item['labels'].get('instruction_bytes')} 字节 NOP",
        lambda x: _human_bytes(x), "GB/s",
        "W^X 生成代码从 4 KiB 扩展到 MiB 级；曲线综合反映 L1I、iTLB、下级缓存和前端输送，不能直接等同于指令缓存容量。",
        True,
    )
    false_sharing_curve = _line_chart(
        "伪共享边界",
        _obs(report, "coherence", "atomic_update_rate"),
        lambda item: int(item["labels"]["separation_bytes"]), lambda _: "两个原子变量",
        lambda x: f"{int(x)}B", "Mop/s",
        "两个核心分别更新不同原子变量；吞吐恢复点通常对应缓存一致性行边界。", True,
    )
    rob_curve = _line_chart(
        "乱序窗口重叠探针",
        _obs(report, "reorder_window", "cold_load_overlap_penalty"),
        lambda item: int(item["labels"].get(
            "filler_instructions", item["labels"].get("estimated_uops", 0)
        )), lambda _: "冷加载 − 热加载",
        lambda x: str(int(x)), "counter-ticks",
        "x86/C86 使用精确静态展开的一微操作整数指令窗口；连续台阶表示第二次缓存缺失不再与第一次重叠。它仍是 ROB/调度资源的低置信代理量。",
    )

    pipeline_rows = []
    for item in _obs(report, "pipeline", "operations_per_cycle"):
        if item["metric"] == "operations_per_cycle":
            kernel = item["labels"].get("kernel", "unknown")
            pipeline_rows.append((KERNEL_ZH.get(kernel, kernel), item["value"], item["unit"]))
    pipeline_chart = _bar_chart(
        "标量微内核执行吞吐", pipeline_rows,
        "依赖链暴露延迟约束，独立链给出后端吞吐下界；结果不能直接等同于执行端口数量。",
    )
    branch_rows = [
        (PATTERN_ZH.get(item["labels"].get("pattern", "unknown"), item["labels"].get("pattern", "unknown")), item["value"], item["unit"])
        for item in _obs(report, "branch", "time_per_branch")
    ]
    branch_chart = _bar_chart(
        "不同分支模式的代价", branch_rows,
        "随机模式与可预测模式的差值近似误预测恢复代价；编译器被要求保留标量分支。",
    )
    core_rows = []
    relations: dict[str, list[float]] = defaultdict(list)
    for item in _obs(report, "core_latency", "cacheline_handoff_latency"):
        relations[item["labels"].get("relation", "unknown")].append(item["value"])
    for relation, values in sorted(relations.items()):
        core_rows.append((RELATION_ZH.get(relation, relation), sum(values) / len(values), "ns/one-way"))
    core_chart = _bar_chart(
        "核间缓存行传递延迟（按拓扑分类）", core_rows,
        "release/acquire 乒乓往返时间除以二。standard 覆盖全部物理核心对，deep 覆盖全部可见逻辑 CPU 对。",
    )
    compute_scaling_curve = _line_chart(
        "整数微内核的多核与 SMT 吞吐扩展",
        _obs(report, "compute_scaling", "integer_add_throughput"),
        lambda item: int(item["labels"].get("threads", 1)),
        lambda item: _value_zh("scope", item["labels"].get("scope", "unknown")),
        lambda x: str(int(x)), "Gop/s",
        "物理核心曲线用于观察全核扩展；SMT 点比较同一物理核心上一个与两个硬件线程的资源共享效果。",
    )
    btb_items = _obs(report, "branch_structure", "btb_branch_latency")
    btb_unconditional_curve = _line_chart(
        "BTB 无条件跳转足迹压力",
        [item for item in btb_items if item["labels"].get("branch_type") == "unconditional"],
        lambda item: int(item["labels"]["branch_count"]),
        lambda item: f"间距 {item['labels'].get('spacing_bytes')}B",
        lambda x: str(int(x)), "ns/branch",
        "无条件已跳转直接分支的数量与地址间距独立变化；耗时台阶只作为多级 BTB/前端结构容量代理。", True,
    )
    btb_conditional_curve = _line_chart(
        "BTB 始终跳转条件分支足迹压力",
        [item for item in btb_items if item["labels"].get("branch_type") == "conditional-taken"],
        lambda item: int(item["labels"]["branch_count"]),
        lambda item: f"间距 {item['labels'].get('spacing_bytes')}B",
        lambda x: str(int(x)), "ns/branch",
        "条件恒为真的直接分支使用相同数量和间距扫描，便于比较条件/无条件分支的 BTB 路径。", True,
    )
    history_curve = _line_chart(
        "方向预测器历史周期压力",
        _obs(report, "branch_structure", "history_period_latency"),
        lambda item: int(item["labels"]["history_period"]), lambda _: "重复伪随机周期",
        lambda x: str(int(x)), "ns/branch",
        "周期越长，预测器需要记住的方向序列越复杂；曲线受到分支地址、别名和预测算法共同影响。", True,
    )
    return_stack_curve = _line_chart(
        "返回地址栈（RAS）深度压力",
        _obs(report, "branch_structure", "return_stack_latency"),
        lambda item: int(item["labels"]["depth"]), lambda _: "唯一返回地址嵌套调用",
        lambda x: str(int(x)), "ns/return",
        "每一层调用拥有不同返回地址；拐点是 RAS/返回预测路径的低置信容量代理。", True,
    )
    indirect_curve = _line_chart(
        "间接分支目标数量压力",
        _obs(report, "branch_structure", "indirect_call_latency"),
        lambda item: int(item["labels"]["target_count"]), lambda _: "单一间接调用点",
        lambda x: str(int(x)), "ns/call",
        "一个间接调用点按打乱顺序切换目标；拐点综合反映间接目标预测器、BTB 和代码足迹。", True,
    )
    core_matrix = ""
    core_latency_items = _obs(report, "core_latency", "cacheline_handoff_latency")
    if core_latency_items:
        scoped_items = [
            item for item in core_latency_items
            if item.get("labels", {}).get("matrix_scope") in {"physical-core", "logical-cpu"}
        ]
        source_items = scoped_items or core_latency_items
        matrix_items = []
        for item in source_items:
            forward = dict(item)
            forward["labels"] = {**item["labels"], "row_cpu": item["labels"].get("cpu_a"),
                                  "column_cpu": item["labels"].get("cpu_b")}
            reverse = dict(item)
            reverse["labels"] = {**item["labels"], "row_cpu": item["labels"].get("cpu_b"),
                                  "column_cpu": item["labels"].get("cpu_a")}
            matrix_items.extend((forward, reverse))
        scopes = {item.get("labels", {}).get("matrix_scope") for item in source_items}
        if scopes == {"physical-core"}:
            matrix_note = f"standard 物理核心矩阵，共实测 {len(source_items)} 个无向核心对；单元格按对称方式展开，对角线不测量。"
        elif scopes == {"logical-cpu"}:
            matrix_note = f"deep 逻辑 CPU 矩阵，共实测 {len(source_items)} 个无向 CPU 对；单元格按对称方式展开，对角线不测量。"
        else:
            matrix_note = f"当前 profile 仅采集 {len(source_items)} 个代表性核心对，因此这里是稀疏矩阵；空白单元格表示未测量。"
        core_matrix = '<div class="wide">' + _heatmap(
            "核间延迟矩阵（CPU × CPU）", matrix_items, "row_cpu", "column_cpu", "ns/one-way",
            matrix_note, "CPU", "CPU", zoomable=True,
        ) + "</div>"
    os_rows = [
        (item["metric"], item["value"], item["unit"])
        for item in report.get("observations", [])
        if item.get("group") == "os_overhead" and item.get("metric") != "minor_faults"
    ]
    os_chart = _bar_chart(
        "操作系统边界开销", [(METRIC_ZH.get(name, name), value, unit) for name, value, unit in os_rows],
        "包括直接系统调用、匿名页首次触碰和固定线程间的管道交接；每个条形保留自己的单位。",
    )
    numa_latency = _heatmap(
        "NUMA 延迟矩阵", _obs(report, "numa", "load_latency"),
        "cpu_node", "memory_node", "ns/access", "行表示执行线程所在 NUMA 节点，列表示页面实际驻留的内存节点。",
        "计算节点", "内存节点",
    )
    numa_bandwidth = _heatmap(
        "NUMA 读取带宽矩阵", _obs(report, "numa", "read_bandwidth"),
        "cpu_node", "memory_node", "GB/s", "非对角单元格表示经 NUMA 互联传输的可持续有效读取载荷。",
        "计算节点", "内存节点",
    )

    warning_html = "".join(f"<li>{_h(warning)}</li>" for warning in report.get("warnings", []))
    if not warning_html:
        warning_html = "<li>本次运行没有探针警告。</li>"
    flags = cpu.get("flags", [])
    flag_html = "".join(f"<span>{_h(flag)}</span>" for flag in flags)
    dmi = system.get("dmi", {})
    machine_name = " · ".join(filter(None, [dmi.get("sys_vendor"), dmi.get("product_name")])) or system.get("hostname", "未知主机")
    generated = datetime.now().astimezone().isoformat(timespec="seconds")
    raw_json_size = len(json.dumps(report, ensure_ascii=False))
    raw_total_count = len(report.get("observations", []))
    core_latency_raw_count = sum(
        1 for item in report.get("observations", []) if item.get("group") == "core_latency"
    )
    raw_core_toggle = (
        f'<label class="raw-toggle"><input type="checkbox" id="show-core-latency"> '
        f'显示核间延迟明细（{core_latency_raw_count} 条）</label>'
        if core_latency_raw_count else ""
    )

    return f"""<!doctype html>
<html lang="zh-CN" data-theme="light">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>System Decap 平台表征报告 · {_h(system.get('hostname', '未知主机'))}</title>
<style>
:root{{--paper:#f4f0e8;--paper-2:#e9e2d5;--ink:#102a3a;--muted:#64727a;--line:#c8c1b4;--panel:#fbf9f4;--dark:#102a3a;--signal:#ff6b35;--teal:#007a80;--gold:#d9a62e;--shadow:0 18px 50px rgba(18,35,43,.10);--serif:"Bodoni 72","Noto Serif CJK SC","Songti SC",Georgia,serif;--sans:"Aptos","Noto Sans CJK SC","PingFang SC",sans-serif;--mono:"Berkeley Mono","SFMono-Regular","Cascadia Code",monospace}}
html[data-theme="dark"]{{--paper:#0b1720;--paper-2:#112632;--ink:#e9eee9;--muted:#9eafb5;--line:#294451;--panel:#10232e;--dark:#07131b;--shadow:0 18px 50px rgba(0,0,0,.28)}}
*{{box-sizing:border-box}}html{{scroll-behavior:smooth}}body{{margin:0;background:var(--paper);color:var(--ink);font-family:var(--sans);line-height:1.55}}
body:before{{content:"";position:fixed;inset:0;pointer-events:none;opacity:.24;background-image:linear-gradient(rgba(16,42,58,.055) 1px,transparent 1px),linear-gradient(90deg,rgba(16,42,58,.055) 1px,transparent 1px);background-size:26px 26px;mask-image:linear-gradient(to bottom,black,transparent 65%)}}
.shell{{width:min(1500px,calc(100% - 40px));margin:auto;position:relative}}.masthead{{padding:72px 0 40px;border-bottom:1px solid var(--line);display:grid;grid-template-columns:1.65fr .7fr;gap:48px}}
.kicker,.section-kicker{{font-family:var(--mono);font-size:11px;letter-spacing:.18em;text-transform:uppercase;color:var(--signal);font-weight:800}}h1{{font:700 clamp(54px,8vw,116px)/.82 var(--serif);letter-spacing:-.055em;margin:20px 0 26px;max-width:920px}}h1 em{{font-style:italic;color:var(--signal)}}
.lede{{font-size:18px;max-width:760px;color:var(--muted)}}.specimen{{background:var(--dark);color:#f6f1e7;padding:26px;border-radius:4px;box-shadow:var(--shadow);align-self:end;position:relative;overflow:hidden}}
.specimen:after{{content:"";position:absolute;width:170px;height:170px;border:34px solid var(--signal);border-radius:50%;right:-95px;top:-105px;opacity:.8}}.specimen dl{{margin:0;display:grid;grid-template-columns:1fr auto;gap:10px;border-top:1px solid #ffffff2b;padding-top:18px}}.specimen dt{{color:#9fb1ba}}.specimen dd{{margin:0;font-family:var(--mono);text-align:right}}
.nav{{position:sticky;top:0;z-index:20;background:color-mix(in srgb,var(--paper) 88%,transparent);backdrop-filter:blur(18px);border-bottom:1px solid var(--line)}}.nav-inner{{display:flex;gap:8px;align-items:center;overflow:auto;padding:11px 0}}.nav a{{color:var(--muted);text-decoration:none;font-size:12px;letter-spacing:.05em;text-transform:uppercase;padding:8px 11px;white-space:nowrap}}.nav a:hover{{color:var(--signal)}}.theme{{margin-left:auto;border:1px solid var(--line);background:var(--panel);color:var(--ink);border-radius:50%;width:34px;height:34px;cursor:pointer}}
section{{padding:72px 0;border-bottom:1px solid var(--line);scroll-margin-top:54px}}.section-head{{display:grid;grid-template-columns:.7fr 1.6fr;gap:30px;margin-bottom:34px}}h2{{font:700 clamp(34px,4.5vw,64px)/.94 var(--serif);letter-spacing:-.035em;margin:8px 0}}.section-intro{{color:var(--muted);font-size:17px;max-width:780px;align-self:end}}
.metrics{{display:grid;grid-template-columns:repeat(6,minmax(0,1fr));gap:1px;background:var(--line);border:1px solid var(--line);box-shadow:var(--shadow)}}.metric{{position:relative;background:var(--panel);min-width:0;min-height:150px;padding:20px;display:flex;flex-direction:column;justify-content:space-between}}.metric>span{{font:700 10px/1.3 var(--mono);letter-spacing:.1em;text-transform:uppercase;color:var(--muted)}}.metric>strong{{font:700 clamp(22px,2.3vw,34px)/1 var(--serif);letter-spacing:-.03em;overflow-wrap:anywhere}}.metric>small{{font:11px var(--mono);color:var(--muted)}}.metric.unavailable{{opacity:.6}}.metric-detail{{display:none;position:absolute;left:12px;right:12px;bottom:calc(100% - 8px);padding:13px;background:var(--dark);color:#eef4ef;z-index:3;font-size:11px;box-shadow:var(--shadow)}}.metric:hover .metric-detail{{display:block}}
.confidence{{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:6px;background:#8d979b}}.confidence.high{{background:#00a67d}}.confidence.medium{{background:var(--gold)}}.confidence.low{{background:var(--signal)}}.confidence.unavailable{{background:#8d979b}}
.chart-grid{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px;margin-top:22px}}.chart-grid>*{{min-width:0}}.chart-grid .wide{{grid-column:1/-1;min-width:0}}.chart{{background:var(--panel);border:1px solid var(--line);padding:20px;min-width:0;box-shadow:0 6px 20px rgba(18,35,43,.05)}}.chart-heading{{display:flex;align-items:center;justify-content:space-between;gap:20px;margin-bottom:8px}}.chart h3,.inventory-grid h3{{font:700 21px/1.1 var(--serif);margin:0}}.chart svg{{width:100%;height:auto;overflow:visible}}.plot-bg,.bar-track{{fill:color-mix(in srgb,var(--paper-2) 55%,transparent)}}.grid{{stroke:var(--line);stroke-width:1;stroke-dasharray:2 5}}.tick{{stroke:var(--muted)}}.axis,.axis-label,.bar-label,.bar-value{{fill:var(--muted);font:10px var(--mono)}}.axis-label{{font-weight:700;letter-spacing:.08em}}.series-line{{fill:none;stroke-width:2.5;stroke-linecap:round;stroke-linejoin:round}}.point{{stroke:var(--panel);stroke-width:2;cursor:crosshair}}.bar{{transform-origin:left;animation:grow .8s ease both}}.bar-value{{font-weight:700;fill:var(--ink)}}.legend{{display:flex;gap:12px;flex-wrap:wrap;font:10px var(--mono);color:var(--muted)}}.legend i{{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--swatch);margin-right:4px}}.chart-note{{margin:4px 0 0;color:var(--muted);font-size:12px;border-top:1px solid var(--line);padding-top:12px}}.empty-chart{{min-height:240px;text-align:center}}.empty-mark{{font:60px var(--serif);color:var(--line);margin-top:38px}}.range{{font:11px var(--mono);color:var(--muted)}}
.topology-map{{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px}}.node{{border:1px solid var(--line);background:var(--panel);padding:18px}}.node header{{display:grid;grid-template-columns:auto auto 1fr;align-items:baseline;gap:9px;border-bottom:1px solid var(--line);padding-bottom:12px}}.node header span{{font:10px var(--mono);color:var(--signal)}}.node header strong{{font:34px var(--serif)}}.node header small{{text-align:right;color:var(--muted)}}.cores{{display:grid;grid-template-columns:repeat(auto-fill,minmax(70px,1fr));gap:5px;padding-top:12px}}.core{{background:var(--paper-2);padding:7px 9px;display:flex;justify-content:space-between;align-items:center;border-left:2px solid var(--teal)}}.core b{{font:11px var(--mono)}}.core em{{font:9px var(--mono);color:var(--muted);font-style:normal}}
.heatmap{{border-collapse:separate;border-spacing:5px;width:max(100%,calc((var(--columns) + 1)*76px));min-width:430px}}.heatmap th{{font:10px var(--mono);color:var(--muted);padding:8px;white-space:nowrap}}.heatmap td{{background:var(--cell);min-width:70px;padding:18px 8px;text-align:center;color:#07131b;border-radius:3px}}.heatmap td strong{{display:block;font:700 19px var(--serif)}}.heatmap td small{{font:9px var(--mono)}}.heatmap td.missing{{background:var(--paper-2);color:var(--muted)}}
.matrix-toolbar{{display:flex;align-items:center;gap:8px;flex-wrap:wrap;padding:10px 0 12px;color:var(--muted);font:10px var(--mono)}}.matrix-toolbar button{{min-width:34px;height:34px;border:1px solid var(--line);border-radius:3px;background:var(--paper-2);color:var(--ink);font:700 15px var(--mono);cursor:pointer}}.matrix-toolbar button:hover,.matrix-toolbar button:focus-visible{{border-color:var(--signal);outline:none}}.matrix-toolbar .text-button{{width:auto;padding:0 11px;font-size:10px}}.matrix-toolbar input[type=range]{{width:min(220px,35vw);accent-color:var(--signal)}}.matrix-toolbar output{{width:42px;color:var(--ink);font-weight:700}}.matrix-toolbar span{{margin-left:auto}}.matrix-viewport{{position:relative;width:100%;height:min(72vh,900px);min-height:260px;overflow:auto;overscroll-behavior:contain;border:1px solid var(--line);background:var(--paper-2);cursor:grab;touch-action:none}}.matrix-viewport:focus-visible{{outline:3px solid color-mix(in srgb,var(--signal) 35%,transparent);outline-offset:2px}}.matrix-viewport.is-dragging{{cursor:grabbing;user-select:none}}.matrix-stage{{position:relative;min-width:1px;min-height:1px}}.zoomable-heatmap .heatmap{{position:absolute;left:0;top:0;transform-origin:top left;margin:0;width:calc((var(--columns) + 1)*76px)}}.zoomable-heatmap .heatmap thead th{{position:sticky;top:0;z-index:2;background:var(--panel)}}.zoomable-heatmap .heatmap tbody th{{position:sticky;left:0;z-index:1;background:var(--panel)}}.zoomable-heatmap .heatmap thead th:first-child{{left:0;z-index:3}}
.split{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:24px}}.table-scroll{{overflow:auto;max-width:100%}}table{{width:100%}}.data-table{{border-collapse:collapse;background:var(--panel);font-size:13px}}.data-table th{{text-align:left;font:700 10px var(--mono);letter-spacing:.07em;color:var(--muted);background:var(--paper-2);position:sticky;top:0}}.data-table th,.data-table td{{padding:12px;border-bottom:1px solid var(--line);vertical-align:top}}.data-table td small{{display:block;color:var(--muted);max-width:420px}}.data-table .numeric{{font:700 14px var(--mono);white-space:nowrap}}.compact th,.compact td{{padding:9px;font-size:11px}}.mono{{font-family:var(--mono);font-size:10px;word-break:break-all}}.labels{{max-width:460px}}.group-tag{{font:700 9px var(--mono);letter-spacing:.04em;background:var(--paper-2);padding:4px 6px}}.identifier{{font:9px var(--mono);color:var(--muted);margin-top:5px}}
.raw-tools{{display:grid;grid-template-columns:minmax(260px,1fr) auto auto;align-items:center;gap:12px;margin-bottom:12px}}.filter{{width:100%;border:1px solid var(--line);background:var(--panel);color:var(--ink);font:14px var(--mono);padding:14px 16px;outline:none}}.filter:focus{{border-color:var(--signal);box-shadow:0 0 0 3px color-mix(in srgb,var(--signal) 18%,transparent)}}.raw-toggle{{display:flex;align-items:center;gap:7px;white-space:nowrap;color:var(--ink);font:12px var(--mono);cursor:pointer}}.raw-toggle input{{width:16px;height:16px;accent-color:var(--signal)}}.raw-status{{white-space:nowrap;color:var(--muted);font:10px var(--mono)}}
.warning-box{{background:var(--dark);color:#ecf2ed;padding:25px;border-left:7px solid var(--signal)}}.warning-box h3{{font:700 24px var(--serif);margin:0 0 10px}}.warning-box li{{margin:8px 0;color:#bccbd0}}.flags{{display:flex;flex-wrap:wrap;gap:5px}}.flags span{{font:9px var(--mono);padding:5px 7px;border:1px solid var(--line);background:var(--panel)}}details{{border:1px solid var(--line);background:var(--panel);padding:16px;margin-top:14px}}summary{{cursor:pointer;font-weight:700}}pre{{white-space:pre-wrap;word-break:break-word;font:10px/1.6 var(--mono);color:var(--muted);max-height:460px;overflow:auto}}
.coverage{{width:10px;height:10px;border-radius:50%;display:block}}.coverage.yes{{background:#00a67d;box-shadow:0 0 0 3px #00a67d22}}.coverage.no{{background:#8d979b}}footer{{padding:42px 0 70px;display:flex;justify-content:space-between;color:var(--muted);font:11px var(--mono)}}
.reveal{{animation:reveal .65s ease both}}@keyframes reveal{{from{{opacity:0;transform:translateY(12px)}}to{{opacity:1;transform:none}}}}@keyframes grow{{from{{transform:scaleX(0)}}to{{transform:scaleX(1)}}}}
@media(max-width:1100px){{.metrics{{grid-template-columns:repeat(3,minmax(0,1fr))}}.masthead{{grid-template-columns:1fr}}}}@media(max-width:760px){{.shell{{width:min(100% - 22px,1500px)}}.masthead{{padding-top:44px}}.section-head,.split,.chart-grid{{grid-template-columns:1fr}}.metrics{{grid-template-columns:repeat(2,minmax(0,1fr))}}.chart-grid .wide{{grid-column:auto}}.matrix-toolbar span{{width:100%;margin-left:0}}.matrix-viewport{{height:65vh;min-height:240px}}.raw-tools{{grid-template-columns:1fr;align-items:start}}section{{padding:50px 0}}footer{{display:block}}}}
@media print{{.nav,.theme{{display:none}}body{{background:white}}.chart,.metric,.node{{break-inside:avoid;box-shadow:none}}section{{padding:30px 0}}}}
</style>
</head>
<body>
<header class="shell masthead">
  <div><div class="kicker">黑盒平台表征 · 实验报告 01</div>
    <h1>System <em>Decap</em></h1>
    <p class="lede">在没有数据手册的条件下，通过软件可见行为重建平台轮廓。每个数值均保留测量方法、作用范围、置信度和边界条件。</p>
  </div>
  <aside class="specimen"><div class="kicker">样本 / 目标机</div><h3>{_h(cpu.get('model', '未知 CPU'))}</h3>
    <dl><dt>主机</dt><dd>{_h(system.get('hostname'))}</dd><dt>平台族</dt><dd>{_h(system.get('platform_family'))}</dd>
      <dt>内核</dt><dd>{_h(system.get('kernel'))}</dd><dt>测试档位</dt><dd>{_h(run.get('profile'))}</dd>
      <dt>运行时间</dt><dd>{_h(run.get('started_at', '')[:19])}</dd></dl></aside>
</header>
<nav class="nav"><div class="shell nav-inner"><a href="#overview">总览</a><a href="#topology">拓扑</a><a href="#memory">内存层级</a><a href="#numa">NUMA</a><a href="#core">核心</a><a href="#inventory">硬件清点</a><a href="#raw">原始数据</a><a href="#method">方法与限制</a><button class="theme" id="theme" title="切换明暗主题" aria-label="切换明暗主题">◐</button></div></nav>
<main class="shell">
<section id="overview"><div class="section-head"><div><div class="section-kicker">01 / 核心摘要</div><h2>平台<br>指纹</h2></div><p class="section-intro">这些核心指标可用于快速比较机器；悬停卡片可查看推断依据。高置信度代表直接清点或稳定的硬件计数，中、低置信度结果则属于黑盒下界、代理量或受运行环境限制的观测。</p></div>
  <div class="metrics">
    {_metric_card(estimates.get('memory.aggregate_read_bandwidth'), '系统聚合读取带宽')}
    {_metric_card(estimates.get('memory.single_core_read_bandwidth'), '单核读取带宽')}
    {_metric_card(estimates.get('memory.random_latency'), 'DRAM 随机延迟')}
    {_metric_card(estimates.get('core.max_observed_ipc'), '最大实测 IPC')}
    {_metric_card(estimates.get('core.frontend_width_lower_bound'), '前端宽度下界')}
    {_metric_card(estimates.get('core.rob_capacity_proxy'), 'ROB 窗口代理值')}
    {_metric_card(estimates.get('numa.same_socket_remote_latency') or estimates.get('numa.remote_latency'), '同插槽跨 NUMA 延迟')}
    {_metric_card(estimates.get('numa.same_socket_remote_payload_bandwidth') or estimates.get('numa.interconnect_payload_bandwidth'), '同插槽跨 NUMA 带宽')}
    {_metric_card(estimates.get('numa.cross_socket_latency'), '跨插槽延迟')}
    {_metric_card(estimates.get('numa.cross_socket_payload_bandwidth'), '跨插槽有效载荷带宽')}
    {_metric_card(estimates.get('core.integer_add_lanes_lower_bound'), '整数加法吞吐')}
    {_metric_card(estimates.get('coherence.same-llc-different-core') or estimates.get('coherence.same-numa-different-core'), '共享 LLC 核间传递')}
    {_metric_card(estimates.get('tlb.first_knee'), '首个 TLB 拐点')}
    {_metric_card(estimates.get('branch.unpredictable_penalty'), '随机分支代价')}
  </div>
</section>
<section id="topology"><div class="section-head"><div><div class="section-kicker">02 / 放置地图</div><h2>处理器<br>拓扑</h2></div><p class="section-intro">{_h(machine_name)} · {topology.get('sockets', 0)} 个插槽、{topology.get('dies', 0)} 个 die、{topology.get('numa_nodes', 0)} 个 NUMA 节点、{topology.get('physical_cores', 0)} 个当前可访问物理核心、{topology.get('logical_cpus', 0)} 个逻辑 CPU。</p></div>
  {_topology_visual(system)}
</section>
<section id="memory"><div class="section-head"><div><div class="section-kicker">03 / 存储层级</div><h2>缓存与<br>内存系统</h2></div><p class="section-intro">依赖加载用于暴露延迟层级，并行数据流用于测量从单核到整个可访问系统的可持续有效载荷。每个原始数据点均保留工作集、线程数和亲和性信息。</p></div>
  <div class="chart-grid"><div class="wide">{cache_curve}</div>{tlb_curve}{false_sharing_curve}<div class="wide">{cache_bandwidth_curve}</div>{stride_curve}{mlp_curve}{page_policy_chart}{forwarding_chart}{loaded_latency_curve}{loaded_bandwidth_curve}<div class="wide">{instruction_fetch_curve}</div><div class="wide">{bandwidth_curve}</div></div>
</section>
<section id="numa"><div class="section-head"><div><div class="section-kicker">04 / 片间互联</div><h2>NUMA 与<br>互联</h2></div><p class="section-intro">页面被绑定到各内存节点；若绑定权限不足，则由固定线程首次触碰完成放置。随后从每个 CPU 节点访问这些页面。虚拟化、容器或 mbind 权限不足会降低内存放置置信度。</p></div>
  <div class="chart-grid">{numa_latency}{numa_bandwidth}</div>
</section>
<section id="core"><div class="section-head"><div><div class="section-kicker">05 / 微架构</div><h2>核心<br>执行引擎</h2></div><p class="section-intro">架构专用标量汇编分别测试前端流量、整数与 FP64 依赖延迟、独立链吞吐及多核/SMT 扩展。生成式分支序列提供 BTB、历史、返回地址栈和间接目标的压力曲线；所有结构容量只作为经验代理。</p></div>
  <div class="chart-grid">{pipeline_chart}{branch_chart}<div class="wide">{compute_scaling_curve}</div>{btb_unconditional_curve}{btb_conditional_curve}{history_curve}{return_stack_curve}{indirect_curve}{core_chart}{rob_curve}{core_matrix}</div>
</section>
<section id="inventory"><div class="section-head"><div><div class="section-kicker">06 / 静态证据</div><h2>硬件<br>清点</h2></div><p class="section-intro">sysfs 与 procfs 是操作系统可见的平台证据。容量与拓扑通常较可靠，但固件缺陷、cgroup 亲和性限制和虚拟化仍可能改变可见范围。</p></div>
  {_inventory_tables(system)}
  <details><summary>ISA 特性标志 · {len(flags)} 项</summary><div class="flags">{flag_html}</div></details>
  <details><summary>DMI / 固件身份信息</summary><pre>{_h(json.dumps(dmi, ensure_ascii=False, indent=2))}</pre></details>
  <details><summary>内核漏洞缓解状态</summary><pre>{_h(json.dumps(system.get('environment', {}).get('vulnerabilities', {}), ensure_ascii=False, indent=2))}</pre></details>
</section>
<section id="raw"><div class="section-head"><div><div class="section-kicker">07 / 证据台账</div><h2>原始<br>观测数据</h2></div><p class="section-intro">本次运行包含 {raw_total_count} 个原始观测点。核间延迟明细因数量较多而默认隐藏；完整 JSON 与 CSV 文件仍保留所有数据。</p></div>
  <div class="raw-tools"><input class="filter" id="filter" placeholder="筛选：cache_latency、operation=read、cpu_node=1 …" aria-label="筛选原始观测数据">{raw_core_toggle}<output class="raw-status" id="raw-status" aria-live="polite">当前显示 {raw_total_count - core_latency_raw_count} / 总计 {raw_total_count} 条</output></div>
  <div class="table-scroll"><table class="data-table" id="raw-table"><thead><tr><th>分组</th><th>指标 / 方法</th><th>数值</th><th>置信度</th><th>标签</th></tr></thead><tbody>{_raw_table(report)}</tbody></table></div>
</section>
<section id="method"><div class="section-head"><div><div class="section-kicker">08 / 结论边界</div><h2>方法与<br>限制</h2></div><p class="section-intro">黑盒表征并非真正对芯片开盖，它回答的是当前软件环境能够观测到什么。频率、固件、内核策略、虚拟化、温度、页面迁移和编译器行为均属于实验条件。</p></div>
  <div class="warning-box"><h3>运行警告与结果限定</h3><ul>{warning_html}</ul></div>
  <div class="chart-grid"><div class="wide">{os_chart}</div></div>
  <details open><summary>指标覆盖台账</summary><div class="table-scroll"><table class="data-table compact"><thead><tr><th>状态</th><th>类别</th><th>指标</th><th>类型</th><th>标称置信度</th></tr></thead><tbody>{_coverage(report)}</tbody></table></div></details>
  <details><summary>复现命令</summary><pre>{_h(' '.join(map(str, run.get('command', []))))}</pre></details>
  <details><summary>运行元数据</summary><pre>{_h(json.dumps({'run': run, 'tool': report.get('tool'), 'native': report.get('native_metadata')}, ensure_ascii=False, indent=2))}</pre></details>
</section>
</main>
<footer class="shell"><span>System Decap v{_h(report.get('tool', {}).get('version', '?'))} · 生成时间 {generated}</span><span>{len(report.get('observations', []))} 个观测 · {len(report.get('estimates', []))} 个推断 · 源数据 {_human_bytes(raw_json_size)}</span></footer>
<script>
const root=document.documentElement,button=document.getElementById('theme');
button.addEventListener('click',()=>{{root.dataset.theme=root.dataset.theme==='dark'?'light':'dark';}});
document.querySelectorAll('[data-matrix-viewer]').forEach(viewport=>{{
  const stage=viewport.querySelector('.matrix-stage'),table=stage.querySelector('.heatmap');
  const toolbar=viewport.previousElementSibling,range=toolbar.querySelector('[data-zoom-range]');
  const output=toolbar.querySelector('[data-zoom-output]');
  const minimum=Number(range.min)/100,maximum=Number(range.max)/100;
  const naturalWidth=table.offsetWidth,naturalHeight=table.offsetHeight;
  let scale=1,dragging=false,fitted=true,startX=0,startY=0,startLeft=0,startTop=0;
  const clamp=value=>Math.min(maximum,Math.max(minimum,value));
  const fitScale=()=>clamp(Math.min(1,viewport.clientWidth/naturalWidth,viewport.clientHeight/naturalHeight));
  const renderScale=(next,anchor)=>{{
    const previous=scale;scale=clamp(next);
    table.style.transform=`scale(${{scale}})`;
    stage.style.width=`${{naturalWidth*scale}}px`;
    stage.style.height=`${{naturalHeight*scale}}px`;
    const percent=scale*100;
    range.value=String(Math.round(percent));output.value=`${{percent<10?percent.toFixed(1):Math.round(percent)}}%`;
    if(anchor){{
      const localX=anchor.clientX-anchor.rect.left,localY=anchor.clientY-anchor.rect.top;
      const contentX=(viewport.scrollLeft+localX)/previous;
      const contentY=(viewport.scrollTop+localY)/previous;
      viewport.scrollLeft=contentX*scale-localX;viewport.scrollTop=contentY*scale-localY;
    }}
  }};
  toolbar.addEventListener('click',event=>{{
    const action=event.target.closest('[data-zoom-action]')?.dataset.zoomAction;
    if(!action)return;
    if(action==='in'){{fitted=false;renderScale(Math.max(scale*1.2,scale+0.01));}}
    if(action==='out'){{fitted=false;renderScale(Math.min(scale/1.2,scale-0.01));}}
    if(action==='actual'){{fitted=false;renderScale(1);}}
    if(action==='fit'){{fitted=true;viewport.scrollTo(0,0);renderScale(fitScale());}}
  }});
  range.addEventListener('input',()=>{{fitted=false;renderScale(Number(range.value)/100);}});
  viewport.addEventListener('wheel',event=>{{
    if(!(event.ctrlKey||event.metaKey))return;
    event.preventDefault();fitted=false;
    renderScale(scale*(event.deltaY<0?1.12:1/1.12),{{clientX:event.clientX,clientY:event.clientY,rect:viewport.getBoundingClientRect()}});
  }},{{passive:false}});
  viewport.addEventListener('pointerdown',event=>{{
    if(event.button!==0)return;dragging=true;startX=event.clientX;startY=event.clientY;
    startLeft=viewport.scrollLeft;startTop=viewport.scrollTop;viewport.classList.add('is-dragging');
    viewport.setPointerCapture(event.pointerId);
  }});
  viewport.addEventListener('pointermove',event=>{{
    if(!dragging)return;viewport.scrollLeft=startLeft-(event.clientX-startX);viewport.scrollTop=startTop-(event.clientY-startY);
  }});
  const stopDragging=event=>{{
    if(!dragging)return;dragging=false;viewport.classList.remove('is-dragging');
    if(viewport.hasPointerCapture(event.pointerId))viewport.releasePointerCapture(event.pointerId);
  }};
  viewport.addEventListener('pointerup',stopDragging);viewport.addEventListener('pointercancel',stopDragging);
  renderScale(fitScale());
  if('ResizeObserver' in window)new ResizeObserver(()=>{{if(fitted)renderScale(fitScale());}}).observe(viewport);
}});
const filter=document.getElementById('filter'),showCoreLatency=document.getElementById('show-core-latency');
const rawRows=[...document.querySelectorAll('#raw-table tbody tr')],rawStatus=document.getElementById('raw-status');
const filterRawRows=()=>{{
  const query=filter.value.toLowerCase().trim();let visible=0;
  rawRows.forEach(row=>{{
    const defaultHidden=row.dataset.defaultHidden==='true'&&!showCoreLatency?.checked;
    const searchHidden=Boolean(query)&&!row.dataset.search.includes(query);
    row.hidden=defaultHidden||searchHidden;if(!row.hidden)visible+=1;
  }});
  rawStatus.value=`当前显示 ${{visible}} / 总计 ${{rawRows.length}} 条`;
}};
filter.addEventListener('input',filterRawRows);showCoreLatency?.addEventListener('change',filterRawRows);filterRawRows();
</script>
</body></html>"""
