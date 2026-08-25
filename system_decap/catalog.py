"""Metric catalog and machine-readable measurement caveats."""

METRICS = [
    # Identity and topology
    ("identity", "CPU vendor/model/family/stepping/microcode", "inventory", "high"),
    ("identity", "ISA features and virtualization", "inventory", "high"),
    ("topology", "logical CPUs, cores, SMT, dies, sockets, NUMA nodes", "inventory", "high"),
    ("topology", "CPU affinity/cgroup restrictions", "inventory", "high"),
    ("topology", "NUMA distance table and node memory", "inventory", "high"),
    ("cache", "L1/L2/L3 type, sharing, line size, sets, ways", "inventory", "high"),
    # Memory hierarchy
    ("latency", "random dependent-load latency vs working set", "measured", "high"),
    ("latency", "L1/L2/L3/DRAM representative latency", "inferred", "medium"),
    ("latency", "TLB/page-walk latency vs page count", "measured", "medium"),
    ("latency", "local/remote NUMA memory latency matrix", "measured", "high"),
    ("bandwidth", "single-core read/write/copy/triad bandwidth", "measured", "high"),
    ("bandwidth", "aggregate bandwidth scaling vs core count", "measured", "high"),
    ("bandwidth", "NUMA local/remote read bandwidth matrix", "measured", "high"),
    ("bandwidth", "cross-NUMA interconnect payload bandwidth", "inferred", "medium"),
    ("bandwidth", "one-core cache bandwidth vs working set", "measured", "medium"),
    ("memory", "stride/prefetch sensitivity", "measured", "medium"),
    ("memory", "memory-level parallelism vs independent chains", "measured", "medium"),
    ("coherence", "core-to-core cache-line transfer latency", "measured", "high"),
    ("coherence", "SMT/core/NUMA/socket ping-pong classes", "measured", "high"),
    ("coherence", "false-sharing sensitivity and line-size knee", "measured", "medium"),
    # Core and frontend/backend
    ("core", "retired IPC by microkernel", "measured with perf", "high"),
    ("core", "frontend retire/decode-width lower bound", "inferred", "low"),
    ("core", "integer add execution-lane lower bound", "inferred", "low"),
    ("core", "integer multiply throughput", "measured", "medium"),
    ("core", "dependency-chain latency proxy", "measured", "medium"),
    ("core", "reorder-window/ROB capacity range", "inferred on x86", "low"),
    ("branch", "predictable/alternating/random branch cost", "measured", "medium"),
    ("branch", "branch miss rate", "measured with perf", "high"),
    # OS/environment
    ("os", "timer overhead and resolution", "measured", "high"),
    ("os", "syscall, minor page fault, scheduler handoff", "measured", "medium"),
    ("os", "kernel, page size, THP, huge pages, perf policy", "inventory", "high"),
    ("environment", "frequency policy, governor, thermal zones", "inventory", "high"),
    ("environment", "kernel CPU vulnerability status", "inventory", "high"),
    ("environment", "block devices and network interfaces", "inventory", "high"),
]


def as_dicts():
    return [
        {"category": c, "metric": m, "kind": k, "nominal_confidence": q}
        for c, m, k, q in METRICS
    ]
