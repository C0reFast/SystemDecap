# System Decap

System Decap 是面向全新 Linux 服务器平台的黑盒表征工具集。在没有 CPU/SoC 数据手册的条件下，它通过 sysfs/procfs 清点、固定 CPU/内存放置的微基准、`perf_event_open` 硬件计数器和保守推断，重建 CPU 核心、缓存、内存、NUMA 与一致性互联的可观察轮廓。测试结果以 JSON/CSV 保存，并由浏览器报告工作台按需加载、绘图和执行多报告对比。

支持：

- x86-64（Intel、AMD 及兼容实现）
- ARM64 / AArch64
- C86 / 海光（运行时识别 `HygonGenuine`，使用经过 C86 兼容的 x86-64 探针）

> “倒推”不等于读取隐藏寄存器。缓存容量、拓扑等可以高置信清点；IPC、延迟和带宽可以直接测量；前端宽度、执行后端数量、ROB 容量只能得到下界、代理量或区间。报告中的每项推断都附带 `confidence`、方法和 caveat。

## 快速开始

要求 Linux、Python 3.10+、CMake 3.16+、支持 C++20 的 GCC/Clang；Ninja 可选。不依赖 NumPy、浏览器 CDN、libnuma 或第三方图表库。

```bash
./system-decap run --profile standard
```

输出目录默认为 `reports/<hostname>-<timestamp>/`：

```text
report.json        系统清点、原始观测、推断和置信度
observations.csv   所有原始测量点，便于二次分析
native-raw.json    C++ 微基准的未经推断输出
lscpu.txt          便于人工复核的 lscpu 快照
```

常用命令：

```bash
# 约 1–3 分钟内完成的快速侦察；具体时间取决于核心数
./system-decap run --profile quick

# 默认完整运行；推荐用于正式比较
./system-decap run --profile standard

# 更大的工作集、更长采样、全部可见逻辑 CPU 对矩阵
./system-decap run --profile deep

# CI/安装检查，不应作为性能结论
./system-decap run --profile smoke --memory-mib 8 --duration-ms 15

# 查看所有可独立运行的底层探针
./build/sdc-native --list-probes

# 只运行单个探针，或组合运行多个探针；仍会生成完整 JSON/CSV 数据
./system-decap run --profile standard --only memory-bandwidth
./system-decap run --profile quick --only numa,core-latency

# 只清点，不执行负载
./system-decap inventory --output inventory.json

# 编译、查看指标目录
./system-decap build
./system-decap list-metrics

# 启动 B/S 报告工作台并自动打开浏览器
./system-decap serve --reports-dir reports --open-browser
```

## 浏览器报告与多报告对比

`run` 不再为每次测试复制一份嵌入数据的 HTML。报告仓库中的
`reports/*/report.json` 是唯一展示数据源，统一前端通过同源 HTTP API 加载：

| 接口 | 用途 |
|---|---|
| `GET /api/reports` | 返回报告目录和主机、CPU、Profile、时间等摘要 |
| `GET /api/reports/<id>` | 返回一份完整的 `report.json` |
| `GET /` | 报告选择、单报告分析和多报告对比工作台 |

启动服务后访问 `http://127.0.0.1:8000/`。左侧勾选 1–8 份报告，第一份作为对比
基线；前端会显示核心指标横向表、相对基线百分比、平台条件和多组原始曲线叠加。
选择会写入 URL，因此可以收藏或分享同一组对比。单报告模式保留核心摘要、缓存/TLB/
内存/分支曲线、NUMA 与核间延迟矩阵、原始观测筛选；大规模核间矩阵使用 Canvas
绘制并支持缩放。

默认只监听 `127.0.0.1`。从远程测试机查看时，推荐使用 SSH 端口转发：

```bash
# 测试机
./system-decap serve --reports-dir /root/SystemDecap/reports --port 8000

# 本地电脑
ssh -L 8000:127.0.0.1:8000 root@TARGET
```

确需局域网直接访问时可以传 `--host 0.0.0.0`，但内置服务没有身份认证；应配合
防火墙或带认证的反向代理，不能直接暴露到不可信网络。

自定义 STREAM 每个数组的工作集和每个吞吐点的最短持续时间：

```bash
./system-decap run --profile standard --memory-mib 512 --duration-ms 400 \
  --output /var/tmp/platform-characterization
```

若固件没有暴露 SMBIOS 内存设备记录，可手动提供 64-bit 通道数和配置速率。以
12 通道 DDR5-6400 为例，报告会计算 `12 × 8 B × 6400 MT/s = 614.4 GB/s`，并用它
校验实测值：

```bash
./system-decap run --profile standard \
  --memory-channels 12 --memory-mtps 6400
```

## Profile 作用域

| Profile | 缓存 sweep 上限 | STREAM 默认每数组 | 吞吐点时长 | 核间测量 | 用途 |
|---|---:|---:|---:|---|---|
| `smoke` | 64 KiB | 8 MiB | 15 ms | 代表关系 | 编译/冒烟 |
| `quick` | ≥32 MiB | ≥64 MiB | 80 ms | 每种关系一对 | 快速侦察 |
| `standard` | ≥128 MiB | 8 GiB | 200 ms | 全部物理核心对 | 正式基线 |
| `deep` | ≥512 MiB | ≥1 GiB | 500 ms | 全部可见逻辑 CPU 对 | 架构研究 |

未显式传 `--memory-mib` 时，工具会按 `shared_cpu_list` 去重所有可见 LLC 实例，
把 STREAM 工作集扩展到整机 LLC 的至少 4 倍，并把 NUMA 工作集扩展到读取节点 LLC
的至少 4 倍；实际值同时受 profile 上限与 `MemAvailable` 安全预算约束。
`--memory-mib` 是每个 STREAM 数组的显式大小；Triad 峰值驻留约为该值的 3 倍。
若显式值或安全预算不足，原始值仍会保留在曲线和 CSV/JSON 中，但不会进入 DRAM
或互联带宽摘要。

读取 kernel 使用 x86/C86 AVX2/SSE2 或 ARM64 NEON 手写向量汇编。匿名内存仍是普通
write-back 映射；`MOVNTDQA`、`PREFETCHNTA`、ARM `LDNP` 等指令只是特定内存类型或
缓存策略下的提示，不能在用户态保证绕过 LLC。因此工具采用“大工作集 + 汇编流式
加载 + SMBIOS/手动理论上界”三重证据，而不把 non-temporal hint 宣称为硬件旁路。

## 当前测量面

- 平台身份：vendor/model/family/stepping/microcode、ISA flags、虚拟化、DMI/BIOS、内核。
- 拓扑：可用/在线逻辑 CPU、物理核、SMT、cluster/die/socket、NUMA node、affinity/cgroup 可见范围。
- Cache：L1I/L1D/L2/L3 容量、共享域、line、ways、sets，以及随机依赖加载的经验台阶。
- TLB/页面：基础页 one-access-per-page sweep（页内 offset 轮换以分散 L1D set）、持续容量拐点、page-walk 代价，以及 MADV_NOHUGEPAGE/THP 建议策略对比和实际匿名大页驻留量。
- 内存：单核与聚合 read/write/copy/triad payload，随物理核数的缩放和饱和点。
- 带载延迟：固定核心执行随机依赖加载，同时由其他物理核心逐级施加流式读取压力。
- NUMA/fabric：CPU node × memory node 的随机延迟和聚合读带宽矩阵；同插槽跨 NPS/NUMA 与跨插槽路径分别汇总。
- 一致性：SMT、共享 LLC、同 NUMA 跨 LLC、跨 NUMA、跨 socket 的 cache-line 单向 handoff；false-sharing 间距曲线。
- 核心：NOP 前端流、1/4/8 独立整数加法链、整数与 FP64 加法/乘法依赖和并行链、retired IPC、ops/cycle，以及物理核心/SMT 吞吐与频率扩展。
- 指令侧：W^X 生成式 x86 1/4/8 字节 NOP 或 A64 4 字节 NOP，输出代码足迹对应的输送吞吐曲线。
- 分支：always-taken、alternating、random 模式，以及 BTB 数量/间距、方向历史周期、返回地址栈深度和间接目标数量压力曲线。
- 存储转发：同址同宽、部分覆盖、错位重叠和跨缓存行 store/load 对延迟。
- 乱序窗口：x86/C86 两个 cold miss 之间插入精确静态展开的一微操作指令序列，连续 overlap knee 输出 ROB/调度窗口低置信区间；ARM64 当前明确标记 unavailable。
- 运行环境：时钟源与读取开销、频率 policy/governor、THP/页大小、perf policy、温区、漏洞缓解、块设备和网络接口。

完整清单、方法和可扩展项见 [docs/METRICS.md](docs/METRICS.md)。与
`clamchowder/Microbenchmarks` 固定提交的逐模块审计见
[docs/MICROBENCHMARKS-COVERAGE.md](docs/MICROBENCHMARKS-COVERAGE.md)。

## 如何获得可复现结果

正式测试应在裸机、负载空闲且温度稳定时进行。建议：

1. 固定 BIOS 性能策略与内核启动参数；记录 SMT、NUMA clustering、prefetcher、C-state 设置。
2. 停止调度抖动较大的服务，确保目标 CPU 和内存未被 cgroup 限制。
3. 保持 governor 一致；若比较核心周期，优先允许非特权 `perf_event_open`。
4. 每台机器至少运行 3 次 standard，比较中位数与离散程度，不用单次极值下结论。
5. STREAM 和 NUMA 带宽工作集至少达到对应 LLC 的 4 倍；不足的点只用于缓存诊断，不进入 DRAM/互联摘要。
6. NUMA 测试优先允许 `mbind`；权限不足时工具退化到 pinned first-touch，并在报告警告。

常见权限检查：

```bash
cat /proc/sys/kernel/perf_event_paranoid
cat /sys/kernel/mm/transparent_hugepage/enabled
taskset -pc $$
```

PMU 事件被拆为核心、分支和缓存三个独立小组，并在正式探针前实跑验证。
`perf_event_paranoid`、NMI watchdog 或硬件事件约束阻止某组运行时，其他组与 wall-time、
counter tick、带宽和延迟仍会测量；不可用卡片会直接显示具体失败原因。不要为了跑分
盲目修改生产服务器安全策略。

## 构建与测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -v
```

ARM64 应在 ARM64 目标机上原生构建，以免交叉编译的 libc/sysroot 与目标内核 ABI 不一致。所有架构专用汇编均由 `__x86_64__` / `__aarch64__` 隔离；C86 走 x86-64 路径并单独报告平台 family。

底层实现按探针拆分在 `native/probes/`：每个文件包含一个测量模块并通过统一注册表接入；
通用的计时、拓扑、PMU、内存映射和 JSON 能力位于 `native/runtime.hpp`，指针追逐、
流式访存及生成代码计量位于 `native/support/`。新增探针时只需增加一个探针模块、工厂声明
和注册表项，不需要修改主执行流程。

## 结果边界

- “总带宽”是本进程在其 affinity/NUMA 可见范围内、通过 4× LLC 和配置理论上界校验的最大可持续 payload，不是 DRAM 引脚理论速率。
- “互联带宽”是确认越过读取节点 LLC 的 remote memory 有效读 payload，不包含协议开销，不能直接等价为链路 GT/s 或位宽；同插槽与跨插槽必须分开比较。
- NOP IPC 是前端/退休综合下界，可能由 µop cache、NOP 特殊处理或 retire width 主导。
- adds/cycle 是可用整数执行吞吐下界，不是硬件端口数量的直接读数。
- BTB、历史、RAS、间接目标、ROB、scheduler/LQ/SQ 等曲线拐点是经验代理，不能直接当成硬件条目数。
- 指令侧 GB/s 按生成代码体字节数统计，用于比较代码足迹层级，不是外部总线的物理字节流量。
- ROB proxy 同时受 ROB、scheduler、load queue、物理寄存器、SMT 活跃状态和内存级并行影响，必须按低置信区间理解。
- 虚拟机、容器、SMT 邻居、迁移页、自动 NUMA balancing、频率变化和热节流均可能改变结果。

## 安全性

工具不加载内核模块、不访问 `/dev/mem`、不写 MSR、不改 governor、不改 sysctl。运行过程中会占用 CPU 和较多内存，可能影响同机业务；请在维护窗口使用 standard/deep。
