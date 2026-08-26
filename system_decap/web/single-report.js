import {
  CONFIDENCE_NAMES,
  GROUP_NAMES,
  METRIC_NAMES,
  SUMMARY_METRICS,
  cpuModel,
  estimateMap,
  findEstimate,
  formatDate,
  formatNumber,
  formatValue,
  observations,
  reportTitle,
} from "./model.js";
import { barChart, heatmapCard, lineChart, reportCurve } from "./charts.js";
import { append, button, element, sectionHeader } from "./ui.js";

function metricCard(item, label) {
  const card = element("article", `metric-card confidence-${item?.confidence || "unavailable"}`);
  append(
    card,
    element("span", "metric-label", label),
    element("strong", "", formatValue(item))
  );
  const footer = element("div", "metric-meta");
  append(
    footer,
    element("span", "", `置信度：${CONFIDENCE_NAMES[item?.confidence] || "不可用"}`),
    element("span", "", item?.basis || item?.caveat || "无可用观测")
  );
  card.append(footer);
  return card;
}

function summaryCards(report) {
  const estimates = estimateMap(report);
  const grid = element("div", "metric-grid");
  for (const [key, label] of SUMMARY_METRICS) {
    const item = findEstimate(estimates, key);
    grid.append(metricCard(item, label));
  }
  return grid;
}

function inventoryGrid(report) {
  const system = report.system || {};
  const topology = system.topology || {};
  const theoretical = system.memory_bandwidth_theoretical || {};
  const rows = [
    ["平台", system.platform_family || system.architecture || "—"],
    ["内核", system.kernel || "—"],
    ["物理核心", formatNumber(topology.physical_cores, 0)],
    ["逻辑 CPU", formatNumber(topology.logical_cpus, 0)],
    ["插槽 / Die", `${topology.sockets ?? "—"} / ${topology.dies ?? "—"}`],
    ["NUMA 节点", formatNumber(topology.numa_nodes, 0)],
    ["内存设备", formatNumber((system.memory_devices || []).length, 0)],
    ["配置理论带宽", theoretical.upper_bound_gbps
      ? `${formatNumber(theoretical.upper_bound_gbps)} GB/s`
      : "未识别"],
  ];
  const grid = element("dl", "inventory-grid");
  for (const [label, value] of rows) {
    append(grid, element("div", "", null));
    const item = grid.lastElementChild;
    append(item, element("dt", "", label), element("dd", "", value));
  }
  return grid;
}


function reportCharts(report) {
  const grid = element("div", "chart-grid");
  const pagePolicyRows = observations(report, "page_policy", "random_load_latency").map((item) => ({
    label: item.labels?.policy || "unknown",
    value: item.value,
    unit: item.unit,
  }));
  const forwardingRows = observations(report, "store_forwarding", "store_load_latency").map((item) => ({
    label: item.labels?.case || "unknown",
    value: item.value,
    unit: item.unit,
  }));
  const pipelineRows = observations(report, "pipeline", "operations_per_cycle").map((item) => ({
    label: item.labels?.kernel || "unknown",
    value: item.value,
    unit: item.unit,
  }));
  const branchRows = observations(report, "branch", "time_per_branch").map((item) => ({
    label: item.labels?.pattern || "unknown",
    value: item.value,
    unit: item.unit,
  }));
  const relationValues = new Map();
  observations(report, "core_latency", "cacheline_handoff_latency").forEach((item) => {
    const relation = item.labels?.relation || "unknown";
    if (!relationValues.has(relation)) relationValues.set(relation, []);
    relationValues.get(relation).push(Number(item.value));
  });
  const relationRows = [...relationValues.entries()].map(([label, values]) => ({
    label,
    value: values.reduce((sum, value) => sum + value, 0) / values.length,
    unit: "ns/one-way",
  }));
  const btbRows = observations(report, "branch_structure", "btb_branch_latency");
  const customCurve = (configuration, rows, xLabel, series) => lineChart({
    ...configuration,
    points: rows.map((item) => ({
      x: Number(item.labels?.[xLabel]),
      y: Number(item.value),
      unit: item.unit,
      series: series(item),
    })),
  });
  append(
    grid,
    reportCurve(report, {
      title: "缓存层级随机依赖加载延迟",
      note: "工作集跨越 L1、L2、LLC 与 DRAM 时的经验台阶",
      group: "cache_latency",
      metric: "random_load_latency",
      xLabel: "working_set_bytes",
      xMode: "bytes",
      xTitle: "工作集",
      yTitle: "ns/access",
      logX: true,
      className: "wide",
    }),
    reportCurve(report, {
      title: "TLB 容量与页表遍历延迟",
      note: "每个基础页一次随机依赖访问",
      group: "tlb_latency",
      metric: "page_random_load_latency",
      xLabel: "pages",
      xMode: "integer",
      xTitle: "基础页数量",
      yTitle: "ns/access",
      logX: true,
    }),
    reportCurve(report, {
      title: "访问步长与硬件预取敏感度",
      note: "只统计请求的 8 字节加载；大步长会降低缓存行利用率",
      group: "memory_access",
      metric: "stride_access_rate",
      xLabel: "stride_bytes",
      xMode: "bytes",
      xTitle: "访问步长",
      yTitle: "Gaccess/s",
      logX: true,
    }),
    reportCurve(report, {
      title: "内存级并行度（MLP）",
      note: "多条相互独立、链内依赖的随机访问链",
      group: "memory_parallelism",
      metric: "random_load_rate",
      xLabel: "chains",
      xMode: "integer",
      xTitle: "独立链数量",
      yTitle: "Gaccess/s",
      logX: true,
    }),
    barChart({
      title: "基础页与透明大页策略对比",
      note: "madvise 只是建议，实际匿名大页驻留量保留在原始标签中",
      rows: pagePolicyRows,
    }),
    barChart({
      title: "Store-to-load forwarding 对齐代价",
      note: "同址同宽为基线；部分覆盖与跨行暴露转发失败路径",
      rows: forwardingRows,
    }),
    reportCurve(report, {
      title: "单核缓存工作集带宽",
      note: "小工作集反映缓存层级吞吐，不作为 DRAM 带宽",
      group: "cache_bandwidth",
      metric: "working_set_bandwidth",
      xLabel: "working_set_bytes",
      seriesLabel: "operation",
      xMode: "bytes",
      xTitle: "工作集",
      yTitle: "GB/s",
      logX: true,
    }),
    reportCurve(report, {
      title: "内存带宽随核心数扩展",
      note: "固定物理核心的流式有效载荷",
      group: "memory_bandwidth",
      metric: "stream_bandwidth",
      xLabel: "threads",
      seriesLabel: "operation",
      xMode: "integer",
      xTitle: "线程数",
      yTitle: "GB/s",
      logX: true,
      className: "wide",
    }),
    reportCurve(report, {
      title: "带宽压力下的随机内存延迟",
      note: "读取压力逐级增加时的依赖加载延迟",
      group: "loaded_memory_latency",
      metric: "random_load_latency_under_load",
      xLabel: "load_threads",
      xMode: "integer",
      xTitle: "压力线程数",
      yTitle: "ns/access",
    }),
    reportCurve(report, {
      title: "指令侧代码输送吞吐与足迹",
      note: "W^X 生成式 NOP 代码体重复扫描",
      group: "instruction_fetch",
      metric: "code_delivery_bandwidth",
      xLabel: "working_set_bytes",
      seriesLabel: "instruction_bytes",
      xMode: "bytes",
      xTitle: "代码足迹",
      yTitle: "GB/s",
      logX: true,
    }),
    reportCurve(report, {
      title: "整数微内核多核与 SMT 扩展",
      note: "固定线程的独立整数加法链",
      group: "compute_scaling",
      metric: "integer_add_throughput",
      xLabel: "threads",
      seriesLabel: "scope",
      xMode: "integer",
      xTitle: "线程数",
      yTitle: "Gop/s",
      logX: true,
      className: "wide",
    }),
    reportCurve(report, {
      title: "伪共享与一致性行边界",
      note: "两个核心更新不同原子变量，吞吐恢复点通常对应一致性行边界",
      group: "coherence",
      metric: "atomic_update_rate",
      xLabel: "separation_bytes",
      xMode: "bytes",
      xTitle: "变量间距",
      yTitle: "Mop/s",
      logX: true,
    }),
    barChart({
      title: "标量微内核执行吞吐",
      note: "依赖链暴露延迟约束，独立链给出后端吞吐下界",
      rows: pipelineRows,
      className: "wide",
    }),
    barChart({
      title: "不同分支模式的代价",
      note: "随机模式与可预测模式的差值近似误预测恢复代价",
      rows: branchRows,
    }),
    barChart({
      title: "核间传递延迟（按拓扑分类）",
      note: "缓存行单向传递延迟按 relation 求均值",
      rows: relationRows,
    }),
    reportCurve(report, {
      title: "乱序窗口重叠代理曲线",
      note: "连续台阶表示第二次缓存缺失不再与第一次重叠",
      group: "reorder_window",
      metric: "cold_load_overlap_penalty",
      xLabel: "filler_instructions",
      xMode: "integer",
      xTitle: "静态填充指令",
      yTitle: "counter-ticks",
    }),
    customCurve({
      title: "BTB 无条件跳转足迹压力",
      note: "分支数量和地址间距独立变化",
      xMode: "integer",
      xTitle: "分支数量",
      yTitle: "ns/branch",
      logX: true,
    }, btbRows.filter((item) => item.labels?.branch_type === "unconditional"),
    "branch_count", (item) => `间距 ${item.labels?.spacing_bytes}B`),
    customCurve({
      title: "BTB 始终跳转条件分支压力",
      note: "与无条件跳转使用相同数量和间距扫描",
      xMode: "integer",
      xTitle: "分支数量",
      yTitle: "ns/branch",
      logX: true,
    }, btbRows.filter((item) => item.labels?.branch_type === "conditional-taken"),
    "branch_count", (item) => `间距 ${item.labels?.spacing_bytes}B`),
    reportCurve(report, {
      title: "方向预测器历史周期压力",
      note: "重复伪随机方向周期越长，需要记忆的序列越复杂",
      group: "branch_structure",
      metric: "history_period_latency",
      xLabel: "history_period",
      xMode: "integer",
      xTitle: "历史周期",
      yTitle: "ns/branch",
      logX: true,
    }),
    reportCurve(report, {
      title: "返回地址栈（RAS）深度压力",
      note: "每层调用拥有不同返回地址",
      group: "branch_structure",
      metric: "return_stack_latency",
      xLabel: "depth",
      xMode: "integer",
      xTitle: "嵌套深度",
      yTitle: "ns/return",
      logX: true,
    }),
    reportCurve(report, {
      title: "间接分支目标数量压力",
      note: "一个间接调用点按打乱顺序切换目标",
      group: "branch_structure",
      metric: "indirect_call_latency",
      xLabel: "target_count",
      xMode: "integer",
      xTitle: "目标数量",
      yTitle: "ns/call",
      logX: true,
    })
  );
  return grid;
}

function matrixSection(report) {
  const grid = element("div", "chart-grid");
  const coreRows = observations(
    report,
    "core_latency",
    "cacheline_handoff_latency"
  );
  const numaLatency = observations(report, "numa", "load_latency");
  const numaBandwidth = observations(report, "numa", "read_bandwidth");
  append(
    grid,
    heatmapCard({
      title: "核间延迟矩阵（CPU × CPU）",
      note: "缓存行 release/acquire 乒乓的单向延迟；拖动滚动、工具栏缩放",
      rows: coreRows,
      rowLabel: "cpu_a",
      columnLabel: "cpu_b",
      symmetric: true,
      unit: "ns",
    }),
    heatmapCard({
      title: "NUMA 随机读取延迟",
      note: "读取节点 × 内存节点",
      rows: numaLatency,
      rowLabel: "cpu_node",
      columnLabel: "memory_node",
      unit: "ns/access",
    }),
    heatmapCard({
      title: "NUMA 读取带宽",
      note: "读取节点 × 内存节点的有效载荷",
      rows: numaBandwidth,
      rowLabel: "cpu_node",
      columnLabel: "memory_node",
      unit: "GB/s",
    })
  );
  return grid;
}

function rawObservations(report) {
  const section = element("div", "raw-ledger");
  const tools = element("div", "raw-tools");
  const search = document.createElement("input");
  search.type = "search";
  search.placeholder = "筛选分组、指标、标签或方法";
  search.setAttribute("aria-label", "筛选原始观测");
  const coreToggleLabel = element("label", "toggle");
  const coreToggle = document.createElement("input");
  coreToggle.type = "checkbox";
  append(coreToggleLabel, coreToggle, document.createTextNode("显示核间延迟明细"));
  const status = element("output", "raw-status");
  append(tools, search, coreToggleLabel, status);
  const scroll = element("div", "table-scroll");
  const table = element("table", "data-table");
  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  ["分组", "指标 / 方法", "数值", "置信度", "标签"].forEach(
    (label) => headRow.append(element("th", "", label))
  );
  thead.append(headRow);
  const tbody = document.createElement("tbody");
  table.append(thead, tbody);
  scroll.append(table);
  const more = button("继续加载", "load-more");
  append(section, tools, scroll, more);

  const all = report.observations || [];
  let limit = 200;
  function render() {
    const query = search.value.trim().toLowerCase();
    const filtered = all.filter((item) => {
      if (item.group === "core_latency" && !coreToggle.checked) return false;
      return !query || JSON.stringify(item).toLowerCase().includes(query);
    });
    const visible = filtered.slice(0, limit);
    tbody.replaceChildren();
    for (const item of visible) {
      const row = document.createElement("tr");
      const labels = Object.entries(item.labels || {})
        .map(([key, value]) => `${key}=${value}`)
        .join(" · ");
      const metricCell = document.createElement("td");
      append(
        metricCell,
        element("strong", "", METRIC_NAMES[item.metric] || item.metric),
        element("small", "", item.method || "")
      );
      [
        element("td", "", GROUP_NAMES[item.group] || item.group),
        metricCell,
        element("td", "numeric", `${formatNumber(item.value)} ${item.unit || ""}`),
        element("td", `confidence-text confidence-${item.confidence}`,
          CONFIDENCE_NAMES[item.confidence] || item.confidence),
        element("td", "labels", labels),
      ].forEach((cell) => row.append(cell));
      tbody.append(row);
    }
    status.value = `显示 ${visible.length} / 匹配 ${filtered.length} / 总计 ${all.length}`;
    more.hidden = visible.length >= filtered.length;
  }
  search.addEventListener("input", () => { limit = 200; render(); });
  coreToggle.addEventListener("change", () => { limit = 200; render(); });
  more.addEventListener("click", () => { limit += 500; render(); });
  render();
  return section;
}

function warningPanel(report) {
  const panel = element("div", "warning-panel");
  const warnings = report.warnings || [];
  panel.append(element("h3", "", warnings.length ? `运行警告 · ${warnings.length}` : "运行警告 · 0"));
  if (!warnings.length) {
    panel.append(element("p", "", "本次运行没有记录警告。"));
    return panel;
  }
  const list = document.createElement("ul");
  warnings.forEach((warning) => list.append(element("li", "", warning)));
  panel.append(list);
  return panel;
}

function renderSingle(report, catalogItem, mount = document.querySelector("#app")) {
  const fragment = document.createDocumentFragment();
  const hero = element("section", "report-hero");
  const identity = element("div");
  append(
    identity,
    element("span", "overline", "SINGLE REPORT / JSON SOURCE"),
    element("h1", "", reportTitle(report, catalogItem)),
    element("p", "hero-model", cpuModel(report))
  );
  const stamp = element("div", "hero-stamp");
  append(
    stamp,
    element("strong", "", String(report.run?.profile || "unknown").toUpperCase()),
    element("span", "", formatDate(report.run?.started_at)),
    element("span", "", `${(report.observations || []).length} 个观测 · ${(report.estimates || []).length} 个推断`)
  );
  append(hero, identity, stamp);
  fragment.append(hero);

  const summary = element("section", "report-section");
  append(
    summary,
    sectionHeader(1, "FINGERPRINT", "核心摘要", "用于快速判断平台轮廓；卡片保留置信度、推断依据与不可用原因。"),
    summaryCards(report)
  );
  fragment.append(summary);

  const inventory = element("section", "report-section");
  append(
    inventory,
    sectionHeader(2, "PLACEMENT MAP", "系统与拓扑", "浏览器直接读取报告 JSON 中的静态清点结果。"),
    inventoryGrid(report)
  );
  fragment.append(inventory);

  const curves = element("section", "report-section");
  append(
    curves,
    sectionHeader(3, "CURVES", "层级与吞吐曲线", "曲线由前端从原始 observations 即时构建，不再预先写入 HTML。"),
    reportCharts(report)
  );
  fragment.append(curves);

  const matrices = element("section", "report-section");
  append(
    matrices,
    sectionHeader(4, "FABRIC", "NUMA 与一致性矩阵", "Canvas 渲染可承载 256 核及更大矩阵，并支持缩放与滚动检查。"),
    matrixSection(report)
  );
  fragment.append(matrices);

  const raw = element("section", "report-section");
  append(
    raw,
    sectionHeader(5, "EVIDENCE LEDGER", "原始观测数据", "核间延迟明细默认隐藏；筛选和分页全部在浏览器本地完成。"),
    rawObservations(report)
  );
  fragment.append(raw);

  const caveats = element("section", "report-section");
  append(
    caveats,
    sectionHeader(6, "BOUNDARIES", "警告与复现条件", "黑盒结果受权限、频率、放置策略、虚拟化与同时运行负载影响。"),
    warningPanel(report)
  );
  const details = document.createElement("details");
  details.className = "json-details";
  details.append(element("summary", "", "运行元数据"));
  details.append(element("pre", "", JSON.stringify({
    run: report.run,
    tool: report.tool,
    native: report.native_metadata,
  }, null, 2)));
  caveats.append(details);
  fragment.append(caveats);

  mount.replaceChildren(fragment);
  mount.focus({ preventScroll: true });
}


export { renderSingle, warningPanel };
