import { formatDate } from "./model.js";
import { renderComparison } from "./comparison.js";
import { renderSingle } from "./single-report.js";
import { append, button, element } from "./ui.js";

const state = {
  catalog: [],
  selected: new Set(),
  reports: new Map(),
  routeIds: [],
};

const elements = {
  app: document.querySelector("#app"),
  list: document.querySelector("#report-list"),
  connection: document.querySelector("#connection"),
  selectionCount: document.querySelector("#selection-count"),
  compare: document.querySelector("#compare"),
  search: document.querySelector("#report-search"),
  toast: document.querySelector("#toast"),
};


function notify(message) {
  elements.toast.textContent = message;
  elements.toast.classList.add("visible");
  window.setTimeout(() => elements.toast.classList.remove("visible"), 3000);
}


function loadingState(message = "正在加载 JSON 报告") {
  const section = element("section", "empty-state");
  append(section, element("span", "pulse"), element("p", "", message));
  elements.app.replaceChildren(section);
}

async function loadReports(ids) {
  return Promise.all(ids.map(async (id) => {
    if (!state.reports.has(id)) {
      const response = await fetch(`/api/reports/${encodeURIComponent(id)}`);
      if (!response.ok) throw new Error(`报告加载失败：HTTP ${response.status}`);
      state.reports.set(id, await response.json());
    }
    return state.reports.get(id);
  }));
}

function updateRoute(ids, replace = false) {
  const url = new URL(window.location.href);
  url.searchParams.delete("report");
  url.searchParams.delete("compare");
  if (ids.length === 1) url.searchParams.set("report", ids[0]);
  if (ids.length > 1) url.searchParams.set("compare", ids.join(","));
  const method = replace ? "replaceState" : "pushState";
  history[method]({ ids }, "", url);
}

async function showReports(ids, pushRoute = true) {
  const knownIds = ids.filter((id) => state.catalog.some((item) => item.id === id));
  if (!knownIds.length) {
    elements.app.replaceChildren(
      element("section", "empty-state", "请选择一份报告，或勾选多份报告进行对比。")
    );
    return;
  }
  if (knownIds.length > 8) {
    notify("一次最多对比 8 份报告");
    return;
  }
  loadingState(knownIds.length > 1 ? `正在加载 ${knownIds.length} 份报告` : "正在加载报告");
  const reports = await loadReports(knownIds);
  const catalogItems = knownIds.map((id) => state.catalog.find((item) => item.id === id));
  state.routeIds = knownIds;
  if (pushRoute) updateRoute(knownIds);
  if (reports.length === 1) renderSingle(reports[0], catalogItems[0]);
  else renderComparison(reports, catalogItems);
}

function syncSelectionControls() {
  elements.selectionCount.textContent = `已选 ${state.selected.size} 份`;
  elements.compare.disabled = state.selected.size === 0;
}

function renderCatalog() {
  elements.list.replaceChildren();
  const query = elements.search.value.trim().toLowerCase();
  const visible = state.catalog.filter((item) =>
    [
      item.hostname,
      item.cpu_model,
      item.profile,
      item.platform_family,
      item.started_at,
      item.name,
    ].join(" ").toLowerCase().includes(query)
  );
  if (!visible.length) {
    elements.list.append(element("p", "catalog-empty", state.catalog.length
      ? "没有匹配的报告"
      : "目录中还没有 report.json"));
    return;
  }
  visible.forEach((item) => {
    const row = element("article", "report-item");
    if (state.routeIds.includes(item.id)) row.classList.add("active");
    const select = element("label", "report-check");
    const checkbox = document.createElement("input");
    checkbox.type = "checkbox";
    checkbox.checked = state.selected.has(item.id);
    checkbox.setAttribute("aria-label", `选择 ${item.hostname}`);
    checkbox.addEventListener("change", () => {
      if (checkbox.checked && state.selected.size >= 8) {
        checkbox.checked = false;
        notify("一次最多对比 8 份报告");
        return;
      }
      checkbox.checked ? state.selected.add(item.id) : state.selected.delete(item.id);
      syncSelectionControls();
    });
    select.append(checkbox);
    const open = button("", "report-open");
    const title = element("span", "report-title");
    append(title, element("strong", "", item.hostname), element("small", "", item.cpu_model || "处理器未知"));
    const metadata = element("span", "report-metadata");
    append(
      metadata,
      element("b", "", String(item.profile || "unknown").toUpperCase()),
      element("span", "", `${item.physical_cores ?? "—"}C / ${item.logical_cpus ?? "—"}T`),
      element("time", "", formatDate(item.started_at))
    );
    append(open, title, metadata);
    open.addEventListener("click", () => showReports([item.id]).catch((error) => notify(error.message)));
    append(row, select, open);
    elements.list.append(row);
  });
}

function routeFromLocation() {
  const params = new URLSearchParams(window.location.search);
  const compare = params.get("compare");
  if (compare) return compare.split(",").filter(Boolean);
  const report = params.get("report");
  return report ? [report] : [];
}

async function refreshCatalog({ restoreRoute = false } = {}) {
  const response = await fetch("/api/reports");
  if (!response.ok) throw new Error(`目录加载失败：HTTP ${response.status}`);
  state.catalog = (await response.json()).reports;
  elements.connection.textContent = `已连接 · ${state.catalog.length} 份报告`;
  const routeIds = restoreRoute ? routeFromLocation() : state.routeIds;
  const validRoute = routeIds.filter((id) => state.catalog.some((item) => item.id === id));
  if (validRoute.length) {
    validRoute.forEach((id) => state.selected.add(id));
    syncSelectionControls();
    await showReports(validRoute, false);
  } else if (state.catalog.length && !state.routeIds.length) {
    state.selected.add(state.catalog[0].id);
    syncSelectionControls();
    await showReports([state.catalog[0].id], false);
    updateRoute([state.catalog[0].id], true);
  }
  renderCatalog();
}

elements.compare.addEventListener("click", () => {
  showReports([...state.selected]).then(renderCatalog).catch((error) => notify(error.message));
});
elements.search.addEventListener("input", renderCatalog);
document.querySelector("#refresh").addEventListener("click", () => {
  refreshCatalog().catch((error) => notify(error.message));
});
document.querySelector("#theme").addEventListener("click", () => {
  document.documentElement.classList.toggle("light");
  localStorage.setItem(
    "system-decap-theme",
    document.documentElement.classList.contains("light") ? "light" : "dark"
  );
});
window.addEventListener("popstate", () => {
  showReports(routeFromLocation(), false).then(renderCatalog).catch((error) => notify(error.message));
});

if (localStorage.getItem("system-decap-theme") === "light") {
  document.documentElement.classList.add("light");
}

refreshCatalog({ restoreRoute: true }).catch((error) => {
  elements.connection.textContent = "报告仓库连接失败";
  elements.app.replaceChildren(
    element("section", "empty-state", "无法读取报告仓库，请检查服务端目录与网络连接。")
  );
  notify(error.message);
});
