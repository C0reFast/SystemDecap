import {
  OPERATION_NAMES,
  REPORT_COLORS,
  formatBytes,
  formatNumber,
  observations,
} from "./model.js";
import { append, button, element, emptyCard } from "./ui.js";

function svgNode(name, attributes = {}) {
  const node = document.createElementNS("http://www.w3.org/2000/svg", name);
  for (const [key, value] of Object.entries(attributes)) {
    node.setAttribute(key, String(value));
  }
  return node;
}

function axisLabel(value, mode) {
  if (mode === "bytes") return formatBytes(value);
  if (mode === "integer") return formatNumber(value, 0);
  return formatNumber(value, 2);
}

function lineChart({
  title,
  note,
  points,
  xMode = "number",
  xTitle = "",
  yTitle = "",
  logX = false,
  className = "",
}) {
  const usable = points.filter(
    (point) => Number.isFinite(Number(point.x)) && Number.isFinite(Number(point.y))
  );
  if (!usable.length) return emptyCard(title, "本报告没有可用于该图的观测点。");

  const card = element("article", `chart-card ${className}`.trim());
  const head = element("div", "chart-head");
  const headText = element("div");
  append(headText, element("h3", "", title), element("p", "", note));
  append(head, headText);
  card.append(head);

  const width = 820;
  const height = 340;
  const margin = { top: 26, right: 26, bottom: 62, left: 78 };
  const innerWidth = width - margin.left - margin.right;
  const innerHeight = height - margin.top - margin.bottom;
  const xs = usable.map((point) => Number(point.x));
  const ys = usable.map((point) => Number(point.y));
  const transformX = (value) => logX ? Math.log2(Math.max(Number(value), 1)) : Number(value);
  const transformedXs = xs.map(transformX);
  let xMin = Math.min(...transformedXs);
  let xMax = Math.max(...transformedXs);
  let yMin = Math.min(0, ...ys);
  let yMax = Math.max(...ys);
  if (xMin === xMax) { xMin -= 0.5; xMax += 0.5; }
  if (yMin === yMax) { yMin -= 0.5; yMax += 0.5; }
  const xScale = (value) => margin.left
    + ((transformX(value) - xMin) / (xMax - xMin)) * innerWidth;
  const yScale = (value) => margin.top
    + innerHeight - ((Number(value) - yMin) / (yMax - yMin)) * innerHeight;

  const svg = svgNode("svg", {
    class: "line-chart",
    viewBox: `0 0 ${width} ${height}`,
    role: "img",
    "aria-label": title,
  });

  for (let tick = 0; tick <= 5; tick += 1) {
    const yValue = yMin + ((yMax - yMin) * tick) / 5;
    const y = yScale(yValue);
    svg.append(svgNode("line", {
      x1: margin.left, x2: width - margin.right, y1: y, y2: y, class: "grid-line",
    }));
    const label = svgNode("text", {
      x: margin.left - 12, y: y + 4, "text-anchor": "end", class: "axis-text",
    });
    label.textContent = formatNumber(yValue, 2);
    svg.append(label);
  }

  for (let tick = 0; tick <= 4; tick += 1) {
    const transformed = xMin + ((xMax - xMin) * tick) / 4;
    const raw = logX ? 2 ** transformed : transformed;
    const x = margin.left + (innerWidth * tick) / 4;
    const label = svgNode("text", {
      x, y: height - margin.bottom + 24, "text-anchor": "middle", class: "axis-text",
    });
    label.textContent = axisLabel(raw, xMode);
    svg.append(label);
  }

  const series = new Map();
  for (const point of usable) {
    const name = point.series || "观测";
    if (!series.has(name)) series.set(name, []);
    series.get(name).push(point);
  }
  [...series.entries()].forEach(([name, values], seriesIndex) => {
    const color = values[0]?.color || REPORT_COLORS[seriesIndex % REPORT_COLORS.length];
    values.sort((left, right) => Number(left.x) - Number(right.x));
    const polyline = svgNode("polyline", {
      points: values.map((point) => `${xScale(point.x)},${yScale(point.y)}`).join(" "),
      fill: "none",
      stroke: color,
      "stroke-width": 2.4,
      "stroke-linejoin": "round",
      "stroke-linecap": "round",
      class: "series-line",
    });
    svg.append(polyline);
    for (const point of values) {
      const circle = svgNode("circle", {
        cx: xScale(point.x),
        cy: yScale(point.y),
        r: 3.4,
        fill: color,
        class: "chart-point",
        tabindex: 0,
      });
      const tooltip = svgNode("title");
      tooltip.textContent = `${name} · ${axisLabel(point.x, xMode)} · ${formatNumber(point.y)} ${point.unit || yTitle}`;
      circle.append(tooltip);
      svg.append(circle);
    }
  });

  const xAxisTitle = svgNode("text", {
    x: margin.left + innerWidth / 2,
    y: height - 8,
    "text-anchor": "middle",
    class: "axis-title",
  });
  xAxisTitle.textContent = xTitle;
  const yAxisTitle = svgNode("text", {
    x: 16,
    y: margin.top + innerHeight / 2,
    "text-anchor": "middle",
    transform: `rotate(-90 16 ${margin.top + innerHeight / 2})`,
    class: "axis-title",
  });
  yAxisTitle.textContent = yTitle;
  svg.append(xAxisTitle, yAxisTitle);
  card.append(svg);

  const legend = element("div", "chart-legend");
  [...series.entries()].forEach(([name, values], index) => {
    const item = element("span");
    const swatch = element("i");
    swatch.style.background = values[0]?.color || REPORT_COLORS[index % REPORT_COLORS.length];
    append(item, swatch, document.createTextNode(name));
    legend.append(item);
  });
  card.append(legend);
  return card;
}

function barChart({ title, note, rows, className = "" }) {
  const usable = rows.filter((row) => Number.isFinite(Number(row.value)));
  if (!usable.length) return emptyCard(title, "本报告没有可用于该图的观测点。");
  const maximum = Math.max(...usable.map((row) => Math.abs(Number(row.value))), 0.000001);
  const card = element("article", `chart-card ${className}`.trim());
  const head = element("div", "chart-head");
  const headText = element("div");
  append(headText, element("h3", "", title), element("p", "", note));
  append(head, headText);
  const chart = element("div", "bar-chart");
  usable.forEach((row, index) => {
    const item = element("div", "bar-row");
    const label = element("span", "bar-name", row.label);
    const track = element("div", "bar-track");
    const fill = element("i", "bar-fill");
    fill.style.width = `${Math.max(1, (Math.abs(Number(row.value)) / maximum) * 100)}%`;
    fill.style.background = row.color || REPORT_COLORS[index % REPORT_COLORS.length];
    track.append(fill);
    const value = element("strong", "", `${formatNumber(row.value)} ${row.unit || ""}`);
    append(item, label, track, value);
    chart.append(item);
  });
  append(card, head, chart);
  return card;
}

function reportCurve(
  report,
  {
    title,
    note,
    group,
    metric,
    xLabel,
    seriesLabel,
    filter,
    xMode,
    xTitle,
    yTitle,
    logX,
    className,
  }
) {
  const rows = observations(report, group, metric, filter || (() => true));
  const points = rows.map((item) => ({
    x: Number(item.labels?.[xLabel]),
    y: Number(item.value),
    unit: item.unit,
    series: seriesLabel
      ? (OPERATION_NAMES[item.labels?.[seriesLabel]] || item.labels?.[seriesLabel] || "观测")
      : "观测",
  }));
  return lineChart({
    title, note, points, xMode, xTitle, yTitle, logX, className,
  });
}


function heatColor(value, minimum, maximum) {
  const ratio = maximum === minimum ? 0.5 : (value - minimum) / (maximum - minimum);
  const hue = 82 - Math.max(0, Math.min(1, ratio)) * 78;
  return `hsl(${hue} 78% 52%)`;
}

function heatmapCard({
  title,
  note,
  rows,
  rowLabel,
  columnLabel,
  symmetric = false,
  unit = "",
}) {
  if (!rows.length) return emptyCard(title, "本报告没有该矩阵的观测点。");
  const rowNames = [...new Set(rows.map((item) => String(item.labels?.[rowLabel])))].sort(
    (a, b) => Number(a) - Number(b)
  );
  const columnNames = [...new Set(rows.map((item) => String(item.labels?.[columnLabel])))].sort(
    (a, b) => Number(a) - Number(b)
  );
  if (symmetric) {
    const all = [...new Set([...rowNames, ...columnNames])].sort((a, b) => Number(a) - Number(b));
    rowNames.splice(0, rowNames.length, ...all);
    columnNames.splice(0, columnNames.length, ...all);
  }
  const rowIndex = new Map(rowNames.map((name, index) => [name, index]));
  const columnIndex = new Map(columnNames.map((name, index) => [name, index]));
  const matrix = new Map();
  for (const item of rows) {
    const row = String(item.labels?.[rowLabel]);
    const column = String(item.labels?.[columnLabel]);
    matrix.set(`${row}\0${column}`, Number(item.value));
    if (symmetric) matrix.set(`${column}\0${row}`, Number(item.value));
  }
  const values = [...matrix.values()].filter(Number.isFinite);
  const minimum = Math.min(...values);
  const maximum = Math.max(...values);
  const largest = Math.max(rowNames.length, columnNames.length);
  const cell = largest > 128 ? 12 : largest > 64 ? 16 : largest > 24 ? 22 : 30;
  const axis = largest > 64 ? 58 : 72;
  const logicalWidth = axis + columnNames.length * cell + 10;
  const logicalHeight = axis + rowNames.length * cell + 10;
  const dpr = largest > 128 ? 1 : Math.min(window.devicePixelRatio || 1, 2);

  const card = element("article", "chart-card matrix-card wide");
  const head = element("div", "chart-head matrix-head");
  const heading = element("div");
  append(
    heading,
    element("h3", "", title),
    element("p", "", `${note} · ${rowNames.length} × ${columnNames.length}`)
  );
  const toolbar = element("div", "matrix-toolbar");
  const out = element("output", "", "100%");
  const range = document.createElement("input");
  range.type = "range";
  range.min = "10";
  range.max = "200";
  range.value = "100";
  range.setAttribute("aria-label", "矩阵缩放");
  const fit = button("适应", "quiet-button");
  const actual = button("100%", "quiet-button");
  append(toolbar, fit, actual, range, out);
  append(head, heading, toolbar);
  card.append(head);

  const viewport = element("div", "matrix-viewport");
  const canvas = document.createElement("canvas");
  canvas.width = Math.ceil(logicalWidth * dpr);
  canvas.height = Math.ceil(logicalHeight * dpr);
  canvas.style.width = `${logicalWidth}px`;
  canvas.style.height = `${logicalHeight}px`;
  canvas.setAttribute("aria-label", `${title}，${rowNames.length} 行 ${columnNames.length} 列`);
  const context = canvas.getContext("2d");
  context.scale(dpr, dpr);
  context.fillStyle = "#111411";
  context.fillRect(0, 0, logicalWidth, logicalHeight);
  context.font = largest > 64 ? "9px ui-monospace" : "11px ui-monospace";
  context.textAlign = "center";
  context.textBaseline = "middle";
  const labelStride = Math.max(1, Math.ceil(largest / 32));
  columnNames.forEach((name, index) => {
    if (index % labelStride !== 0) return;
    context.save();
    context.fillStyle = "#aaa99f";
    context.translate(axis + index * cell + cell / 2, axis - 7);
    context.rotate(-Math.PI / 3);
    context.fillText(name, 0, 0);
    context.restore();
  });
  rowNames.forEach((name, index) => {
    if (index % labelStride === 0) {
      context.fillStyle = "#aaa99f";
      context.textAlign = "right";
      context.fillText(name, axis - 8, axis + index * cell + cell / 2);
    }
    columnNames.forEach((column, columnOffset) => {
      const value = matrix.get(`${name}\0${column}`);
      context.fillStyle = Number.isFinite(value)
        ? heatColor(value, minimum, maximum)
        : name === column ? "#30362f" : "#202420";
      context.fillRect(
        axis + columnOffset * cell + 1,
        axis + index * cell + 1,
        Math.max(1, cell - 2),
        Math.max(1, cell - 2)
      );
    });
  });
  viewport.append(canvas);
  const tooltip = element("div", "matrix-tooltip");
  viewport.append(tooltip);
  card.append(viewport);

  function setZoom(percent) {
    const scale = Number(percent) / 100;
    canvas.style.width = `${logicalWidth * scale}px`;
    canvas.style.height = `${logicalHeight * scale}px`;
    range.value = String(Math.round(Number(percent)));
    out.value = `${Math.round(Number(percent))}%`;
  }
  function fitZoom() {
    const available = Math.max(200, viewport.clientWidth - 4);
    setZoom(Math.min(100, Math.max(10, (available / logicalWidth) * 100)));
    viewport.scrollTo(0, 0);
  }
  range.addEventListener("input", () => setZoom(range.value));
  actual.addEventListener("click", () => setZoom(100));
  fit.addEventListener("click", fitZoom);
  canvas.addEventListener("mousemove", (event) => {
    const rect = canvas.getBoundingClientRect();
    const x = ((event.clientX - rect.left) / rect.width) * logicalWidth;
    const y = ((event.clientY - rect.top) / rect.height) * logicalHeight;
    const column = Math.floor((x - axis) / cell);
    const row = Math.floor((y - axis) / cell);
    if (row < 0 || column < 0 || row >= rowNames.length || column >= columnNames.length) {
      tooltip.classList.remove("visible");
      return;
    }
    const value = matrix.get(`${rowNames[row]}\0${columnNames[column]}`);
    tooltip.textContent = `${rowNames[row]} → ${columnNames[column]} · ${Number.isFinite(value) ? formatNumber(value) : "未测"} ${unit}`;
    tooltip.style.left = `${event.offsetX + 14}px`;
    tooltip.style.top = `${event.offsetY + 14}px`;
    tooltip.classList.add("visible");
  });
  canvas.addEventListener("mouseleave", () => tooltip.classList.remove("visible"));
  window.requestAnimationFrame(fitZoom);
  return card;
}


export { lineChart, barChart, reportCurve, heatmapCard };
