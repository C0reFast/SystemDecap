"""Turn raw observations into carefully qualified black-box estimates."""

from __future__ import annotations

import math
import statistics
from collections import defaultdict
from typing import Any, Iterable


def _matching(observations: list[dict[str, Any]], group: str, metric: str) -> list[dict[str, Any]]:
    return [item for item in observations if item["group"] == group and item["metric"] == metric]


def _estimate(
    key: str,
    name: str,
    value: float | int | str | None,
    unit: str,
    confidence: str,
    basis: str,
    caveat: str = "",
    category: str = "summary",
) -> dict[str, Any]:
    return {
        "key": key,
        "name": name,
        "value": value,
        "unit": unit,
        "confidence": confidence,
        "basis": basis,
        "caveat": caveat,
        "category": category,
        "available": value is not None,
    }


def _median(values: Iterable[float]) -> float | None:
    materialized = list(values)
    return statistics.median(materialized) if materialized else None


def _bandwidth_quality(item: dict[str, Any] | None) -> str:
    if not item:
        return "unavailable"
    if item.get("labels", {}).get("working_set_exceeds_llc") != "true":
        return "low"
    return "high" if item.get("confidence", "high") == "high" else "medium"


def _bandwidth_basis(item: dict[str, Any] | None, prefix: str) -> str:
    if not item:
        return "未测量"
    coverage = item.get("labels", {}).get("working_set_exceeds_llc")
    if coverage == "true":
        return f"{prefix}；工作集已达到整机/读取节点 LLC 的 4 倍"
    if coverage == "false":
        return f"{prefix}；工作集未越过整机 LLC，结果可能主要来自缓存"
    return f"{prefix}；缺少 LLC 覆盖证据"


def _dram_bandwidth_points(items: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    """Return only points whose working set was verified to exceed aggregate LLC."""
    result = []
    for item in items:
        labels = item.get("labels", {})
        if labels.get("working_set_exceeds_llc") != "true":
            continue
        working_set = labels.get("working_set_bytes")
        llc = labels.get("aggregate_llc_bytes", labels.get("reader_node_llc_bytes"))
        if working_set is not None and llc is not None:
            try:
                if int(working_set) < int(llc) * 4:
                    continue
            except (TypeError, ValueError):
                continue
        result.append(item)
    return result


def _memory_bandwidth_upper_bound(system: dict[str, Any]) -> float | None:
    value = system.get("memory_bandwidth_theoretical", {}).get("upper_bound_gbps")
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) and parsed > 0 else None


def _primary_cache(system: dict[str, Any]) -> dict[int, dict[str, Any]]:
    allowed = system.get("topology", {}).get("allowed_cpu_list", [0])
    primary = allowed[0] if allowed else 0
    result: dict[int, dict[str, Any]] = {}
    for cache in system.get("caches", []):
        if primary not in cache.get("shared_cpus", []):
            continue
        if cache.get("type") not in ("Data", "Unified"):
            continue
        level = int(cache.get("level", 0))
        if level and (level not in result or cache.get("size_bytes", 0) > result[level].get("size_bytes", 0)):
            result[level] = cache
    return result


def _nearest_curve_value(curve: list[dict[str, Any]], bytes_target: int) -> float | None:
    candidates = [
        item for item in curve
        if int(item.get("labels", {}).get("working_set_bytes", 0)) <= bytes_target
    ]
    if not candidates:
        return None
    return max(candidates, key=lambda x: int(x["labels"]["working_set_bytes"]))["value"]


def _knees(curve: list[dict[str, Any]], x_label: str, ratio: float = 1.45) -> list[dict[str, float]]:
    ordered = sorted(curve, key=lambda x: int(x.get("labels", {}).get(x_label, 0)))
    result = []
    previous = None
    for item in ordered:
        value = float(item["value"])
        x = int(item.get("labels", {}).get(x_label, 0))
        if previous is not None and value > previous * ratio:
            result.append({"x": x, "before": previous, "after": value, "ratio": value / previous})
        previous = value
    return result


def _sustained_knees(
    curve: list[dict[str, Any]], x_label: str, ratio: float = 1.35, sustain: int = 2
) -> list[dict[str, float]]:
    """Return cliffs that stay elevated, rejecting isolated conflict/noise spikes."""
    ordered = sorted(curve, key=lambda item: int(item.get("labels", {}).get(x_label, 0)))
    result: list[dict[str, float]] = []
    for index in range(1, len(ordered)):
        before = float(ordered[index - 1]["value"])
        after = float(ordered[index]["value"])
        followers = ordered[index + 1:index + 1 + sustain]
        if (
            before > 0
            and after > before * ratio
            and len(followers) == sustain
            and all(float(item["value"]) > before * ratio for item in followers)
        ):
            result.append({
                "x": int(ordered[index].get("labels", {}).get(x_label, 0)),
                "before": before,
                "after": after,
                "ratio": after / before,
            })
    return result


def _pressure_knee(
    curve: list[dict[str, Any]], x_label: str, ratio: float = 1.30
) -> dict[str, float] | None:
    ordered = sorted(curve, key=lambda item: int(item.get("labels", {}).get(x_label, 0)))
    if len(ordered) < 3:
        return None
    baseline_items = ordered[: min(3, len(ordered))]
    baseline = statistics.median(float(item["value"]) for item in baseline_items)
    for item in ordered[len(baseline_items):]:
        value = float(item["value"])
        if baseline > 0 and value > baseline * ratio:
            return {
                "x": int(item.get("labels", {}).get(x_label, 0)),
                "baseline": baseline,
                "value": value,
                "ratio": value / baseline,
            }
    return None


def infer(system: dict[str, Any], native: dict[str, Any]) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    observations = native.get("observations", [])
    native_metadata = native.get("metadata", {})
    pmu_core_error = native_metadata.get("pmu_core_error") or native_metadata.get("perf_error")
    pmu_unavailable_basis = (
        f"核心 PMU 组运行失败：{pmu_core_error}" if pmu_core_error else "核心 PMU 计数不可用"
    )
    pmu_environment = "，".join(
        f"{name}={native_metadata[name]}"
        for name in ("perf_event_paranoid", "nmi_watchdog")
        if str(native_metadata.get(name, "")) != ""
    )
    estimates: list[dict[str, Any]] = []
    diagnostics: dict[str, Any] = {
        "cache_knees": [], "tlb_knees": [], "branch_structure_knees": {}
    }

    topology = system.get("topology", {})
    for key, name, unit in [
        ("logical_cpus", "可用逻辑 CPU", "CPUs"),
        ("physical_cores", "可用物理核心", "cores"),
        ("sockets", "CPU 插槽", "sockets"),
        ("numa_nodes", "可用 NUMA 节点", "nodes"),
    ]:
        estimates.append(_estimate(f"topology.{key}", name, topology.get(key), unit, "high", "Linux sysfs 拓扑信息", category="topology"))

    primary_caches = _primary_cache(system)
    latency_curve = _matching(observations, "cache_latency", "random_load_latency")
    for level in sorted(primary_caches):
        cache = primary_caches[level]
        size = cache.get("size_bytes", 0)
        estimates.append(_estimate(
            f"cache.l{level}.capacity", f"L{level} 容量（主核可见实例）", size, "bytes", "high",
            "sysfs 缓存索引", category="cache",
        ))
        estimates.append(_estimate(
            f"cache.l{level}.line_size", f"L{level} coherence line", cache.get("line_bytes"), "bytes", "high",
            "sysfs coherency_line_size 字段", category="cache",
        ))
        estimates.append(_estimate(
            f"cache.l{level}.associativity", f"L{level} 组相联路数", cache.get("ways"), "ways", "high",
            "sysfs ways_of_associativity 字段", category="cache",
        ))
        # Half-capacity avoids measuring the cliff at a full, set-constrained cache.
        latency = _nearest_curve_value(latency_curve, max(4096, size // 2))
        estimates.append(_estimate(
            f"cache.l{level}.latency", f"L{level} 随机依赖加载延迟", latency, "ns/access",
            "medium" if latency is not None else "unavailable",
            "不超过标称缓存容量一半的最大指针追逐工作集",
            "缓存共享、TLB 和替换策略可能移动实测拐点。", "latency",
        ))
    diagnostics["cache_knees"] = _knees(latency_curve, "working_set_bytes")
    if latency_curve:
        largest = max(latency_curve, key=lambda x: int(x["labels"]["working_set_bytes"]))
        largest_bytes = int(largest["labels"]["working_set_bytes"])
        llc_size = max((cache.get("size_bytes", 0) for cache in primary_caches.values()), default=0)
        outside_llc = largest_bytes >= llc_size * 2 if llc_size else True
        estimates.append(_estimate(
            "memory.random_latency", "主核 DRAM 随机加载延迟", largest["value"], "ns/access",
            "medium" if outside_llc else "low", f"最大指针追逐工作集：{largest_bytes} 字节",
            "工作集可能没有完全超出有效 LLC。" if not outside_llc else "结果包含 TLB/页表遍历影响。",
            "latency",
        ))
        cache_bw = [
            item for item in _matching(observations, "cache_bandwidth", "working_set_bandwidth")
            if item.get("labels", {}).get("operation") == "read"
        ]
        representative_bw = _nearest_curve_value(cache_bw, max(32768, size // 2))
        estimates.append(_estimate(
            f"cache.l{level}.read_bandwidth", f"L{level} 单核重复读取吞吐", representative_bw, "GB/s",
            "medium" if representative_bw is not None else "unavailable",
            "不超过缓存容量一半的单核重复读取流",
            "该软件可见结果包含编译器向量化和循环开销。", "cache",
        ))

    tlb_curve = _matching(observations, "tlb_latency", "page_random_load_latency")
    diagnostics["tlb_knees"] = _sustained_knees(tlb_curve, "pages", 1.35)
    if diagnostics["tlb_knees"]:
        first = diagnostics["tlb_knees"][0]
        estimates.append(_estimate(
            "tlb.first_knee", "首个 TLB 容量拐点", first["x"], "base pages", "medium",
            f"逐页单次访问扫描中的延迟跳升为 {first['ratio']:.2f}×",
            "这是容量拐点，不能直接视为某一级 TLB 的精确条目数。", "tlb",
        ))

    page_policy = _matching(observations, "page_policy", "random_load_latency")
    page_by_name = {item["labels"].get("policy"): item for item in page_policy}
    base_page = page_by_name.get("base-page-advised")
    thp_page = page_by_name.get("thp-advised")
    page_policy_quality = (
        "low" if base_page and thp_page
        and "low" in {base_page.get("confidence"), thp_page.get("confidence")}
        else "medium" if base_page and thp_page else "unavailable"
    )
    estimates.append(_estimate(
        "memory.thp_latency_ratio", "THP 建议相对基础页建议的随机延迟",
        thp_page["value"] / base_page["value"] if thp_page and base_page and base_page["value"] else None,
        "×", page_policy_quality,
        "同一工作集 MADV_HUGEPAGE 延迟 / MADV_NOHUGEPAGE 延迟",
        "madvise 只是策略建议；报告同时保留 AnonHugePages 实际驻留字节数。", "memory",
    ))

    loaded_latency = sorted(
        _matching(observations, "loaded_memory_latency", "random_load_latency_under_load"),
        key=lambda item: int(item["labels"].get("load_threads", 0)),
    )
    unloaded = next((item for item in loaded_latency if item["labels"].get("load_threads") == "0"), None)
    worst_loaded = max(loaded_latency, key=lambda item: item["value"], default=None)
    estimates.append(_estimate(
        "memory.loaded_latency_slowdown", "带宽压力下的最大内存延迟放大",
        worst_loaded["value"] / unloaded["value"] if worst_loaded and unloaded and unloaded["value"] else None,
        "×", "medium" if worst_loaded and unloaded else "unavailable",
        (f"{worst_loaded['labels'].get('load_threads')} 个加载线程下的最大值 / 无加载线程基线"
         if worst_loaded and unloaded else "未测量"),
        "并发线程同时消耗内存控制器、缓存和片上互联资源。", "memory",
    ))

    forwarding = _matching(observations, "store_forwarding", "store_load_latency")
    forwarding_by_case = {item["labels"].get("case"): item for item in forwarding}
    exact_forward = forwarding_by_case.get("exact-8-to-8")
    for case, name in [
        ("partial-4-to-8", "部分覆盖"),
        ("overlap-offset-1", "错位重叠"),
        ("split-cache-line", "跨缓存行"),
    ]:
        item = forwarding_by_case.get(case)
        estimates.append(_estimate(
            f"memory.store_forwarding_penalty.{case}", f"Store forwarding {name}额外代价",
            item["value"] - exact_forward["value"] if item and exact_forward else None,
            "ns/pair", "medium" if item and exact_forward else "unavailable",
            "对应 store/load 对耗时减去同地址 8→8 基线",
            "包含循环、地址生成和平台计时噪声；用于相对比较。", "memory",
        ))

    instruction_fetch = [
        item for item in _matching(observations, "instruction_fetch", "code_delivery_bandwidth")
        if item["labels"].get("instruction_bytes") == "4"
    ]
    instruction_fetch_knee = None
    if instruction_fetch:
        ordered_fetch = sorted(
            instruction_fetch, key=lambda item: int(item["labels"].get("working_set_bytes", 0))
        )
        peak_prefix = max(float(item["value"]) for item in ordered_fetch[: min(3, len(ordered_fetch))])
        instruction_fetch_knee = next((
            int(item["labels"]["working_set_bytes"]) for item in ordered_fetch[3:]
            if float(item["value"]) < peak_prefix * 0.70
        ), None)
    estimates.append(_estimate(
        "core.instruction_footprint_knee", "指令输送吞吐首个明显下降足迹",
        instruction_fetch_knee, "bytes", "low" if instruction_fetch_knee else "unavailable",
        "4 字节 NOP 编码吞吐首次低于小工作集峰值 70% 的代码足迹",
        "这是 L1I/iTLB/更低层取指路径的综合拐点，不是精确 L1I 容量。", "core",
    ))

    bandwidth = _matching(observations, "memory_bandwidth", "stream_bandwidth")
    raw_read_bandwidth = [
        item for item in bandwidth if item["labels"].get("operation") == "read"
    ]
    llc_qualified_read = _dram_bandwidth_points(raw_read_bandwidth)
    theoretical = system.get("memory_bandwidth_theoretical", {})
    theoretical_limit = _memory_bandwidth_upper_bound(system)
    limit_with_tolerance = theoretical_limit * 1.05 if theoretical_limit else None
    above_theoretical = [
        item for item in llc_qualified_read
        if limit_with_tolerance is not None and float(item["value"]) > limit_with_tolerance
    ]
    read_bandwidth = [item for item in llc_qualified_read if item not in above_theoretical]
    diagnostics["memory_bandwidth"] = {
        "theoretical_upper_bound_gbps": theoretical_limit,
        "validation_tolerance_percent": 5.0 if theoretical_limit else None,
        "above_theoretical_limit": len(above_theoretical),
        "cache_polluted_or_unverified": len(raw_read_bandwidth) - len(llc_qualified_read),
    }
    estimates.append(_estimate(
        "memory.theoretical_peak_bandwidth", "内存配置理论峰值带宽",
        theoretical_limit, "GB/s", "medium" if theoretical_limit else "unavailable",
        (theoretical.get("source", "内存配置清点") if theoretical_limit else
         "SMBIOS 内存设备位宽/速率记录缺失或不完整；可同时传 --memory-channels 与 --memory-mtps"),
        "这是配置清点得到的保守上界，不是可持续性能目标；2DPC 时可能高估。",
        "bandwidth",
    ))
    one_core = [item for item in read_bandwidth if item["labels"].get("threads") == "1"]
    one_core_best = max(one_core, key=lambda item: item["value"], default=None)
    max_read = max(read_bandwidth, key=lambda x: x["value"], default=None)
    if above_theoretical and not read_bandwidth:
        no_dram_basis = "合格工作集的观测值全部超过内存配置理论上界，已判为无效"
    else:
        no_dram_basis = "没有工作集明确越过整机 LLC 的合格观测点；缓存污染点仅保留在原始曲线中"
    limit_caveat = (
        f"已排除 {len(above_theoretical)} 个超出配置理论上界 5% 容差的观测点。"
        if above_theoretical else ""
    )
    estimates.append(_estimate(
        "memory.single_core_read_bandwidth", "单核内存读带宽",
        one_core_best["value"] if one_core_best else None, "GB/s",
        _bandwidth_quality(one_core_best),
        (_bandwidth_basis(one_core_best, "固定在一个物理核心上的流式有效载荷")
         if one_core_best else no_dram_basis),
        limit_caveat, "bandwidth",
    ))
    estimates.append(_estimate(
        "memory.aggregate_read_bandwidth", "系统聚合内存读带宽",
        max_read["value"] if max_read else None, "GB/s",
        _bandwidth_quality(max_read),
        (_bandwidth_basis(
            max_read,
            f"最佳实测点使用 {max_read['labels'].get('threads')} 个线程" if max_read else "未测量",
        ) if max_read else no_dram_basis),
        "只统计有效载荷带宽，不包含写分配和协议流量。" + limit_caveat, "bandwidth",
    ))
    if max_read and one_core:
        max_threads = int(max_read["labels"].get("threads", 1))
        single_value = max(item["value"] for item in one_core)
        estimates.append(_estimate(
            "memory.read_scaling_efficiency", "内存读带宽并行效率",
            max_read["value"] / max(single_value * max_threads, 1e-12) * 100.0,
            "%", "medium", "最佳点聚合带宽 /（单核带宽 × 线程数）",
            "单核工作集较小或频率策略不同时，结果可能超过 100%。", "bandwidth",
        ))
        ordered_read = sorted(read_bandwidth, key=lambda item: int(item["labels"].get("threads", 1)))
        saturation = next(
            (int(item["labels"]["threads"]) for item in ordered_read if item["value"] >= max_read["value"] * 0.95),
            max_threads,
        )
        estimates.append(_estimate(
            "memory.read_saturation_threads", "读带宽达到 95% 峰值的线程数", saturation, "threads",
            "medium", "首个达到最佳带宽 95% 的线程扩展点", category="bandwidth",
        ))
    operation_names = {"write": "写入", "copy": "复制", "triad": "三元运算"}
    for operation in ("write", "copy", "triad"):
        items = _dram_bandwidth_points(
            item for item in bandwidth if item["labels"].get("operation") == operation
        )
        best = max(items, key=lambda x: x["value"], default=None)
        single = [item for item in items if item["labels"].get("threads") == "1"]
        single_best = max(single, key=lambda item: item["value"], default=None)
        estimates.append(_estimate(
            f"memory.single_core_{operation}_bandwidth", f"单核{operation_names[operation]}带宽",
            single_best["value"] if single_best else None, "GB/s",
            _bandwidth_quality(single_best),
            _bandwidth_basis(single_best, "固定在一个物理核心上的流式有效载荷"),
            category="bandwidth",
        ))
        estimates.append(_estimate(
            f"memory.aggregate_{operation}_bandwidth", f"系统聚合{operation_names[operation]}带宽",
            best["value"] if best else None, "GB/s", _bandwidth_quality(best),
            _bandwidth_basis(
                best,
                f"最佳实测有效载荷点使用 {best['labels'].get('threads')} 个线程"
                if best else "未测量",
            ),
            category="bandwidth",
        ))

    numa_latency = _matching(observations, "numa", "load_latency")
    local_lat = [item["value"] for item in numa_latency if item["labels"].get("local") == "true"]
    remote_lat = [item["value"] for item in numa_latency if item["labels"].get("local") == "false"]
    local_quality = "high" if local_lat and all(
        item.get("confidence") == "high" for item in numa_latency if item["labels"].get("local") == "true"
    ) else "medium" if local_lat else "unavailable"
    remote_quality = "high" if remote_lat and all(
        item.get("confidence") == "high" for item in numa_latency if item["labels"].get("local") == "false"
    ) else "medium" if remote_lat else "unavailable"
    estimates.append(_estimate(
        "numa.local_latency", "NUMA 本地随机延迟", _median(local_lat), "ns/access",
        local_quality, "CPU 节点 × 内存节点矩阵对角线的中位数", category="numa",
    ))
    estimates.append(_estimate(
        "numa.remote_latency", "NUMA 跨节点随机延迟", _median(remote_lat), "ns/access",
        remote_quality, "CPU 节点 × 内存节点矩阵非对角线的中位数", category="numa",
    ))
    if local_lat and remote_lat:
        estimates.append(_estimate(
            "numa.remote_latency_penalty", "跨 NUMA 延迟倍率", statistics.median(remote_lat) / statistics.median(local_lat),
            "×", "high" if local_quality == remote_quality == "high" else "medium",
            "远端延迟中位数 / 本地延迟中位数", category="numa",
        ))
    raw_numa_bw = _matching(observations, "numa", "read_bandwidth")
    numa_llc_qualified = _dram_bandwidth_points(raw_numa_bw)
    numa_bw = [
        item for item in numa_llc_qualified
        if limit_with_tolerance is None or float(item["value"]) <= limit_with_tolerance
    ]
    diagnostics["numa_bandwidth"] = {
        "raw_points": len(raw_numa_bw),
        "qualified_points": len(numa_bw),
        "rejected_points": len(raw_numa_bw) - len(numa_bw),
    }
    local_bw = [item["value"] for item in numa_bw if item["labels"].get("local") == "true"]
    remote_bw = [item["value"] for item in numa_bw if item["labels"].get("local") == "false"]
    local_bw_items = [item for item in numa_bw if item["labels"].get("local") == "true"]
    remote_bw_items = [item for item in numa_bw if item["labels"].get("local") == "false"]
    estimates.append(_estimate(
        "numa.local_payload_bandwidth", "NUMA 本地有效读带宽", _median(local_bw), "GB/s",
        "high" if local_bw_items and all(item.get("confidence") == "high"
                                          for item in local_bw_items)
        else "medium" if local_bw_items else "unavailable",
        "NUMA 读取有效载荷矩阵中通过 4× LLC 与配置上界校验的对角线中位数",
        category="numa",
    ))
    estimates.append(_estimate(
        "numa.interconnect_payload_bandwidth", "跨 NUMA 混合路径有效读带宽",
        _median(remote_bw), "GB/s",
        "medium" if remote_bw_items else "unavailable",
        "所有通过 4× LLC 与配置上界校验的非本地路径有效载荷中位数",
        "同插槽跨 NPS 与跨插槽路径可能同时存在；应优先查看拆分后的路径指标。", "numa",
    ))
    for relation, key_prefix, name in (
        ("cross-numa-same-socket", "same_socket_remote", "同插槽跨 NUMA"),
        ("cross-socket", "cross_socket", "跨插槽"),
    ):
        relation_latencies = [
            item for item in numa_latency
            if item.get("labels", {}).get("relation") == relation
        ]
        relation_bandwidths = [
            item for item in numa_bw
            if item.get("labels", {}).get("relation") == relation
        ]
        latency_quality = (
            "high" if relation_latencies and all(
                item.get("confidence") == "high" for item in relation_latencies
            ) else "medium" if relation_latencies else "unavailable"
        )
        bandwidth_quality = (
            "high" if relation_bandwidths and all(
                item.get("confidence") == "high"
                for item in relation_bandwidths
            ) else "medium" if relation_bandwidths else "unavailable"
        )
        estimates.append(_estimate(
            f"numa.{key_prefix}_latency", f"{name}随机延迟",
            _median(item["value"] for item in relation_latencies), "ns/access",
            latency_quality, f"{name}路径中位数", category="numa",
        ))
        estimates.append(_estimate(
            f"numa.{key_prefix}_payload_bandwidth", f"{name}有效读带宽",
            _median(item["value"] for item in relation_bandwidths), "GB/s",
            bandwidth_quality, f"{name}路径有效载荷中位数",
            "仅保留工作集达到读取节点 LLC 4 倍且未超过内存配置理论上界的点。", "numa",
        ))

    core_latencies = _matching(observations, "core_latency", "cacheline_handoff_latency")
    by_relation: dict[str, list[float]] = defaultdict(list)
    relation_names = {
        "smt-sibling": "SMT 同核线程",
        "same-llc-different-core": "共享 LLC 的不同核心",
        "cross-llc-same-numa": "同 NUMA 跨 LLC",
        "same-numa-different-core": "同 NUMA 不同核心",
        "cross-numa-same-socket": "同插槽跨 NUMA",
        "cross-socket": "跨插槽",
    }
    for item in core_latencies:
        by_relation[item["labels"].get("relation", "unknown")].append(item["value"])
    for relation, values in sorted(by_relation.items()):
        estimates.append(_estimate(
            f"coherence.{relation}", f"核间缓存行传递：{relation_names.get(relation, relation)}", statistics.median(values),
            "ns/one-way", "high", f"{len(values)} 个乒乓核心对的中位数", category="coherence",
        ))

    false_sharing = _matching(observations, "coherence", "atomic_update_rate")
    if false_sharing:
        maximum = max(item["value"] for item in false_sharing)
        recovered = [
            item for item in false_sharing
            if item["value"] >= maximum * 0.75
        ]
        line_knee = min((int(item["labels"]["separation_bytes"]) for item in recovered), default=None)
        estimates.append(_estimate(
            "coherence.empirical_line_knee", "False-sharing 消失间距", line_knee, "bytes", "medium",
            "首个达到最大独立原子更新速率 75% 的变量间距",
            "若 sysfs coherency_line_size 可用，应以其作为权威值。", "coherence",
        ))

    pipeline_ipc = _matching(observations, "pipeline", "ipc")
    best_ipc = max(pipeline_ipc, key=lambda x: x["value"], default=None)
    estimates.append(_estimate(
        "core.max_observed_ipc", "最大实测退休 IPC", best_ipc["value"] if best_ipc else None,
        "instructions/cycle", "high" if best_ipc else "unavailable",
        f"最佳微内核：{best_ipc['labels'].get('kernel')}" if best_ipc else pmu_unavailable_basis,
        ("这是通用退休能力的下界，不是架构理论最大值。" if best_ipc else pmu_environment), "core",
    ))
    nop_ipc = [item for item in pipeline_ipc if item["labels"].get("kernel") == "nop_frontend"]
    frontend_lower = math.floor(max((item["value"] for item in nop_ipc), default=0)) or None
    estimates.append(_estimate(
        "core.frontend_width_lower_bound", "前端/退休宽度下界", frontend_lower, "instructions/cycle",
        "low" if frontend_lower else "unavailable",
        "NOP 流退休 IPC 向下取整" if frontend_lower else pmu_unavailable_basis,
        ("µop 缓存、宏融合、NOP 特殊处理和退休宽度可能比译码宽度更早成为瓶颈。"
         if frontend_lower else pmu_environment), "core",
    ))
    add_parallel = [
        item for item in _matching(observations, "pipeline", "operations_per_cycle")
        if item["labels"].get("kernel", "").startswith("integer_add_parallel")
    ]
    best_add = max(add_parallel, key=lambda x: x["value"], default=None)
    estimates.append(_estimate(
        "core.integer_add_lanes_lower_bound", "整数加法后端吞吐下界",
        best_add["value"] if best_add else None, "adds/cycle", "low" if best_add else "unavailable",
        f"最佳独立链微内核：{best_add['labels'].get('kernel')}" if best_add else pmu_unavailable_basis,
        "近似可用执行吞吐，不能直接视为执行端口数量。" if best_add else pmu_environment, "core",
    ))
    dep_add = [
        item for item in _matching(observations, "pipeline", "cycles_per_operation")
        if item["labels"].get("kernel") == "integer_add_dependency"
    ]
    estimates.append(_estimate(
        "core.integer_add_latency", "整数加法依赖延迟", dep_add[0]["value"] if dep_add else None,
        "cycles/op", "medium" if dep_add else "unavailable", "单条依赖链上的标量加法", category="core",
    ))
    dep_mul = [
        item for item in _matching(observations, "pipeline", "cycles_per_operation")
        if item["labels"].get("kernel") == "integer_mul_dependency"
    ]
    estimates.append(_estimate(
        "core.integer_multiply_latency", "整数乘法依赖延迟", dep_mul[0]["value"] if dep_mul else None,
        "cycles/op", "medium" if dep_mul else "unavailable", "单条依赖链上的标量乘法", category="core",
    ))
    mul_parallel = [
        item for item in _matching(observations, "pipeline", "operations_per_cycle")
        if item["labels"].get("kernel") == "integer_mul_parallel4"
    ]
    estimates.append(_estimate(
        "core.integer_multiply_throughput", "整数乘法后端吞吐",
        mul_parallel[0]["value"] if mul_parallel else None, "multiplies/cycle",
        "medium" if mul_parallel else "unavailable", "四条独立标量乘法链", category="core",
    ))
    fp_specs = [
        ("fp64_add_dependency", "core.fp64_add_latency", "FP64 加法依赖延迟", "cycles/op"),
        ("fp64_add_parallel4", "core.fp64_add_throughput", "FP64 加法并行吞吐", "operations/cycle"),
        ("fp64_mul_dependency", "core.fp64_multiply_latency", "FP64 乘法依赖延迟", "cycles/op"),
        ("fp64_mul_parallel4", "core.fp64_multiply_throughput", "FP64 乘法并行吞吐", "operations/cycle"),
    ]
    for kernel, key, name, unit in fp_specs:
        metric = "cycles_per_operation" if kernel.endswith("dependency") else "operations_per_cycle"
        items = [
            item for item in _matching(observations, "pipeline", metric)
            if item["labels"].get("kernel") == kernel
        ]
        estimates.append(_estimate(
            key, name, items[0]["value"] if items else None, unit,
            "medium" if items else "unavailable", f"标量微内核 {kernel}",
            "这是标量 FP64 路径的软件可见值，不代表 SIMD/矩阵单元峰值。", "core",
        ))
    frequencies = _matching(observations, "pipeline", "effective_core_frequency")
    estimates.append(_estimate(
        "core.effective_frequency", "微基准期间有效核心频率",
        _median(item["value"] for item in frequencies), "GHz", "medium" if frequencies else "unavailable",
        "各标量微内核的 perf 核心周期 / 墙钟时间中位数", category="core",
    ))

    compute_scaling = [
        item for item in _matching(observations, "compute_scaling", "integer_add_throughput")
        if item["labels"].get("scope") == "physical-cores"
    ]
    peak_compute = max(compute_scaling, key=lambda item: item["value"], default=None)
    one_compute = next((item for item in compute_scaling if item["labels"].get("threads") == "1"), None)
    estimates.append(_estimate(
        "core.aggregate_integer_add_throughput", "全核整数加法实测峰值",
        peak_compute["value"] if peak_compute else None, "Gop/s",
        "medium" if peak_compute else "unavailable",
        (f"固定 {peak_compute['labels'].get('threads')} 个物理核心的独立加法链"
         if peak_compute else "未测量"),
        "用于频率和扩展性比较，不是所有整数指令的通用峰值。", "core",
    ))
    if peak_compute and one_compute:
        threads = int(peak_compute["labels"].get("threads", 1))
        estimates.append(_estimate(
            "core.compute_scaling_efficiency", "全核整数吞吐扩展效率",
            peak_compute["value"] / max(one_compute["value"] * threads, 1e-12) * 100.0,
            "%", "medium", "峰值吞吐 /（单核吞吐 × 峰值线程数）",
            "受全核频率、热功耗限制和后台负载影响。", "core",
        ))
    smt_compute = next((
        item for item in _matching(observations, "compute_scaling", "integer_add_throughput")
        if item["labels"].get("scope") == "smt-siblings"
    ), None)
    estimates.append(_estimate(
        "core.smt_integer_add_gain", "同一物理核启用两个 SMT 线程的吞吐增益",
        smt_compute["value"] / one_compute["value"] if smt_compute and one_compute else None,
        "×", "medium" if smt_compute and one_compute else "unavailable",
        "SMT sibling 双线程吞吐 / 单物理核心单线程吞吐", category="core",
    ))

    rob = _matching(observations, "reorder_window", "rob_capacity_proxy")
    architecture = native_metadata.get("architecture", "")
    estimates.append(_estimate(
        "core.rob_capacity_proxy", "ROB/乱序窗口容量估计", rob[0]["value"] if rob else None,
        ("条静态指令" if rob and rob[0].get("unit") == "static-instructions"
         else rob[0].get("unit", "条静态指令") if rob else "条静态指令"),
        "low" if rob else "unavailable",
        (rob[0]["method"] if rob else
         "ARM64 暂无可移植的精确静态窗口探针" if architecture == "arm64" else
         "精确静态窗口曲线未找到连续保持的拐点"),
        ("填充区每条指令均为一个简单整数 µop；结果仍可能先受 scheduler、load queue 或物理寄存器限制。"
         if rob else "未输出不稳定的单点阈值；请查看原始重叠曲线和运行警告。"), "core",
    ))

    branch_time = _matching(observations, "branch", "time_per_branch")
    branch_by_pattern = {item["labels"].get("pattern"): item["value"] for item in branch_time}
    if "random" in branch_by_pattern and "always-taken" in branch_by_pattern:
        estimates.append(_estimate(
            "branch.unpredictable_penalty", "随机分支额外代价",
            branch_by_pattern["random"] - branch_by_pattern["always-taken"], "ns/branch", "medium",
            "随机模式单分支耗时减去始终跳转模式耗时", category="branch",
        ))
    branch_misses = {
        item["labels"].get("pattern"): item["value"]
        for item in _matching(observations, "branch", "miss_rate")
    }
    estimates.append(_estimate(
        "branch.random_miss_rate", "随机分支误预测率", branch_misses.get("random"), "%",
        "high" if "random" in branch_misses else "unavailable", "perf 分支未命中数 / 分支指令数", category="branch",
    ))

    branch_proxy_specs = [
        (
            "btb", "btb_branch_latency", "branch_count", "branch.btb_footprint_knee",
            "BTB 直接跳转足迹拐点", "entries", 1.25,
            lambda item: item["labels"].get("spacing_bytes") == "16"
            and item["labels"].get("branch_type") == "unconditional",
        ),
        (
            "history", "history_period_latency", "history_period", "branch.history_period_knee",
            "方向预测历史周期拐点", "branches", 1.30, lambda item: True,
        ),
        (
            "return_stack", "return_stack_latency", "depth", "branch.return_stack_depth_proxy",
            "返回地址栈深度拐点", "nested calls", 1.20, lambda item: True,
        ),
        (
            "indirect", "indirect_call_latency", "target_count", "branch.indirect_target_knee",
            "间接分支目标数量拐点", "targets", 1.30, lambda item: True,
        ),
    ]
    for diagnostic_key, metric, x_label, key, name, unit, ratio, predicate in branch_proxy_specs:
        curve = [
            item for item in _matching(observations, "branch_structure", metric)
            if predicate(item)
        ]
        knee = _pressure_knee(curve, x_label, ratio)
        diagnostics["branch_structure_knees"][diagnostic_key] = knee
        estimates.append(_estimate(
            key, name, knee["x"] if knee else None, unit,
            "low" if knee else "unavailable",
            (f"压力曲线相对前三个小足迹点中位数上升 {knee['ratio']:.2f}×"
             if knee else "未找到满足阈值的稳定拐点"),
            "该数值是多级预测器、代码布局、计时和通用 perf 事件共同作用的经验代理，不是硬件条目数真值。",
            "branch",
        ))

    mlp_rates = _matching(observations, "memory_parallelism", "random_load_rate")
    one_chain_rate = next((item["value"] for item in mlp_rates if item["labels"].get("chains") == "1"), None)
    best_mlp = max(mlp_rates, key=lambda item: item["value"], default=None)
    estimates.append(_estimate(
        "memory.max_observed_mlp_speedup", "随机加载并行度加速",
        best_mlp["value"] / one_chain_rate if best_mlp and one_chain_rate else None, "×",
        "medium" if best_mlp and one_chain_rate else "unavailable",
        f"最佳独立链数量：{best_mlp['labels'].get('chains')}" if best_mlp else "未测量",
        "这是软件可见的 MLP 下界；总工作集保持近似不变。", "memory",
    ))
    estimates.append(_estimate(
        "memory.mlp_saturation_chains", "最佳随机加载独立链数",
        int(best_mlp["labels"]["chains"]) if best_mlp else None, "chains",
        "medium" if best_mlp else "unavailable", "随机加载速率达到实测最大值时的独立链数量", category="memory",
    ))

    stride_rates = _matching(observations, "memory_access", "stride_access_rate")
    if stride_rates:
        base_rate = min(stride_rates, key=lambda item: int(item["labels"]["stride_bytes"]))["value"]
        knee = next(
            (int(item["labels"]["stride_bytes"]) for item in sorted(stride_rates, key=lambda x: int(x["labels"]["stride_bytes"]))
             if item["value"] < base_rate * 0.5),
            None,
        )
        estimates.append(_estimate(
            "memory.prefetch_stride_knee", "顺序预取吞吐减半步长", knee, "bytes", "low" if knee else "unavailable",
            "首个低于 8 字节步长访问速率 50% 的步长",
            "该结果综合反映缓存行利用率、向量化和硬件预取行为。", "memory",
        ))

    overhead_map = {
        (item["metric"]): item for item in observations if item["group"] == "os_overhead"
    }
    for metric, name in [
        ("getpid_syscall", "getpid 系统调用开销"),
        ("anonymous_page_first_touch", "匿名页首次触碰开销"),
        ("scheduler_pipe_handoff", "线程调度/pipe 单向交接"),
    ]:
        item = overhead_map.get(metric)
        estimates.append(_estimate(
            f"os.{metric}", name, item["value"] if item else None, item["unit"] if item else "",
            item["confidence"] if item else "unavailable", item["method"] if item else "未测量", category="os",
        ))

    return estimates, diagnostics
