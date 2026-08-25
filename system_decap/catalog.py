"""Metric catalog and machine-readable measurement caveats."""

METRICS = [
    # Identity and topology
    ("identity", "CPU 厂商、型号、family、stepping 与微码", "inventory", "high"),
    ("identity", "ISA 特性与虚拟化状态", "inventory", "high"),
    ("topology", "逻辑 CPU、物理核心、SMT、die、插槽与 NUMA 节点", "inventory", "high"),
    ("topology", "CPU 亲和性与 cgroup 可见范围限制", "inventory", "high"),
    ("topology", "NUMA 距离表与节点内存", "inventory", "high"),
    ("cache", "L1/L2/L3 类型、共享域、缓存行、组数与路数", "inventory", "high"),
    # Memory hierarchy
    ("latency", "随机依赖加载延迟随工作集变化", "measured", "high"),
    ("latency", "L1/L2/L3/DRAM 代表性延迟", "inferred", "medium"),
    ("latency", "TLB/页表遍历延迟随页数变化", "measured", "medium"),
    ("latency", "NUMA 本地/远端内存延迟矩阵", "measured", "high"),
    ("bandwidth", "单核读取、写入、复制与三元运算带宽", "measured", "high"),
    ("bandwidth", "聚合带宽随核心数扩展曲线", "measured", "high"),
    ("bandwidth", "NUMA 本地/远端读取带宽矩阵", "measured", "high"),
    ("bandwidth", "跨 NUMA 互联有效载荷带宽", "inferred", "medium"),
    ("bandwidth", "单核缓存带宽随工作集变化", "measured", "medium"),
    ("memory", "访问步长与预取敏感度", "measured", "medium"),
    ("memory", "独立链数量与内存级并行度", "measured", "medium"),
    ("coherence", "核间缓存行传递延迟与 CPU×CPU 矩阵", "measured", "high"),
    ("coherence", "SMT、同核、跨 NUMA、跨插槽乒乓分类", "measured", "high"),
    ("coherence", "伪共享敏感度与缓存行边界", "measured", "medium"),
    # Core and frontend/backend
    ("core", "各微内核的退休 IPC", "measured with perf", "high"),
    ("core", "前端退休/译码宽度下界", "inferred", "low"),
    ("core", "整数加法执行通道吞吐下界", "inferred", "low"),
    ("core", "整数乘法吞吐", "measured", "medium"),
    ("core", "依赖链延迟代理量", "measured", "medium"),
    ("core", "乱序窗口/ROB 容量范围", "inferred on x86", "low"),
    ("branch", "可预测、交替与随机分支代价", "measured", "medium"),
    ("branch", "分支误预测率", "measured with perf", "high"),
    # OS/environment
    ("os", "计时器开销与分辨率", "measured", "high"),
    ("os", "系统调用、次缺页与调度交接", "measured", "medium"),
    ("os", "内核、页大小、THP、大页与 perf 策略", "inventory", "high"),
    ("environment", "频率策略、governor 与温区", "inventory", "high"),
    ("environment", "内核 CPU 漏洞状态", "inventory", "high"),
    ("environment", "块设备与网络接口", "inventory", "high"),
]


def as_dicts():
    return [
        {"category": c, "metric": m, "kind": k, "nominal_confidence": q}
        for c, m, k, q in METRICS
    ]
