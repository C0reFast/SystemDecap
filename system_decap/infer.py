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


def infer(system: dict[str, Any], native: dict[str, Any]) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    observations = native.get("observations", [])
    estimates: list[dict[str, Any]] = []
    diagnostics: dict[str, Any] = {"cache_knees": [], "tlb_knees": []}

    topology = system.get("topology", {})
    for key, name, unit in [
        ("logical_cpus", "可用逻辑 CPU", "CPUs"),
        ("physical_cores", "可用物理核心", "cores"),
        ("sockets", "CPU 插槽", "sockets"),
        ("numa_nodes", "可用 NUMA 节点", "nodes"),
    ]:
        estimates.append(_estimate(f"topology.{key}", name, topology.get(key), unit, "high", "Linux sysfs topology", category="topology"))

    primary_caches = _primary_cache(system)
    latency_curve = _matching(observations, "cache_latency", "random_load_latency")
    for level in sorted(primary_caches):
        cache = primary_caches[level]
        size = cache.get("size_bytes", 0)
        estimates.append(_estimate(
            f"cache.l{level}.capacity", f"L{level} 容量（主核可见实例）", size, "bytes", "high",
            "sysfs cache index", category="cache",
        ))
        estimates.append(_estimate(
            f"cache.l{level}.line_size", f"L{level} coherence line", cache.get("line_bytes"), "bytes", "high",
            "sysfs coherency_line_size", category="cache",
        ))
        estimates.append(_estimate(
            f"cache.l{level}.associativity", f"L{level} 组相联路数", cache.get("ways"), "ways", "high",
            "sysfs ways_of_associativity", category="cache",
        ))
        # Half-capacity avoids measuring the cliff at a full, set-constrained cache.
        latency = _nearest_curve_value(latency_curve, max(4096, size // 2))
        estimates.append(_estimate(
            f"cache.l{level}.latency", f"L{level} 随机依赖加载延迟", latency, "ns/access",
            "medium" if latency is not None else "unavailable",
            "largest pointer-chase working set not exceeding half reported cache capacity",
            "Cache sharing, TLB and replacement policy can move the observed knee.", "latency",
        ))
    diagnostics["cache_knees"] = _knees(latency_curve, "working_set_bytes")
    if latency_curve:
        largest = max(latency_curve, key=lambda x: int(x["labels"]["working_set_bytes"]))
        largest_bytes = int(largest["labels"]["working_set_bytes"])
        llc_size = max((cache.get("size_bytes", 0) for cache in primary_caches.values()), default=0)
        outside_llc = largest_bytes >= llc_size * 2 if llc_size else True
        estimates.append(_estimate(
            "memory.random_latency", "主核 DRAM 随机加载延迟", largest["value"], "ns/access",
            "medium" if outside_llc else "low", f"largest pointer-chase set: {largest_bytes} bytes",
            "Working set may not fully exceed the effective LLC." if not outside_llc else "Includes TLB/page-walk effects.",
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
            "one-core read stream at no more than half cache capacity",
            "Compiler vectorization and loop overhead are part of this software-visible value.", "cache",
        ))

    tlb_curve = _matching(observations, "tlb_latency", "page_random_load_latency")
    diagnostics["tlb_knees"] = _knees(tlb_curve, "pages", 1.35)
    if diagnostics["tlb_knees"]:
        first = diagnostics["tlb_knees"][0]
        estimates.append(_estimate(
            "tlb.first_knee", "首个 TLB 容量拐点", first["x"], "base pages", "medium",
            f"latency jump {first['ratio']:.2f}× in one-access-per-page sweep",
            "This is a capacity knee, not a direct count of one specific TLB level.", "tlb",
        ))

    bandwidth = _matching(observations, "memory_bandwidth", "stream_bandwidth")
    read_bandwidth = [item for item in bandwidth if item["labels"].get("operation") == "read"]
    one_core = [item for item in read_bandwidth if item["labels"].get("threads") == "1"]
    max_read = max(read_bandwidth, key=lambda x: x["value"], default=None)
    estimates.append(_estimate(
        "memory.single_core_read_bandwidth", "单核内存读带宽",
        max((item["value"] for item in one_core), default=None), "GB/s",
        "high" if one_core else "unavailable", "one pinned physical core, streaming payload", category="bandwidth",
    ))
    estimates.append(_estimate(
        "memory.aggregate_read_bandwidth", "系统聚合内存读带宽",
        max_read["value"] if max_read else None, "GB/s", "high" if max_read else "unavailable",
        f"best measured point at {max_read['labels'].get('threads')} threads" if max_read else "not measured",
        "Payload bandwidth; write-allocate and protocol traffic are not counted.", "bandwidth",
    ))
    if max_read and one_core:
        max_threads = int(max_read["labels"].get("threads", 1))
        single_value = max(item["value"] for item in one_core)
        estimates.append(_estimate(
            "memory.read_scaling_efficiency", "内存读带宽并行效率",
            max_read["value"] / max(single_value * max_threads, 1e-12) * 100.0,
            "%", "medium", "aggregate / (single-core × threads) at best point",
            "Values above 100% can occur when a small one-core working set or frequency policy differs.", "bandwidth",
        ))
        ordered_read = sorted(read_bandwidth, key=lambda item: int(item["labels"].get("threads", 1)))
        saturation = next(
            (int(item["labels"]["threads"]) for item in ordered_read if item["value"] >= max_read["value"] * 0.95),
            max_threads,
        )
        estimates.append(_estimate(
            "memory.read_saturation_threads", "读带宽达到 95% 峰值的线程数", saturation, "threads",
            "medium", "first measured scaling point at or above 95% of best bandwidth", category="bandwidth",
        ))
    for operation in ("write", "copy", "triad"):
        items = [item for item in bandwidth if item["labels"].get("operation") == operation]
        best = max(items, key=lambda x: x["value"], default=None)
        single = [item for item in items if item["labels"].get("threads") == "1"]
        estimates.append(_estimate(
            f"memory.single_core_{operation}_bandwidth", f"单核 {operation} 带宽",
            max((item["value"] for item in single), default=None), "GB/s",
            "high" if single else "unavailable", "one pinned physical core, streaming payload", category="bandwidth",
        ))
        estimates.append(_estimate(
            f"memory.aggregate_{operation}_bandwidth", f"系统聚合 {operation} 带宽",
            best["value"] if best else None, "GB/s", "high" if best else "unavailable",
            f"best measured payload point at {best['labels'].get('threads')} threads" if best else "not measured",
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
        local_quality, "median diagonal of CPU-node × memory-node matrix", category="numa",
    ))
    estimates.append(_estimate(
        "numa.remote_latency", "NUMA 跨节点随机延迟", _median(remote_lat), "ns/access",
        remote_quality, "median off-diagonal of CPU-node × memory-node matrix", category="numa",
    ))
    if local_lat and remote_lat:
        estimates.append(_estimate(
            "numa.remote_latency_penalty", "跨 NUMA 延迟倍率", statistics.median(remote_lat) / statistics.median(local_lat),
            "×", "high" if local_quality == remote_quality == "high" else "medium",
            "median remote latency / median local latency", category="numa",
        ))
    numa_bw = _matching(observations, "numa", "read_bandwidth")
    local_bw = [item["value"] for item in numa_bw if item["labels"].get("local") == "true"]
    remote_bw = [item["value"] for item in numa_bw if item["labels"].get("local") == "false"]
    estimates.append(_estimate(
        "numa.local_payload_bandwidth", "NUMA 本地有效读带宽", _median(local_bw), "GB/s",
        "high" if local_bw and all(
            item.get("confidence") == "high" for item in numa_bw if item["labels"].get("local") == "true"
        ) else "medium" if local_bw else "unavailable",
        "median diagonal NUMA read payload", category="numa",
    ))
    estimates.append(_estimate(
        "numa.interconnect_payload_bandwidth", "跨 NUMA 互联有效读带宽",
        _median(remote_bw), "GB/s", "medium" if remote_bw else "unavailable",
        "median remote-node memory payload bandwidth",
        "This is sustainable payload through the interconnect, not raw link signaling rate.", "numa",
    ))

    core_latencies = _matching(observations, "core_latency", "cacheline_handoff_latency")
    by_relation: dict[str, list[float]] = defaultdict(list)
    for item in core_latencies:
        by_relation[item["labels"].get("relation", "unknown")].append(item["value"])
    for relation, values in sorted(by_relation.items()):
        estimates.append(_estimate(
            f"coherence.{relation}", f"核间 cache-line 传递：{relation}", statistics.median(values),
            "ns/one-way", "high", f"median of {len(values)} ping-pong pair(s)", category="coherence",
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
            "first separation reaching 75% of maximum independent-atomic rate",
            "Use sysfs coherency_line_size as the authoritative value when available.", "coherence",
        ))

    pipeline_ipc = _matching(observations, "pipeline", "ipc")
    best_ipc = max(pipeline_ipc, key=lambda x: x["value"], default=None)
    estimates.append(_estimate(
        "core.max_observed_ipc", "最大实测退休 IPC", best_ipc["value"] if best_ipc else None,
        "instructions/cycle", "high" if best_ipc else "unavailable",
        f"best microkernel: {best_ipc['labels'].get('kernel')}" if best_ipc else "perf unavailable",
        "A lower bound for general retire capability, not an architectural maximum.", "core",
    ))
    nop_ipc = [item for item in pipeline_ipc if item["labels"].get("kernel") == "nop_frontend"]
    frontend_lower = math.floor(max((item["value"] for item in nop_ipc), default=0)) or None
    estimates.append(_estimate(
        "core.frontend_width_lower_bound", "前端/退休宽度下界", frontend_lower, "instructions/cycle",
        "low" if frontend_lower else "unavailable", "floor of NOP-stream retired IPC",
        "µop cache, macro-fusion, NOP special handling and retire width may dominate decode width.", "core",
    ))
    add_parallel = [
        item for item in _matching(observations, "pipeline", "operations_per_cycle")
        if item["labels"].get("kernel", "").startswith("integer_add_parallel")
    ]
    best_add = max(add_parallel, key=lambda x: x["value"], default=None)
    estimates.append(_estimate(
        "core.integer_add_lanes_lower_bound", "整数加法后端吞吐下界",
        best_add["value"] if best_add else None, "adds/cycle", "low" if best_add else "unavailable",
        f"best independent-chain kernel: {best_add['labels'].get('kernel')}" if best_add else "perf unavailable",
        "Approximates usable execution throughput, not a literal port count.", "core",
    ))
    dep_add = [
        item for item in _matching(observations, "pipeline", "cycles_per_operation")
        if item["labels"].get("kernel") == "integer_add_dependency"
    ]
    estimates.append(_estimate(
        "core.integer_add_latency", "整数加法依赖延迟", dep_add[0]["value"] if dep_add else None,
        "cycles/op", "medium" if dep_add else "unavailable", "single dependency-chain scalar add", category="core",
    ))
    dep_mul = [
        item for item in _matching(observations, "pipeline", "cycles_per_operation")
        if item["labels"].get("kernel") == "integer_mul_dependency"
    ]
    estimates.append(_estimate(
        "core.integer_multiply_latency", "整数乘法依赖延迟", dep_mul[0]["value"] if dep_mul else None,
        "cycles/op", "medium" if dep_mul else "unavailable", "single dependency-chain scalar multiply", category="core",
    ))
    mul_parallel = [
        item for item in _matching(observations, "pipeline", "operations_per_cycle")
        if item["labels"].get("kernel") == "integer_mul_parallel4"
    ]
    estimates.append(_estimate(
        "core.integer_multiply_throughput", "整数乘法后端吞吐",
        mul_parallel[0]["value"] if mul_parallel else None, "multiplies/cycle",
        "medium" if mul_parallel else "unavailable", "four independent scalar multiply chains", category="core",
    ))
    frequencies = _matching(observations, "pipeline", "effective_core_frequency")
    estimates.append(_estimate(
        "core.effective_frequency", "微基准期间有效核心频率",
        _median(item["value"] for item in frequencies), "GHz", "medium" if frequencies else "unavailable",
        "median perf core cycles / wall time across scalar kernels", category="core",
    ))

    rob = _matching(observations, "reorder_window", "rob_capacity_proxy")
    estimates.append(_estimate(
        "core.rob_capacity_proxy", "ROB/乱序窗口容量估计", rob[0]["value"] if rob else None,
        "estimated µops", "low" if rob else "unavailable",
        rob[0]["method"] if rob else "probe unavailable or no stable knee",
        "Dynamic loop-uop estimate; validate with multiple frequencies and perf permissions.", "core",
    ))

    branch_time = _matching(observations, "branch", "time_per_branch")
    branch_by_pattern = {item["labels"].get("pattern"): item["value"] for item in branch_time}
    if "random" in branch_by_pattern and "always-taken" in branch_by_pattern:
        estimates.append(_estimate(
            "branch.unpredictable_penalty", "随机分支额外代价",
            branch_by_pattern["random"] - branch_by_pattern["always-taken"], "ns/branch", "medium",
            "random-pattern time minus always-taken time", category="branch",
        ))
    branch_misses = {
        item["labels"].get("pattern"): item["value"]
        for item in _matching(observations, "branch", "miss_rate")
    }
    estimates.append(_estimate(
        "branch.random_miss_rate", "随机分支误预测率", branch_misses.get("random"), "%",
        "high" if "random" in branch_misses else "unavailable", "perf branch misses / branches", category="branch",
    ))

    mlp_rates = _matching(observations, "memory_parallelism", "random_load_rate")
    one_chain_rate = next((item["value"] for item in mlp_rates if item["labels"].get("chains") == "1"), None)
    best_mlp = max(mlp_rates, key=lambda item: item["value"], default=None)
    estimates.append(_estimate(
        "memory.max_observed_mlp_speedup", "随机加载并行度加速",
        best_mlp["value"] / one_chain_rate if best_mlp and one_chain_rate else None, "×",
        "medium" if best_mlp and one_chain_rate else "unavailable",
        f"best independent chain count: {best_mlp['labels'].get('chains')}" if best_mlp else "not measured",
        "A software-visible MLP lower bound; the total working set is held approximately constant.", "memory",
    ))
    estimates.append(_estimate(
        "memory.mlp_saturation_chains", "最佳随机加载独立链数",
        int(best_mlp["labels"]["chains"]) if best_mlp else None, "chains",
        "medium" if best_mlp else "unavailable", "chain count with maximum measured random-load rate", category="memory",
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
            "first stride below 50% of 8-byte-stride access rate",
            "Combines cache-line utilization, vectorization and hardware prefetch behavior.", "memory",
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
            item["confidence"] if item else "unavailable", item["method"] if item else "not measured", category="os",
        ))

    return estimates, diagnostics
