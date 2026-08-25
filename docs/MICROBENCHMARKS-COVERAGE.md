# clamchowder/Microbenchmarks 功能覆盖审计

本文以 `clamchowder/Microbenchmarks` 的提交
[`13159d44086d8b042e1c56218cfbfa24ae940899`](https://github.com/clamchowder/Microbenchmarks/tree/13159d44086d8b042e1c56218cfbfa24ae940899)
为固定审计基线。参考仓库本身将自己定义为实验性 playground，其中部分源码未接入默认生成器、不能编译或只支持特定操作系统/ISA；因此这里分别记录“参考仓库当前可运行功能”和“仓库中存在的实验源码”，不把文件存在等同于稳定功能。

## 状态定义

| 状态 | 含义 |
|---|---|
| 原生等价 | System Decap 有相同目的、可自动运行并输出结构化数据的探针 |
| 扩展覆盖 | 覆盖同一目的，并额外提供拓扑放置、扩展曲线、置信度或报告图 |
| 代理覆盖 | 可观察同类结构压力，但不能声称与参考实现或硬件真实容量一一等价 |
| 部分覆盖 | 覆盖功能类别，但没有穷举参考项目的每种 ISA/指令变体 |
| 范围外 | 不属于当前 x86-64、ARM64、C86 Linux CPU 黑盒表征范围 |

## 顶层模块对照

| 参考模块 | 参考功能 | System Decap 对应实现 | 状态与边界 |
|---|---|---|---|
| `CoherencyLatency` | 两线程锁/缓存一致性延迟 | `core_latency/cacheline_handoff_latency`；按 SMT、同 NUMA、跨 NUMA、跨插槽分类；standard 输出物理核心 CPU×CPU 矩阵，deep 输出逻辑 CPU 矩阵 | 扩展覆盖。使用 release/acquire 缓存行乒乓，报告单向时间；协议状态不等同于物理链路延迟 |
| `CoreClockChecker` | 单核/多核负载下的核心时钟 | `pipeline/effective_core_frequency` 与 `compute_scaling/effective_core_frequency`；同时清点 cpufreq policy、governor、min/max/current | 扩展覆盖。精确核心周期依赖 `perf_event_open` 权限；不把 TSC/CNTVCT 当成核心时钟 |
| `InstructionRate` | x86/ARM 多种整数、浮点、融合和向量指令的延迟/吞吐 | 标量整数 add/mul 与 FP64 add/mul 的依赖链、四/八独立链，输出 Gop/s、ops/cycle、cycles/op、IPC | 部分覆盖。覆盖整数与 FP64 执行能力类别，但未穷举 AVX/AVX-512/NEON/SVE、AES、除法、转换、宏融合等每个参考微内核 |
| `LoadedMemoryLatency` | 在不同内存带宽压力下测随机加载延迟 | `loaded_memory_latency` 同时输出压力线程数、实际并发读取 GB/s 与依赖加载 ns/access | 原生等价，并增加实测压力强度曲线 |
| `MemoryBandwidth` | 单/多线程数据侧读写复制；private/shared；指令侧 4/8 字节 NOP | `memory_bandwidth` 的 read/write/copy/triad 物理核心扩展；`cache_bandwidth` 工作集曲线；`instruction_fetch` 的 x86 1/4/8 字节 NOP 和 A64 4 字节 NOP | 部分到扩展覆盖。已有 private 分片与代码足迹能力；未把多个核心读取同一地址的 request combining 模式作为“带宽”主结果，也未提供每种手写 AVX/NEON 变体 |
| `MemoryLatency` | 缓存/内存随机延迟、TLB、hugepage、store-to-load forwarding | `cache_latency`、`tlb_latency`、`page_policy`、`store_forwarding` | 扩展覆盖主要类别。页面策略记录实际 `AnonHugePages`；STLF 覆盖同址 8→8、部分 4→8、错位与跨缓存行，未复刻参考项目的 x86 128-bit FP/vector 特例 |
| `mt_instructionrate` | 多线程指令吞吐与 SMT/核心扩展 | `compute_scaling` 对 NUMA 均衡物理核心按 1/2/4/… 扩展，并单测一组 SMT sibling；同步记录有效频率 | 部分到扩展覆盖。当前扩展内核为整数加法，不穷举参考项目全部 ISA 微内核 |
| `GpuMemLatency` | OpenCL GPU 全局/本地/纹理内存延迟、带宽、原子与指令率 | 无 | 范围外。当前产品承诺是 Linux CPU 平台（x86-64、ARM64、C86），没有把 OpenCL 设备作为目标 |
| `svm` | OpenCL SVM CPU↔GPU 原子延迟 | 无 | 范围外。需要 OpenCL 运行时、支持细粒度 SVM 的 GPU 和独立设备拓扑模型 |

## AsmGen 与微架构结构探针

参考提交的 `AsmGen/Program.cs` 默认只注册 4/8/16/32/64 字节间距的无条件 BTB、4/8/16/32 字节间距的条件 BTB，以及方向历史测试。目录中还保留许多未注册的 ROB、scheduler、register-file、load/store queue、RAS 和 indirect-branch 实验类。

| 结构 | System Decap 实现 | 覆盖状态 |
|---|---|---|
| 无条件/始终跳转条件 BTB | W^X 生成式直接分支，4/8/16/32/64 B 间距，分支数按 2× 扩展；输出 ns/branch、counter-ticks 与可用时的 perf miss rate | 原生等价当前默认生成器；容量只报告压力曲线拐点 |
| 方向预测历史 | 重复伪随机方向周期按 1/2/4/… 扩展；输出时间与 miss rate | 原生等价功能类别。参考实现还可让多个静态分支各自拥有模式，本实现当前使用一个标量分支点 |
| 返回地址栈（RAS） | 生成具有唯一返回地址的嵌套 call/ret 链，深度按 2× 扩展 | 原生等价功能类别；拐点为低置信代理 |
| 间接目标预测 | 一个间接调用点按打乱顺序访问 1/2/4/… 个生成目标 | 原生等价功能类别；没有同时二维扫描静态分支点数量 |
| ROB/乱序窗口 | x86/C86 两个冷加载之间插入动态独立 µop 窗口，输出完整重叠损失曲线和保守拐点 | 代理覆盖；可能先耗尽 ROB、scheduler、load queue、物理寄存器或循环前端。ARM64 明确不可用 |
| 整数/FP scheduler 与物理寄存器文件 | 1/4/8 条整数链、1/4 条 FP64 链可显示吞吐和依赖差异 | 部分代理；没有足够证据输出 scheduler entries 或 register-file entries |
| Load queue / Store queue / store-data queue | MLP 独立加载链、STLF 对齐矩阵和流式读写可以施加对应压力 | 部分代理；不会把这些曲线自动命名为精确 LQ/SQ 条目数 |
| 分支缓冲、混合资源与 ISA 专用结构 | 基础分支、ROB、整数/FP、加载/存储曲线可交叉观察 | 未逐项复刻。参考目录中的大量类也是未注册实验，且高度依赖具体微架构和指令集 |

## 相比参考仓库的额外能力

System Decap 还提供参考仓库没有统一集成的能力：

- CPU、cache、SMT、die、socket、NUMA、DMI、内核、频率和运行环境自动清点；
- 单核缓存带宽与系统 DRAM 带宽明确分图，避免把 32 KiB 的 L1/L2 数百 GB/s 误解为内存总带宽；
- CPU node × memory node 的延迟和有效载荷带宽矩阵；
- 全部物理核心或逻辑 CPU 的核间一致性延迟矩阵；
- 页策略实际大页驻留量、预取步长、MLP、伪共享和操作系统开销；
- 单文件中文离线 HTML、原始 JSON/CSV、指标目录、推断依据、置信度和 caveat。

## 结论

不能严谨地宣称“逐个源码实验 100% 等价覆盖”：GPU/SVM 明确在产品范围外，CPU 指令率与未注册的微架构结构实验也没有逐指令、逐结构复刻。当前版本已经覆盖参考仓库中适用于 x86-64、ARM64、C86 的主要**可运行 CPU 功能类别**，并将不能稳定倒推出真值的项目保留为原始压力曲线或低置信代理。若以后扩展 SIMD/矩阵单元或 GPU，应新增独立 capability 检测和可选后端，不应让目标机因为缺少对应 ISA/OpenCL 运行时而无法完成基础报告。
