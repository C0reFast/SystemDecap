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

每个基础页只放一个指针，页序随机，保持依赖加载。基础页探针显式请求
`MADV_NOHUGEPAGE`，并让相邻虚拟页使用轮换的 cache-line offset，将访问分散到
L1D cache set，避免“每页 offset 0”把 cache associativity 冲突误判成 TLB 拐点。输出：

- page count；
- page size；
- virtual working set；
- ns/access；
- 相邻点 latency ratio 与经验 knee。

首个拐点必须在后续至少两个测试点持续升高；孤立尖峰会被拒绝。它仍只是某级
TLB/页表 cache 的容量候选，不应直接命名为“L1 DTLB entries”。THP 状态、页表层数、
ASID/PCID、虚拟化二阶段翻译都会改变结果。

standard/deep 还会对同一随机依赖加载工作集分别请求 `MADV_NOHUGEPAGE` 与
`MADV_HUGEPAGE`，并从 `/proc/self/smaps` 读取 `AnonHugePages`。因此报告能区分“提出
THP 建议”和“映射确实获得匿名大页”；madvise 不保证内核一定折叠页面。

待扩展：显式 hugetlbfs 2 MiB/1 GiB TLB、iTLB、TLB shootdown、page-walk concurrency、NUMA page-table placement。

## 5. 内存带宽

四种 pinned physical-core 流式 kernel：

| Kernel | 有效 payload 计数 | 主要观察 |
|---|---:|---|
| read | 8 B/element | 读通道、控制器、预取器 |
| write | 8 B/element | store pipeline；不把 RFO 算入 payload |
| copy | 16 B/element | read + write payload |
| triad | 24 B/element | 两读一写 + scalar arithmetic |

线程数按 `1, 2, 4, …, physical cores` 扩展。每个线程固定到不同物理核心，使用不重叠分片。输出单核值、每个扩展点、最佳聚合点和饱和趋势。

工具从每个可见 CPU 的 sysfs cache 索引枚举并按 `shared_cpu_list` 去重 LLC 实例，
而不是只读取主核所在 CCD 的一个 L3。自动工作集以“至少 4 × 可见整机 LLC”为目标，
同时受档位上限与 `/proc/meminfo` 的 `MemAvailable` 安全预算约束。每个点记录
`working_set_bytes`、`aggregate_llc_bytes`、`bytes_per_thread` 和
`working_set_exceeds_llc`。没有达到 4× LLC 的值仍保留用于诊断，但不得作为 DRAM
总带宽。Python 推断层会用原始字节数字重新校验，防止 `--skip-build` 误用旧二进制的
2× 标签。

read kernel 使用 x86/C86 AVX2/SSE2 或 ARM64 NEON 手写向量汇编；其余流式 kernel
由编译器向量化。普通匿名页是 write-back 内存，手写汇编和 non-temporal hint 都不能
保证绕过全部缓存。工具会解析 SMBIOS Type 17 的 DIMM 数据位宽与配置 MT/s，或者接受
`--memory-channels` / `--memory-mtps` 覆盖项，计算保守配置上界。超过上界 5% 容差的
读取点自动判为无效。不把 ECC、command、coherence、RFO 和 fabric protocol bytes
计入有效载荷。

带载延迟探针在一个固定物理核心执行 DRAM 级随机依赖加载，同时逐级增加其他物理核心上的流式读取线程。每个点同时记录 `ns/access`、压力线程数和真正达到的并发 `GB/s`，用于观察控制器/互联接近饱和时的 latency cliff。

指令侧带宽使用 W^X 生成代码：先写入已知 NOP/RET 序列，清理指令缓存后再通过
`mprotect` 切换为 RX，从不创建 RWX 映射。x86/C86 扫描 1/4/8 字节 NOP，ARM64 扫描
4 字节 A64 NOP。`code_delivery_bandwidth` 按每轮代码体字节数计算，适合观察 L1I、
iTLB 和下级取指路径的相对台阶，不应解释成 DRAM 物理带宽。

待扩展：non-temporal write、read-for-ownership、混合读写比例、随机带宽、bank/rank/channel interleave、DRAM row hit/miss、memory controller 数量、CXL/PMEM 分层。

## 6. NUMA 与互联

对每个 `(CPU node, memory node)` 组合：

1. `mbind(MPOL_BIND)` 请求目标 memory node；失败时将初始化线程固定在该 node，使用 first-touch 退化路径。
2. 随机依赖链测 `ns/access`。
3. 目标 CPU node 的物理核心并行读取同一 memory node，测 payload GB/s。

矩阵对角线是 local，非对角线进一步按 socket 拆成
`cross-numa-same-socket` 与 `cross-socket`。摘要分别提供同插槽跨 NPS/NUMA 和跨插槽的
延迟与有效载荷，避免把两种物理路径混成一个数。NUMA 带宽工作集以读取节点的全部
LLC 的 4 倍为目标；每个点保留读写 socket、放置方式与 LLC 覆盖证据。只有达到 4×
读取节点 LLC 且未超过配置理论上界的值才能进入摘要。远端 payload 是互联可承载的有效载荷，不能直接
倒推出物理 link 宽度、lane 数或 signaling rate。

待扩展：双向同时流量、all-to-all saturation、不同 hop 数、socket/die fabric 分离、coherence 与 non-coherent DMA、GPU/CXL/PCIe peer traffic。

## 7. 核间一致性与共享

### Cache-line handoff

两个固定线程通过一个独占 cache line 做 release/acquire ping-pong，往返时间除 2，输出 `ns/one-way`。自动分类：

- SMT sibling；
- same LLC / different core；
- cross LLC / same NUMA；
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
- 4 条独立 multiply chains：multiply throughput；
- FP64 add/multiply 的单依赖链与四独立链：标量浮点延迟和吞吐。

性能计数器拆成三个独立小组：`cycles + instructions`、
`branches + branch-misses`、`cache-references + cache-misses`。每组均检查
open/reset/enable/disable/read，并运行短负载确认 `time_running > 0`；因此分支或缓存
事件受约束时不会再连带使 IPC 失效。元数据与中文警告保留每组具体错误、
`perf_event_paranoid` 和 `nmi_watchdog`。若核心组可用，每个 kernel 同时输出：

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

“后端有多少个”没有单一定义：执行 port、scheduler、ALU、AGU、load/store pipe、vector pipe 都不同。要进一步分解，需要增加 load-only/store-only、LEA/shift/divide、FP32、NEON/SVE/AVX2/AVX-512、混合指令对和 port-contention probes。

物理核心扩展探针把八条独立整数加法链固定到 NUMA 均衡的物理核心集合，按
`1,2,4,…` 扩展；另取一组 SMT sibling 测双线程共享核心。同步采集每个工作线程的
perf core cycles/wall-time，因此可把吞吐扩展下降与全核频率变化放在同一张报告中比较。

## 9. ROB / 乱序窗口

当前 x86/C86 probe 把两个被 flush 的独立 cache miss 分开，中间插入运行时生成、
静态展开的简单整数加法序列。每条填充指令精确对应一个简单整数 µop，不再使用包含
循环索引、比较和跳转的动态 C++ 循环。只有连续三个点保持 cold-hot penalty 台阶时
才输出代理值；报告保留精确填充指令数、第二次 load 前的固定指令数和保守区间。

这仍是**低置信代理**，因为结果可能由以下任一资源先耗尽：ROB、integer scheduler、
load queue、physical registers、dispatch width 或 memory-level parallelism。SMT 活跃状态
也可能改变每线程可见窗口；报告不得把该代理值写成硬件 ROB 真值。

ARM64 目前不输出 ROB 数量，明确标记 unavailable。跨 ARM vendor 稳定清理 cache line 且生成精确可变静态 µop 窗口需要单独 backend；输出一个不可靠数字比留空更糟。

可继续增加：load/store queue depth、physical register file、scheduler entries、memory disambiguation、store forwarding window、maximum outstanding misses/MSHR。

## 10. 分支系统

固定 scalar branch loop 测 always-taken、alternating、随机位序列，输出 ns/branch、perf branch miss rate 与 IPC。随机减 always-taken 是误预测恢复额外代价的时间代理。

standard/deep 还运行四类 W^X/标量压力曲线：

- BTB：无条件和始终跳转条件分支，4/8/16/32/64 B 地址间距，分支数按 2× 扩展；
- direction history：重复伪随机方向周期从 1 按 2× 扩展；
- RAS：每层具有唯一返回地址的嵌套 call/ret 链；
- indirect target：一个间接调用点按打乱顺序访问逐渐增多的生成目标。

每类都保留 wall time，能访问 perf 时还记录 generic branch miss rate。推断器只在小足迹基线明显恶化时给出“拐点”，并固定标为低置信度；它们可能同时受到多级预测器、地址别名、iTLB、代码缓存和通用 perf 事件定义影响。

待扩展：BTB associativity/set mapping、每个静态分支独立方向历史、二维间接分支点×目标扫描、taken/not-taken asymmetry、跨 context predictor isolation。

## 11. Store-to-load forwarding

固定核心上的依赖 store/load 对覆盖四种布局：同址 8→8 基线、同址部分 4→8、加载地址错开 1 字节，以及 8 字节访问跨越 64 字节缓存行。输出 ns/pair 和平台计数器 tick/pair；相对基线的额外代价用于识别转发失败、重放或跨行路径。该探针不把结果解释为 store queue 容量。

## 12. 建议补充的系统级探针

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
