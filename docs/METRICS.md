# 指标与推断方法

本文档区分四类信息：`inventory`（OS 暴露的静态事实）、`measured`（直接微基准）、`inferred`（由曲线或下界推断）、`unavailable`（软件黑盒无法稳定确定或当前架构未实现）。报告保留所有原始点，推断器只生成便于阅读的摘要，不删除反常点。

## 1. 身份、固件与可见边界

| 指标 | 来源 | 典型置信度 | 备注 |
|---|---|---|---|
| ISA / platform family | `uname`, `/proc/cpuinfo` | 高 | Hygon vendor 映射为 `c86-hygon` |
| vendor/model/family/model/stepping | `/proc/cpuinfo` | 高 | ARM 使用 implementer/part/architecture 字段 |
| microcode | `/proc/cpuinfo` | 中高 | ARM/固件可能不暴露 |
| ISA flags | `/proc/cpuinfo` | 高 | 表示内核允许当前进程使用的能力 |
| DMI/BIOS/board | `/sys/class/dmi/id` | 中高 | ARM ACPI/DT 平台可能为空 |
| kernel/OS/clock source | uname/sysfs | 高 | 直接影响实验环境 |
| virtualization/container | flags、procfs、容器标记 | 中 | 嵌套虚拟化可能无法完全识别 |
| CPU vulnerability mitigation | sysfs | 高 | 是当前内核状态，不是硅片原始能力 |
| affinity/cgroup 范围 | `sched_getaffinity` | 高 | 所有“系统”结果限定在该范围内 |

补充清点：页大小、THP、hugepage 数、`perf_event_paranoid`、cpufreq driver/governor/min/max/current、温区、块设备容量/rotational/NUMA、网络 speed/MTU/NUMA。

## 2. CPU 拓扑

从每个 `cpuN/topology` 和 `cpuN/nodeX` 重建：

- online/allowed logical CPU list；
- socket (`physical_package_id`)；
- die (`die_id`)；
- cluster（ARM hybrid/cluster 平台可选）；
- core (`core_id`)；
- SMT sibling 和 core sibling；
- 每节点 CPU、内存总量、free memory、NUMA distance row；
- 可用 physical core、threads/core、die、socket 与 NUMA node 数量。

异构 ARM 核可通过 `cpu_capacity` 和 cpufreq policy 看到第一层线索；当前报告不把 capacity 自动解释成具体微架构型号。

## 3. Cache hierarchy

### 静态字段

每个 cache instance 报告 level、Instruction/Data/Unified、capacity、coherency line、ways、sets、partitions、shared CPU list。`size ≈ line × sets × ways × partitions` 可用于固件一致性复核。

### 经验 latency 曲线

在固定 CPU 上构造每 cache-line 一个指针的随机单环；下一地址依赖上一加载结果，压制 memory-level parallelism 和预取。工作集从 4 KiB 按 2× 扩展：

```text
load p0 → load p1 → load p2 → ...
```

输出 `ns/access × working_set_bytes`。明显台阶是 L1/L2/L3/TLB/DRAM 边界的候选。直接将台阶等同于容量并不严谨，因为 set mapping、inclusive/exclusive policy、slice hash、共享竞争、THP 和 TLB 都会移动曲线。

摘要中的 L1/L2/L3 representative latency 取不超过对应 cache 容量的工作集点；DRAM proxy 取最大工作集点。如果最大工作集没有显著超过 LLC，置信度降低。

可继续扩展的 cache 指标：instruction-cache 容量、associativity conflict sweep、replacement policy、inclusive/exclusive、LLC slice/hash、cache-to-cache clean/dirty transfer、non-temporal load/store、hardware prefetch distance 与 stream 数量。

## 4. TLB 与地址翻译

每个基础页只放一个指针，页序随机，保持依赖加载。输出：

- page count；
- page size；
- virtual working set；
- ns/access；
- 相邻点 latency ratio 与经验 knee。

首个拐点只是某级 TLB/页表 cache 的容量候选，不应直接命名为“L1 DTLB entries”。THP 状态、页表层数、ASID/PCID、虚拟化二阶段翻译都会改变结果。

待扩展：2 MiB/1 GiB hugepage TLB、iTLB、TLB shootdown、page-walk concurrency、NUMA page-table placement。

## 5. 内存带宽

四种 pinned physical-core 流式 kernel：

| Kernel | 有效 payload 计数 | 主要观察 |
|---|---:|---|
| read | 8 B/element | 读通道、控制器、预取器 |
| write | 8 B/element | store pipeline；不把 RFO 算入 payload |
| copy | 16 B/element | read + write payload |
| triad | 24 B/element | 两读一写 + scalar arithmetic |

线程数按 `1, 2, 4, …, physical cores` 扩展。每个线程固定到不同物理核心，使用不重叠分片。输出单核值、每个扩展点、最佳聚合点和饱和趋势。

它不是理论 DDR 带宽：不读取 DIMM 速率/通道数，不把 ECC、command、coherence、RFO 和 fabric protocol bytes 计入。工作集应大于 LLC；短 profile 或用户指定过小工作集可能测到 cache bandwidth。

待扩展：non-temporal write、read-for-ownership、混合读写比例、随机带宽、loaded latency、bank/rank/channel interleave、DRAM row hit/miss、memory controller 数量、CXL/PMEM 分层。

## 6. NUMA 与互联

对每个 `(CPU node, memory node)` 组合：

1. `mbind(MPOL_BIND)` 请求目标 memory node；失败时将初始化线程固定在该 node，使用 first-touch 退化路径。
2. 随机依赖链测 `ns/access`。
3. 目标 CPU node 的物理核心并行读取同一 memory node，测 payload GB/s。

矩阵对角线是 local，非对角线是 remote。摘要提供 local/remote 中位 latency、remote/local penalty，以及 remote payload 的中位数。后者是“互联可承载的有效远端内存流量”，不能直接倒推出物理 link 宽度、lane 数或 signaling rate。

待扩展：双向同时流量、all-to-all saturation、不同 hop 数、socket/die fabric 分离、coherence 与 non-coherent DMA、GPU/CXL/PCIe peer traffic。

## 7. 核间一致性与共享

### Cache-line handoff

两个固定线程通过一个独占 cache line 做 release/acquire ping-pong，往返时间除 2，输出 `ns/one-way`。自动分类：

- SMT sibling；
- same NUMA / different core；
- cross NUMA / same socket；
- cross socket。

smoke/quick 每类选代表 pair；standard 采集全部可见物理核心 pair，deep 采集全部可见逻辑 CPU pair。该值包括 atomic polling 与协议状态转换，不等于物理 wire latency，但非常适合拓扑比较。

### False sharing

两个核心分别更新相距 8/16/32/64/128/256 B 的原子变量。吞吐恢复到独立 cache-line 水平的位置给出经验 line knee。sysfs `coherency_line_size` 仍是更高置信来源。

待扩展：MESI/MOESI state-specific clean/dirty transfer、read sharing fan-out、atomic CAS/fetch-add latency、lock convoy、directory/home-agent mapping。

## 8. 前端、IPC 与后端

架构专用 x86-64/AArch64 标量汇编提供可核对的 known-op microkernel：

- 128 NOP loop：frontend/retire 综合下界；
- 64 个单依赖链 integer add：add dependency latency proxy；
- 4/8 独立 add chains：integer backend 可用吞吐；
- 1 个 multiply dependency chain：integer multiply latency；
- 4 条独立 multiply chains：multiply throughput。

若 `perf_event_open` 可用，每个 kernel 同时输出：

- `instructions / core cycles`（retired IPC）；
- known operations / core cycles；
- core cycles / operation；
- generic cache miss rate；
- Gop/s。

否则保留 wall-time Gop/s 与 ops/platform-counter-tick，但不把 invariant TSC/CNTVCT tick 伪装成 core cycle。

推断规则：

- 最大实测 IPC 是 retire 能力的**下界**；
- NOP IPC 向下取整是 frontend/retire width 的低置信**下界**；
- parallel add 的 adds/cycle 是可用 arithmetic throughput，不是“后端端口数量”；
- dependency multiply 的 cycles/op 更接近 latency，但仍包含 loop 与计数开销。

“后端有多少个”没有单一定义：执行 port、scheduler、ALU、AGU、load/store pipe、vector pipe 都不同。要进一步分解，需要增加 load-only/store-only、LEA/shift/divide、FP32/FP64、NEON/SVE/AVX2/AVX-512、混合指令对和 port-contention probes。

## 9. ROB / 乱序窗口

当前 x86/C86 probe 把两个被 flush 的独立 cache miss 分开，中间插入可变长度动态 integer-uop loop。只要第二个 miss 能进入乱序窗口，两次 miss 会重叠；超过窗口后 cold-hot penalty 出现台阶。报告输出完整曲线、first knee 和保守 µop 区间。

这是**低置信代理**，因为结果可能由以下任一资源先耗尽：ROB、integer scheduler、load queue、physical registers、branch/loop buffer、dispatch width 或 memory-level parallelism。循环的 macro-fusion 也令“dynamic iteration → µop”只能估算。

ARM64 目前不输出 ROB 数量，明确标记 unavailable。跨 ARM vendor 稳定清理 cache line 且生成精确可变静态 µop 窗口需要单独 backend；输出一个不可靠数字比留空更糟。

可继续增加：load/store queue depth、physical register file、scheduler entries、memory disambiguation、store forwarding window、maximum outstanding misses/MSHR。

## 10. 分支系统

固定 scalar branch loop 测 always-taken、alternating、随机位序列，输出 ns/branch、perf branch miss rate 与 IPC。随机减 always-taken 是误预测恢复额外代价的时间代理。

待扩展：BTB levels/entries/associativity、direction predictor history length、RAS depth、indirect target predictor、taken/not-taken asymmetry、跨 context predictor isolation。

## 11. 建议补充的系统级探针

这些指标已纳入路线，但当前版本不应伪装成已实现：

- 标量/向量 INT8/FP16/BF16/FP32/FP64 峰值吞吐与频率降档；
- AVX/SVE vector length、matrix/tensor 单元；
- instruction-cache、decoded µop cache、loop buffer 容量；
- syscall、futex、进程/线程 context switch、IPI、TLB shootdown；
- page fault、zero-fill、fork/COW、allocator 与 hugepage 成本；
- PCIe generation/width/NUMA locality、DMA/IOMMU latency/bandwidth；
- SSD sequential/random IOPS、queue depth、fsync latency；
- NIC pps/throughput/IRQ/RSS/NUMA；
- power/energy per operation、idle state exit、turbo residency、thermal throttling；
- RAS/ECC/EDAC、machine check、memory poison；
- 多租户干扰、cache QoS/CAT/MPAM、memory bandwidth allocation；
- 长时间稳定性、噪声分布、置信区间和跨运行回归比较。

这些扩展应继续遵循同一数据契约：raw observation、unit、labels、method、confidence、caveat、可复现命令。
