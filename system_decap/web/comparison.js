import {
  REPORT_COLORS,
  SUMMARY_METRICS,
  comparisonLabel,
  cpuModel,
  estimateMap,
  findEstimate,
  formatDate,
  formatNumber,
  formatValue,
  observations,
  reportTitle,
} from "./model.js";
import { lineChart } from "./charts.js";
import { warningPanel } from "./single-report.js";
import { append, button, element, sectionHeader } from "./ui.js";

function comparisonPointSeries(reports, catalogItems, definition) {
  const points = [];
  reports.forEach((report, index) => {
    const rows = observations(
      report,
      definition.group,
      definition.metric,
      definition.filter || (() => true)
    );
    for (const item of rows) {
      points.push({
        x: Number(item.labels?.[definition.xLabel]),
        y: Number(item.value),
        unit: item.unit,
        series: comparisonLabel(report, catalogItems[index]),
        color: REPORT_COLORS[index % REPORT_COLORS.length],
      });
    }
  });
  return points;
}

function comparisonTable(reports, catalogItems) {
  const maps = reports.map(estimateMap);
  const card = element("div", "table-scroll comparison-table-wrap");
  const table = element("table", "data-table comparison-table");
  const thead = document.createElement("thead");
  const heading = document.createElement("tr");
  heading.append(element("th", "", "指标"));
  reports.forEach((report, index) => {
    const cell = document.createElement("th");
    const marker = element("i", "report-color");
    marker.style.background = REPORT_COLORS[index % REPORT_COLORS.length];
    append(cell, marker, document.createTextNode(comparisonLabel(report, catalogItems[index])));
    if (index === 0) cell.append(element("small", "", "基线"));
    heading.append(cell);
  });
  thead.append(heading);
  const tbody = document.createElement("tbody");
  for (const [key, label] of SUMMARY_METRICS) {
    const row = document.createElement("tr");
    row.append(element("th", "", label));
    const baseline = findEstimate(maps[0], key);
    reports.forEach((_, index) => {
      const item = findEstimate(maps[index], key);
      const cell = document.createElement("td");
      cell.append(element("strong", "", formatValue(item)));
      if (
        index > 0
        && item?.available !== false
        && baseline?.available !== false
        && Number.isFinite(Number(item?.value))
        && Number.isFinite(Number(baseline?.value))
        && Number(baseline.value) !== 0
      ) {
        const delta = ((Number(item.value) - Number(baseline.value)) / Math.abs(Number(baseline.value))) * 100;
        cell.append(element(
          "small",
          delta >= 0 ? "delta positive" : "delta negative",
          `${delta >= 0 ? "+" : ""}${formatNumber(delta, 1)}% 相对基线`
        ));
      } else if (index > 0) {
        cell.append(element("small", "delta", "无法计算差值"));
      }
      row.append(cell);
    });
    tbody.append(row);
  }
  table.append(thead, tbody);
  card.append(table);
  return card;
}

function topologyComparison(reports, catalogItems) {
  const rows = [
    ["处理器", (report) => cpuModel(report)],
    ["平台", (report) => report.system?.platform_family || "—"],
    ["Profile", (report) => report.run?.profile || "—"],
    ["物理核心", (report) => report.system?.topology?.physical_cores ?? "—"],
    ["逻辑 CPU", (report) => report.system?.topology?.logical_cpus ?? "—"],
    ["NUMA 节点", (report) => report.system?.topology?.numa_nodes ?? "—"],
    ["运行时间", (report) => formatDate(report.run?.started_at)],
    ["观测点", (report) => (report.observations || []).length],
  ];
  const wrap = element("div", "table-scroll comparison-table-wrap");
  const table = element("table", "data-table comparison-table");
  const head = document.createElement("tr");
  head.append(element("th", "", "系统属性"));
  catalogItems.forEach((item, index) => {
    const th = document.createElement("th");
    const marker = element("i", "report-color");
    marker.style.background = REPORT_COLORS[index % REPORT_COLORS.length];
    append(th, marker, document.createTextNode(comparisonLabel(reports[index], item)));
    head.append(th);
  });
  const thead = document.createElement("thead");
  thead.append(head);
  const tbody = document.createElement("tbody");
  for (const [label, getter] of rows) {
    const row = document.createElement("tr");
    row.append(element("th", "", label));
    reports.forEach((report) => row.append(element("td", "", getter(report))));
    tbody.append(row);
  }
  table.append(thead, tbody);
  wrap.append(table);
  return wrap;
}

function downloadComparison(reports, catalogItems) {
  const keys = [...new Set(reports.flatMap((report) => (report.estimates || []).map((item) => item.key)))];
  const quote = (value) => `"${String(value ?? "").replaceAll('"', '""')}"`;
  const lines = [
    ["key", "metric", ...catalogItems.map((item) => item.hostname || item.name)].map(quote).join(","),
  ];
  for (const key of keys) {
    const items = reports.map((report) => estimateMap(report).get(key));
    lines.push([
      key,
      items.find(Boolean)?.name || key,
      ...items.map((item) => item?.available === false ? "" : item?.value ?? ""),
    ].map(quote).join(","));
  }
  const blob = new Blob(["\ufeff" + lines.join("\n")], { type: "text/csv;charset=utf-8" });
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = `system-decap-comparison-${new Date().toISOString().slice(0, 10)}.csv`;
  link.click();
  URL.revokeObjectURL(link.href);
}

function renderComparison(
  reports,
  catalogItems,
  mount = document.querySelector("#app")
) {
  const fragment = document.createDocumentFragment();
  const hero = element("section", "report-hero comparison-hero");
  const identity = element("div");
  append(
    identity,
    element("span", "overline", "MULTI-REPORT / RELATIVE BASELINE"),
    element("h1", "", `${reports.length} 份报告对比`),
    element("p", "hero-model", "第一列作为相对差值基线；差值仅表示方向，不自动判断优劣。")
  );
  const actions = element("div", "hero-actions");
  const exportButton = button("导出对比 CSV", "primary-button");
  exportButton.addEventListener("click", () => downloadComparison(reports, catalogItems));
  actions.append(exportButton);
  append(hero, identity, actions);
  fragment.append(hero);

  const strip = element("div", "compare-strip");
  reports.forEach((report, index) => {
    const item = element("article");
    item.style.setProperty("--report-color", REPORT_COLORS[index % REPORT_COLORS.length]);
    append(
      item,
      element("span", "compare-index", index === 0 ? "BASE" : `R${index + 1}`),
      element("strong", "", reportTitle(report, catalogItems[index])),
      element("small", "", cpuModel(report)),
      element("small", "", formatDate(report.run?.started_at))
    );
    strip.append(item);
  });
  fragment.append(strip);

  const summary = element("section", "report-section");
  append(
    summary,
    sectionHeader(1, "DELTA LEDGER", "核心指标横向对比", "数值来自每份 JSON 的 estimates；百分比相对第一份报告计算。"),
    comparisonTable(reports, catalogItems)
  );
  fragment.append(summary);

  const topology = element("section", "report-section");
  append(
    topology,
    sectionHeader(2, "CONTEXT", "平台条件对照", "先确认核心数、NUMA、Profile 和测试时间等实验条件是否可比。"),
    topologyComparison(reports, catalogItems)
  );
  fragment.append(topology);

  const curves = element("section", "report-section");
  const chartGrid = element("div", "chart-grid");
  const definitions = [
    {
      title: "缓存延迟层级叠加",
      note: "相同工作集下比较随机依赖加载延迟",
      group: "cache_latency",
      metric: "random_load_latency",
      xLabel: "working_set_bytes",
      xMode: "bytes",
      xTitle: "工作集",
      yTitle: "ns/access",
      logX: true,
    },
    {
      title: "聚合读取带宽扩展",
      note: "仅比较 operation=read 的线程扩展曲线",
      group: "memory_bandwidth",
      metric: "stream_bandwidth",
      xLabel: "threads",
      filter: (item) => item.labels?.operation === "read",
      xMode: "integer",
      xTitle: "线程数",
      yTitle: "GB/s",
      logX: true,
    },
    {
      title: "带载内存延迟",
      note: "压力线程增加时的随机依赖加载延迟",
      group: "loaded_memory_latency",
      metric: "random_load_latency_under_load",
      xLabel: "load_threads",
      xMode: "integer",
      xTitle: "压力线程数",
      yTitle: "ns/access",
    },
    {
      title: "整数计算扩展",
      note: "物理核心整数加法聚合吞吐",
      group: "compute_scaling",
      metric: "integer_add_throughput",
      xLabel: "threads",
      filter: (item) => item.labels?.scope === "physical-cores",
      xMode: "integer",
      xTitle: "线程数",
      yTitle: "Gop/s",
      logX: true,
    },
  ];
  definitions.forEach((definition) => {
    chartGrid.append(lineChart({
      ...definition,
      points: comparisonPointSeries(reports, catalogItems, definition),
      className: "wide",
    }));
  });
  append(
    curves,
    sectionHeader(3, "OVERLAY", "原始曲线叠加", "每条线都从对应报告 observations 动态生成；缺失曲线不会被补零。"),
    chartGrid
  );
  fragment.append(curves);

  const warnings = element("section", "report-section");
  const warningGrid = element("div", "warning-grid");
  reports.forEach((report, index) => {
    const panel = warningPanel(report);
    panel.style.setProperty("--report-color", REPORT_COLORS[index % REPORT_COLORS.length]);
    panel.prepend(element("span", "warning-owner", comparisonLabel(report, catalogItems[index])));
    warningGrid.append(panel);
  });
  append(
    warnings,
    sectionHeader(4, "CAVEATS", "各报告警告", "对比前应逐份检查权限、内存放置和 PMU 可用性。"),
    warningGrid
  );
  fragment.append(warnings);

  mount.replaceChildren(fragment);
  mount.focus({ preventScroll: true });
}


export { renderComparison };
