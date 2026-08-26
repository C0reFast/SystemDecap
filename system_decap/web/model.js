const REPORT_COLORS = [
  "#c8ff3d", "#45d7ff", "#ff8a5c", "#f7d35c",
  "#b994ff", "#ff6ea9", "#70e1a1", "#ff5d5d",
];

const SUMMARY_METRICS = [
  ["memory.aggregate_read_bandwidth", "系统聚合读取带宽"],
  ["memory.single_core_read_bandwidth", "单核读取带宽"],
  ["memory.random_latency", "DRAM 随机延迟"],
  ["core.max_observed_ipc", "最大实测 IPC"],
  ["core.frontend_width_lower_bound", "前端宽度下界"],
  ["core.rob_capacity_proxy", "ROB 窗口代理值"],
  ["numa.same_socket_remote_latency", "同插槽跨 NUMA 延迟"],
  ["numa.same_socket_remote_payload_bandwidth", "同插槽跨 NUMA 带宽"],
  ["numa.cross_socket_latency", "跨插槽延迟"],
  ["numa.cross_socket_payload_bandwidth", "跨插槽有效载荷带宽"],
  ["core.integer_add_lanes_lower_bound", "整数加法吞吐"],
  ["coherence.same-llc-different-core", "共享 LLC 核间传递"],
  ["tlb.first_knee", "首个 TLB 拐点"],
  ["branch.unpredictable_penalty", "随机分支代价"],
];

const GROUP_NAMES = {
  timer: "计时器",
  cache_latency: "缓存延迟",
  tlb_latency: "TLB 延迟",
  memory_bandwidth: "内存带宽",
  cache_bandwidth: "缓存带宽",
  memory_access: "内存访问",
  memory_parallelism: "内存级并行",
  page_policy: "页面策略",
  loaded_memory_latency: "带载内存延迟",
  store_forwarding: "存储转发",
  instruction_fetch: "指令侧",
  core_latency: "核间延迟",
  coherence: "一致性",
  numa: "NUMA",
  pipeline: "核心流水线",
  branch: "分支预测",
  reorder_window: "乱序窗口",
  compute_scaling: "计算扩展",
  branch_structure: "分支结构",
  os_overhead: "操作系统",
};

const METRIC_NAMES = {
  random_load_latency: "随机依赖加载延迟",
  page_random_load_latency: "逐页随机加载延迟",
  stream_bandwidth: "流式有效载荷带宽",
  working_set_bandwidth: "工作集带宽",
  random_load_latency_under_load: "带载随机加载延迟",
  concurrent_read_bandwidth: "并发读取带宽",
  code_delivery_bandwidth: "代码输送吞吐",
  integer_add_throughput: "整数加法聚合吞吐",
  operation_throughput: "微内核操作吞吐",
  time_per_branch: "单分支耗时",
  cacheline_handoff_latency: "缓存行核间传递延迟",
  load_latency: "加载延迟",
  read_bandwidth: "读取带宽",
};

const OPERATION_NAMES = {
  read: "读取",
  write: "写入",
  copy: "复制",
  triad: "三元运算",
};

const CONFIDENCE_NAMES = {
  high: "高",
  medium: "中",
  low: "低",
  unavailable: "不可用",
};

const ESTIMATE_ALIASES = {
  "numa.same_socket_remote_latency": ["numa.remote_latency"],
  "numa.same_socket_remote_payload_bandwidth": ["numa.interconnect_payload_bandwidth"],
  "coherence.same-llc-different-core": ["coherence.same-numa-different-core"],
};

function formatNumber(value, maximumFractionDigits = 2) {
  const number = Number(value);
  if (!Number.isFinite(number)) return "—";
  return new Intl.NumberFormat("zh-CN", { maximumFractionDigits }).format(number);
}

function formatValue(item) {
  if (!item || item.available === false || item.value === null || item.value === undefined) {
    return "不可用";
  }
  return `${formatNumber(item.value)}${item.unit ? ` ${item.unit}` : ""}`;
}

function formatBytes(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return String(value ?? "—");
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let scaled = number;
  let index = 0;
  while (scaled >= 1024 && index < units.length - 1) {
    scaled /= 1024;
    index += 1;
  }
  return `${formatNumber(scaled, scaled < 10 ? 2 : 1)} ${units[index]}`;
}

function formatDate(value) {
  if (!value) return "时间未知";
  const date = new Date(value);
  return Number.isNaN(date.getTime())
    ? String(value)
    : new Intl.DateTimeFormat("zh-CN", {
        dateStyle: "medium",
        timeStyle: "short",
      }).format(date);
}

function reportTitle(report, catalogItem = {}) {
  const system = report.system || {};
  return catalogItem.hostname || system.hostname || "未命名报告";
}

function comparisonLabel(report, catalogItem = {}) {
  return [
    reportTitle(report, catalogItem),
    String(report.run?.profile || "unknown").toUpperCase(),
    formatDate(report.run?.started_at),
  ].join(" · ");
}

function cpuModel(report) {
  return report.system?.cpu?.model || "处理器型号未知";
}

function estimateMap(report) {
  return new Map((report.estimates || []).map((item) => [item.key, item]));
}

function findEstimate(estimates, key) {
  if (estimates.has(key)) return estimates.get(key);
  for (const alias of ESTIMATE_ALIASES[key] || []) {
    if (estimates.has(alias)) return estimates.get(alias);
  }
  return undefined;
}

function observations(report, group, metric, filter = () => true) {
  return (report.observations || []).filter(
    (item) => item.group === group && item.metric === metric && filter(item)
  );
}


export {
  REPORT_COLORS,
  SUMMARY_METRICS,
  GROUP_NAMES,
  METRIC_NAMES,
  OPERATION_NAMES,
  CONFIDENCE_NAMES,
  formatNumber,
  formatValue,
  formatBytes,
  formatDate,
  reportTitle,
  comparisonLabel,
  cpuModel,
  estimateMap,
  findEstimate,
  observations,
};
