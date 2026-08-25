"""Render a standalone, offline, data-dense HTML laboratory report."""

from __future__ import annotations

import html
import json
import math
from collections import defaultdict
from datetime import datetime
from typing import Any, Callable


PALETTE = ["#ff6b35", "#00a6a6", "#e3b341", "#4f7cff", "#ce5a9e", "#78b159", "#8f72d8"]


def _h(value: Any) -> str:
    return html.escape(str(value), quote=True)


def _human_bytes(value: float | int | None) -> str:
    if value is None:
        return "—"
    number = float(value)
    for suffix in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(number) < 1024 or suffix == "TiB":
            return f"{number:.0f} {suffix}" if suffix == "B" else f"{number:.2f} {suffix}"
        number /= 1024
    return f"{number:.2f} TiB"


def _number(value: Any, unit: str = "") -> str:
    if value is None:
        return "—"
    if isinstance(value, str):
        return value
    number = float(value)
    if unit == "bytes":
        return _human_bytes(number)
    if abs(number) >= 1000:
        result = f"{number:,.0f}"
    elif abs(number) >= 100:
        result = f"{number:.1f}"
    elif abs(number) >= 10:
        result = f"{number:.2f}"
    else:
        result = f"{number:.3f}"
    return f"{result} {unit}".strip()


def _estimate_map(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {item["key"]: item for item in report.get("estimates", [])}


def _obs(report: dict[str, Any], group: str, metric: str) -> list[dict[str, Any]]:
    return [
        item for item in report.get("observations", [])
        if item.get("group") == group and item.get("metric") == metric
    ]


def _empty_chart(title: str, note: str) -> str:
    return f"""
    <div class="chart empty-chart">
      <div class="chart-heading"><h3>{_h(title)}</h3></div>
      <div class="empty-mark">∅</div><p>{_h(note)}</p>
    </div>"""


def _line_chart(
    title: str,
    items: list[dict[str, Any]],
    x_value: Callable[[dict[str, Any]], float],
    series_value: Callable[[dict[str, Any]], str],
    x_display: Callable[[float], str],
    unit: str,
    note: str,
    log_x: bool = False,
) -> str:
    if not items:
        return _empty_chart(title, note)
    width, height = 860, 330
    left, right, top, bottom = 76, 24, 40, 58
    plot_w, plot_h = width - left - right, height - top - bottom
    grouped: dict[str, list[tuple[float, float, dict[str, Any]]]] = defaultdict(list)
    for item in items:
        try:
            x = float(x_value(item))
            y = float(item["value"])
            if x > 0 and math.isfinite(x) and math.isfinite(y):
                grouped[series_value(item)].append((x, y, item))
        except (KeyError, TypeError, ValueError):
            continue
    if not grouped:
        return _empty_chart(title, note)
    all_points = [point for points in grouped.values() for point in points]
    raw_x = [point[0] for point in all_points]
    ys = [point[1] for point in all_points]
    transformed_x = [math.log2(x) if log_x else x for x in raw_x]
    x_min, x_max = min(transformed_x), max(transformed_x)
    y_min, y_max = min(0.0, min(ys)), max(ys)
    if x_min == x_max:
        x_max += 1
    if y_min == y_max:
        y_max += 1
    y_max *= 1.08

    def px(value: float) -> float:
        transformed = math.log2(value) if log_x else value
        return left + (transformed - x_min) / (x_max - x_min) * plot_w

    def py(value: float) -> float:
        return top + plot_h - (value - y_min) / (y_max - y_min) * plot_h

    elements = [
        f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{_h(title)}">',
        f'<rect class="plot-bg" x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" rx="5"/>',
    ]
    for tick in range(6):
        value = y_min + (y_max - y_min) * tick / 5
        y = py(value)
        elements.append(f'<line class="grid" x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}"/>')
        elements.append(f'<text class="axis" x="{left - 10}" y="{y + 4:.2f}" text-anchor="end">{value:.2f}</text>')
    unique_x = sorted(set(raw_x))
    tick_x = unique_x if len(unique_x) <= 8 else [unique_x[round(i * (len(unique_x) - 1) / 7)] for i in range(8)]
    for value in tick_x:
        x = px(value)
        elements.append(f'<line class="tick" x1="{x:.2f}" y1="{top + plot_h}" x2="{x:.2f}" y2="{top + plot_h + 6}"/>')
        elements.append(f'<text class="axis" x="{x:.2f}" y="{top + plot_h + 24}" text-anchor="middle">{_h(x_display(value))}</text>')
    for series_index, (series, points) in enumerate(sorted(grouped.items())):
        color = PALETTE[series_index % len(PALETTE)]
        points.sort(key=lambda point: point[0])
        coords = " ".join(f"{px(x):.2f},{py(y):.2f}" for x, y, _ in points)
        elements.append(f'<polyline class="series-line" points="{coords}" stroke="{color}"/>')
        for x, y, item in points:
            label_detail = ", ".join(f"{k}={v}" for k, v in item.get("labels", {}).items())
            elements.append(
                f'<circle class="point" cx="{px(x):.2f}" cy="{py(y):.2f}" r="4.5" fill="{color}">'
                f'<title>{_h(series)} · {_h(label_detail)} · {y:.4f} {_h(unit)}</title></circle>'
            )
    elements.append(f'<text class="axis-label" x="18" y="{top + plot_h / 2}" transform="rotate(-90 18 {top + plot_h / 2})" text-anchor="middle">{_h(unit)}</text>')
    elements.append("</svg>")
    legend = "".join(
        f'<span><i style="--swatch:{PALETTE[index % len(PALETTE)]}"></i>{_h(series)}</span>'
        for index, series in enumerate(sorted(grouped))
    )
    return f"""
    <div class="chart reveal">
      <div class="chart-heading"><h3>{_h(title)}</h3><div class="legend">{legend}</div></div>
      {''.join(elements)}
      <p class="chart-note">{_h(note)}</p>
    </div>"""


def _bar_chart(title: str, rows: list[tuple[str, float, str]], note: str) -> str:
    if not rows:
        return _empty_chart(title, note)
    width = 860
    row_height = 36
    top, left, right, bottom = 26, 210, 180, 28
    height = top + bottom + row_height * len(rows)
    maximum = max((value for _, value, _ in rows), default=1) or 1
    plot_w = width - left - right
    elements = [f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{_h(title)}">']
    for index, (label, value, unit) in enumerate(rows):
        y = top + index * row_height
        bar_w = max(2, value / maximum * plot_w)
        color = PALETTE[index % len(PALETTE)]
        elements.append(f'<text class="bar-label" x="{left - 12}" y="{y + 20}" text-anchor="end">{_h(label)}</text>')
        elements.append(f'<rect class="bar-track" x="{left}" y="{y + 7}" width="{plot_w}" height="18" rx="3"/>')
        elements.append(f'<rect class="bar" x="{left}" y="{y + 7}" width="{bar_w:.2f}" height="18" rx="3" fill="{color}"/>')
        elements.append(f'<text class="bar-value" x="{min(width - 4, left + bar_w + 8):.2f}" y="{y + 21}">{value:.3f} {_h(unit)}</text>')
    elements.append("</svg>")
    return f"""<div class="chart reveal"><div class="chart-heading"><h3>{_h(title)}</h3></div>
    {''.join(elements)}<p class="chart-note">{_h(note)}</p></div>"""


def _heatmap(
    title: str,
    items: list[dict[str, Any]],
    row_key: str,
    column_key: str,
    unit: str,
    note: str,
    row_prefix: str = "CPU",
    column_prefix: str = "MEM",
) -> str:
    if not items:
        return _empty_chart(title, note)
    rows = sorted({item.get("labels", {}).get(row_key, "?") for item in items}, key=lambda x: int(x) if str(x).isdigit() else str(x))
    columns = sorted({item.get("labels", {}).get(column_key, "?") for item in items}, key=lambda x: int(x) if str(x).isdigit() else str(x))
    values = {(item["labels"].get(row_key, "?"), item["labels"].get(column_key, "?")): float(item["value"]) for item in items}
    present = list(values.values())
    minimum, maximum = min(present), max(present)

    def color(value: float) -> str:
        ratio = 0.5 if maximum == minimum else (value - minimum) / (maximum - minimum)
        # Deep teal -> amber -> signal orange.
        if ratio < 0.5:
            local = ratio * 2
            start, end = (0, 122, 128), (227, 179, 65)
        else:
            local = (ratio - 0.5) * 2
            start, end = (227, 179, 65), (255, 107, 53)
        rgb = tuple(round(a + (b - a) * local) for a, b in zip(start, end))
        return f"rgb{rgb}"

    header = "<th></th>" + "".join(f"<th>{_h(column_prefix)} { _h(column) }</th>" for column in columns)
    body = []
    for row in rows:
        cells = []
        for column in columns:
            value = values.get((row, column))
            if value is None:
                cells.append('<td class="missing">—</td>')
            else:
                cells.append(
                    f'<td style="--cell:{color(value)}"><strong>{value:.2f}</strong><small>{_h(unit)}</small></td>'
                )
        body.append(f"<tr><th>{_h(row_prefix)} { _h(row) }</th>{''.join(cells)}</tr>")
    return f"""
    <div class="chart reveal heatmap-card"><div class="chart-heading"><h3>{_h(title)}</h3>
      <span class="range">{minimum:.2f} → {maximum:.2f} {_h(unit)}</span></div>
      <div class="table-scroll"><table class="heatmap"><thead><tr>{header}</tr></thead><tbody>{''.join(body)}</tbody></table></div>
      <p class="chart-note">{_h(note)}</p>
    </div>"""


def _metric_card(item: dict[str, Any] | None, eyebrow: str) -> str:
    if not item or not item.get("available"):
        return f'<article class="metric unavailable"><span>{_h(eyebrow)}</span><strong>Not available</strong><small>See run warnings</small></article>'
    confidence = item.get("confidence", "unknown")
    return f"""
    <article class="metric reveal">
      <span>{_h(eyebrow)}</span>
      <strong>{_h(_number(item.get('value'), item.get('unit', '')))}</strong>
      <small><i class="confidence {confidence}"></i>{_h(confidence)} confidence</small>
      <div class="metric-detail"><b>{_h(item.get('name'))}</b><br>{_h(item.get('basis'))}<br>{_h(item.get('caveat', ''))}</div>
    </article>"""


def _topology_visual(system: dict[str, Any]) -> str:
    topology = system.get("topology", {})
    cpus = [cpu for cpu in topology.get("cpus", []) if cpu.get("allowed") and cpu.get("online")]
    by_node: dict[Any, list[dict[str, Any]]] = defaultdict(list)
    for cpu in cpus:
        by_node[cpu.get("node", 0)].append(cpu)
    nodes = []
    for node, node_cpus in sorted(by_node.items()):
        by_core: dict[tuple[Any, Any, Any], list[int]] = defaultdict(list)
        for cpu in node_cpus:
            by_core[(cpu.get("socket"), cpu.get("die"), cpu.get("core"))].append(cpu["cpu"])
        core_html = "".join(
            f'<span class="core" title="socket {key[0]}, die {key[1]}, core {key[2]}">'
            f'<b>C{key[2]}</b><em>{"/".join(map(str, threads))}</em></span>'
            for key, threads in sorted(by_core.items())
        )
        nodes.append(
            f'<div class="node"><header><span>NUMA</span><strong>{_h(node)}</strong>'
            f'<small>{len(by_core)} cores · {len(node_cpus)} threads</small></header><div class="cores">{core_html}</div></div>'
        )
    return f'<div class="topology-map">{"".join(nodes)}</div>' if nodes else "<p>No accessible topology data.</p>"


def _inventory_tables(system: dict[str, Any]) -> str:
    caches = system.get("caches", [])
    cache_rows = "".join(
        f"<tr><td>L{_h(c.get('level'))} {_h(c.get('type'))}</td><td>{_h(_human_bytes(c.get('size_bytes')))}</td>"
        f"<td>{_h(c.get('line_bytes'))}</td><td>{_h(c.get('ways'))}</td><td>{_h(c.get('sets'))}</td>"
        f"<td class='mono'>{_h(','.join(map(str, c.get('shared_cpus', []))))}</td></tr>"
        for c in caches
    ) or '<tr><td colspan="6">No cache inventory</td></tr>'
    numa_rows = "".join(
        f"<tr><td>{_h(n.get('node'))}</td><td class='mono'>{_h(','.join(map(str, n.get('cpus', []))))}</td>"
        f"<td>{_h(_human_bytes(n.get('memory', {}).get('MemTotal')))}</td>"
        f"<td class='mono'>{_h(' '.join(map(str, n.get('distance', []))))}</td></tr>"
        for n in system.get("numa", [])
    ) or '<tr><td colspan="4">Single node or NUMA sysfs unavailable</td></tr>'
    return f"""
    <div class="split inventory-grid">
      <div><h3>Cache instances</h3><div class="table-scroll"><table class="data-table compact"><thead><tr>
        <th>Level/type</th><th>Capacity</th><th>Line</th><th>Ways</th><th>Sets</th><th>Shared CPUs</th>
      </tr></thead><tbody>{cache_rows}</tbody></table></div></div>
      <div><h3>NUMA inventory</h3><div class="table-scroll"><table class="data-table compact"><thead><tr>
        <th>Node</th><th>CPUs</th><th>Memory</th><th>Distance row</th>
      </tr></thead><tbody>{numa_rows}</tbody></table></div></div>
    </div>"""


def _raw_table(report: dict[str, Any]) -> str:
    rows = []
    for item in report.get("observations", []):
        labels = " · ".join(f"{k}={v}" for k, v in sorted(item.get("labels", {}).items()))
        search = " ".join([item.get("group", ""), item.get("metric", ""), labels, item.get("method", "")])
        rows.append(
            f'<tr data-search="{_h(search.lower())}"><td><span class="group-tag">{_h(item.get("group"))}</span></td>'
            f'<td><b>{_h(item.get("metric"))}</b><small>{_h(item.get("method"))}</small></td>'
            f'<td class="numeric">{_h(_number(item.get("value"), item.get("unit", "")))}</td>'
            f'<td><i class="confidence {_h(item.get("confidence"))}"></i>{_h(item.get("confidence"))}</td>'
            f'<td class="mono labels">{_h(labels)}</td></tr>'
        )
    return "".join(rows)


def _coverage(report: dict[str, Any]) -> str:
    groups = {item.get("group") for item in report.get("observations", [])}
    mapping = {
        "identity": True, "topology": True, "cache": bool(_obs(report, "cache_latency", "random_load_latency")),
        "latency": bool(groups & {"cache_latency", "tlb_latency", "numa"}),
        "bandwidth": bool(groups & {"memory_bandwidth", "numa"}),
        "memory": bool(groups & {"memory_access", "memory_parallelism"}),
        "coherence": bool(groups & {"core_latency", "coherence"}),
        "core": "pipeline" in groups, "branch": "branch" in groups,
        "os": True, "environment": True,
    }
    rows = []
    for item in report.get("metric_catalog", []):
        covered = mapping.get(item.get("category"), False)
        rows.append(
            f'<tr><td><i class="coverage {"yes" if covered else "no"}"></i></td>'
            f'<td>{_h(item.get("category"))}</td><td>{_h(item.get("metric"))}</td>'
            f'<td>{_h(item.get("kind"))}</td><td>{_h(item.get("nominal_confidence"))}</td></tr>'
        )
    return "".join(rows)


def render_report(report: dict[str, Any]) -> str:
    """Render report data as a single HTML file with no network dependencies."""
    system = report.get("system", {})
    cpu = system.get("cpu", {})
    topology = system.get("topology", {})
    estimates = _estimate_map(report)
    run = report.get("run", {})

    cache_curve = _line_chart(
        "Memory hierarchy · dependent-load latency",
        _obs(report, "cache_latency", "random_load_latency"),
        lambda item: int(item["labels"]["working_set_bytes"]), lambda _: "random load",
        lambda x: _human_bytes(x), "ns/access",
        "Working sets grow by 2×. Steps usually indicate cache-capacity boundaries; address translation is included.", True,
    )
    tlb_curve = _line_chart(
        "Address translation · one random access per page",
        _obs(report, "tlb_latency", "page_random_load_latency"),
        lambda item: int(item["labels"]["pages"]), lambda _: "base pages",
        lambda x: f"{int(x)}p", "ns/access",
        "Only one cache line is touched per page, emphasizing TLB capacity and page-walk knees.", True,
    )
    bandwidth_curve = _line_chart(
        "Sustainable memory payload bandwidth",
        _obs(report, "memory_bandwidth", "stream_bandwidth"),
        lambda item: int(item["labels"]["threads"]), lambda item: item["labels"]["operation"],
        lambda x: str(int(x)), "GB/s",
        "Threads are pinned to distinct physical cores. Copy/triad count useful payload, excluding coherence and write-allocate traffic.",
    )
    cache_bandwidth_curve = _line_chart(
        "One-core bandwidth across cache-sized working sets",
        _obs(report, "cache_bandwidth", "working_set_bandwidth"),
        lambda item: int(item["labels"]["working_set_bytes"]), lambda item: item["labels"]["operation"],
        lambda x: _human_bytes(x), "GB/s",
        "Repeated read/copy throughput forms hierarchy steps as the working set crosses L1, L2, L3 and memory.", True,
    )
    stride_curve = _line_chart(
        "Stride & hardware-prefetch sensitivity",
        _obs(report, "memory_access", "stride_access_rate"),
        lambda item: int(item["labels"]["stride_bytes"]), lambda _: "fixed stride",
        lambda x: f"{int(x)}B", "Gaccess/s",
        "Only requested 8-byte loads are counted. Larger strides reduce cache-line utilization and challenge prefetchers.", True,
    )
    mlp_curve = _line_chart(
        "Memory-level parallelism",
        _obs(report, "memory_parallelism", "random_load_rate"),
        lambda item: int(item["labels"]["chains"]), lambda _: "independent chains",
        lambda x: str(int(x)), "Gaccess/s",
        "Independent random chains, each internally dependent, expose a lower bound on concurrent misses sustained by one core.", True,
    )
    false_sharing_curve = _line_chart(
        "False-sharing boundary",
        _obs(report, "coherence", "atomic_update_rate"),
        lambda item: int(item["labels"]["separation_bytes"]), lambda _: "two atomics",
        lambda x: f"{int(x)}B", "Mop/s",
        "Two cores update distinct atomics. The throughput recovery usually marks the coherence-line boundary.", True,
    )
    rob_curve = _line_chart(
        "Reorder-window overlap probe",
        _obs(report, "reorder_window", "cold_load_overlap_penalty"),
        lambda item: int(item["labels"]["estimated_uops"]), lambda _: "cold − hot",
        lambda x: str(int(x)), "counter-ticks",
        "x86/C86 only. A step appears when the second cache miss no longer overlaps the first; this is a low-confidence ROB/scheduling-window proxy.",
    )

    pipeline_rows = []
    for item in _obs(report, "pipeline", "operations_per_cycle"):
        if item["metric"] == "operations_per_cycle":
            pipeline_rows.append((item["labels"].get("kernel", "unknown"), item["value"], item["unit"]))
    pipeline_chart = _bar_chart(
        "Execution throughput by scalar microkernel", pipeline_rows,
        "Dependency chains expose latency constraints; independent chains expose a backend throughput lower bound, not a literal port count.",
    )
    branch_rows = [
        (item["labels"].get("pattern", "unknown"), item["value"], item["unit"])
        for item in _obs(report, "branch", "time_per_branch")
    ]
    branch_chart = _bar_chart(
        "Branch pattern cost", branch_rows,
        "The random-versus-predictable gap approximates misprediction recovery cost. The compiler is instructed to preserve scalar branches.",
    )
    core_rows = []
    relations: dict[str, list[float]] = defaultdict(list)
    for item in _obs(report, "core_latency", "cacheline_handoff_latency"):
        relations[item["labels"].get("relation", "unknown")].append(item["value"])
    for relation, values in sorted(relations.items()):
        core_rows.append((relation, sum(values) / len(values), "ns/one-way"))
    core_chart = _bar_chart(
        "Core-to-core cache-line handoff", core_rows,
        "Release/acquire ping-pong round trip divided by two. The deep profile samples a much fuller CPU-pair matrix.",
    )
    core_matrix = ""
    if run.get("profile") == "deep":
        matrix_items = []
        for item in _obs(report, "core_latency", "cacheline_handoff_latency"):
            forward = dict(item)
            forward["labels"] = {**item["labels"], "row_cpu": item["labels"].get("cpu_a"),
                                  "column_cpu": item["labels"].get("cpu_b")}
            reverse = dict(item)
            reverse["labels"] = {**item["labels"], "row_cpu": item["labels"].get("cpu_b"),
                                  "column_cpu": item["labels"].get("cpu_a")}
            matrix_items.extend((forward, reverse))
        core_matrix = '<div class="wide">' + _heatmap(
            "Core-pair handoff matrix", matrix_items, "row_cpu", "column_cpu", "ns/one-way",
            "Symmetric visualization of the deep-profile cache-line handoff pairs.", "CPU", "CPU",
        ) + "</div>"
    os_rows = [
        (item["metric"], item["value"], item["unit"])
        for item in report.get("observations", [])
        if item.get("group") == "os_overhead" and item.get("metric") != "minor_faults"
    ]
    os_chart = _bar_chart(
        "OS boundary overheads", os_rows,
        "Direct syscall, anonymous-page first touch and pinned-thread pipe handoff; each bar carries its own unit.",
    )
    numa_latency = _heatmap(
        "NUMA latency matrix", _obs(report, "numa", "load_latency"),
        "cpu_node", "memory_node", "ns/access", "Rows are executing CPU nodes; columns are page-resident memory nodes.",
    )
    numa_bandwidth = _heatmap(
        "NUMA read-bandwidth matrix", _obs(report, "numa", "read_bandwidth"),
        "cpu_node", "memory_node", "GB/s", "Off-diagonal cells are useful payload carried by the NUMA interconnect.",
    )

    warning_html = "".join(f"<li>{_h(warning)}</li>" for warning in report.get("warnings", []))
    if not warning_html:
        warning_html = "<li>No probe warnings were emitted.</li>"
    flags = cpu.get("flags", [])
    flag_html = "".join(f"<span>{_h(flag)}</span>" for flag in flags)
    dmi = system.get("dmi", {})
    machine_name = " · ".join(filter(None, [dmi.get("sys_vendor"), dmi.get("product_name")])) or system.get("hostname", "unknown")
    generated = datetime.now().astimezone().isoformat(timespec="seconds")
    raw_json_size = len(json.dumps(report, ensure_ascii=False))

    return f"""<!doctype html>
<html lang="zh-CN" data-theme="light">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>System Decap · {_h(system.get('hostname', 'unknown'))}</title>
<style>
:root{{--paper:#f4f0e8;--paper-2:#e9e2d5;--ink:#102a3a;--muted:#64727a;--line:#c8c1b4;--panel:#fbf9f4;--dark:#102a3a;--signal:#ff6b35;--teal:#007a80;--gold:#d9a62e;--shadow:0 18px 50px rgba(18,35,43,.10);--serif:"Bodoni 72","Noto Serif CJK SC","Songti SC",Georgia,serif;--sans:"Aptos","Noto Sans CJK SC","PingFang SC",sans-serif;--mono:"Berkeley Mono","SFMono-Regular","Cascadia Code",monospace}}
html[data-theme="dark"]{{--paper:#0b1720;--paper-2:#112632;--ink:#e9eee9;--muted:#9eafb5;--line:#294451;--panel:#10232e;--dark:#07131b;--shadow:0 18px 50px rgba(0,0,0,.28)}}
*{{box-sizing:border-box}}html{{scroll-behavior:smooth}}body{{margin:0;background:var(--paper);color:var(--ink);font-family:var(--sans);line-height:1.55}}
body:before{{content:"";position:fixed;inset:0;pointer-events:none;opacity:.24;background-image:linear-gradient(rgba(16,42,58,.055) 1px,transparent 1px),linear-gradient(90deg,rgba(16,42,58,.055) 1px,transparent 1px);background-size:26px 26px;mask-image:linear-gradient(to bottom,black,transparent 65%)}}
.shell{{width:min(1500px,calc(100% - 40px));margin:auto;position:relative}}.masthead{{padding:72px 0 40px;border-bottom:1px solid var(--line);display:grid;grid-template-columns:1.65fr .7fr;gap:48px}}
.kicker,.section-kicker{{font-family:var(--mono);font-size:11px;letter-spacing:.18em;text-transform:uppercase;color:var(--signal);font-weight:800}}h1{{font:700 clamp(54px,8vw,116px)/.82 var(--serif);letter-spacing:-.055em;margin:20px 0 26px;max-width:920px}}h1 em{{font-style:italic;color:var(--signal)}}
.lede{{font-size:18px;max-width:760px;color:var(--muted)}}.specimen{{background:var(--dark);color:#f6f1e7;padding:26px;border-radius:4px;box-shadow:var(--shadow);align-self:end;position:relative;overflow:hidden}}
.specimen:after{{content:"";position:absolute;width:170px;height:170px;border:34px solid var(--signal);border-radius:50%;right:-95px;top:-105px;opacity:.8}}.specimen dl{{margin:0;display:grid;grid-template-columns:1fr auto;gap:10px;border-top:1px solid #ffffff2b;padding-top:18px}}.specimen dt{{color:#9fb1ba}}.specimen dd{{margin:0;font-family:var(--mono);text-align:right}}
.nav{{position:sticky;top:0;z-index:20;background:color-mix(in srgb,var(--paper) 88%,transparent);backdrop-filter:blur(18px);border-bottom:1px solid var(--line)}}.nav-inner{{display:flex;gap:8px;align-items:center;overflow:auto;padding:11px 0}}.nav a{{color:var(--muted);text-decoration:none;font-size:12px;letter-spacing:.05em;text-transform:uppercase;padding:8px 11px;white-space:nowrap}}.nav a:hover{{color:var(--signal)}}.theme{{margin-left:auto;border:1px solid var(--line);background:var(--panel);color:var(--ink);border-radius:50%;width:34px;height:34px;cursor:pointer}}
section{{padding:72px 0;border-bottom:1px solid var(--line);scroll-margin-top:54px}}.section-head{{display:grid;grid-template-columns:.7fr 1.6fr;gap:30px;margin-bottom:34px}}h2{{font:700 clamp(34px,4.5vw,64px)/.94 var(--serif);letter-spacing:-.035em;margin:8px 0}}.section-intro{{color:var(--muted);font-size:17px;max-width:780px;align-self:end}}
.metrics{{display:grid;grid-template-columns:repeat(6,minmax(0,1fr));gap:1px;background:var(--line);border:1px solid var(--line);box-shadow:var(--shadow)}}.metric{{position:relative;background:var(--panel);min-width:0;min-height:150px;padding:20px;display:flex;flex-direction:column;justify-content:space-between}}.metric>span{{font:700 10px/1.3 var(--mono);letter-spacing:.1em;text-transform:uppercase;color:var(--muted)}}.metric>strong{{font:700 clamp(22px,2.3vw,34px)/1 var(--serif);letter-spacing:-.03em;overflow-wrap:anywhere}}.metric>small{{font:11px var(--mono);color:var(--muted)}}.metric.unavailable{{opacity:.6}}.metric-detail{{display:none;position:absolute;left:12px;right:12px;bottom:calc(100% - 8px);padding:13px;background:var(--dark);color:#eef4ef;z-index:3;font-size:11px;box-shadow:var(--shadow)}}.metric:hover .metric-detail{{display:block}}
.confidence{{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:6px;background:#8d979b}}.confidence.high{{background:#00a67d}}.confidence.medium{{background:var(--gold)}}.confidence.low{{background:var(--signal)}}.confidence.unavailable{{background:#8d979b}}
.chart-grid{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px;margin-top:22px}}.chart-grid .wide{{grid-column:1/-1}}.chart{{background:var(--panel);border:1px solid var(--line);padding:20px;min-width:0;box-shadow:0 6px 20px rgba(18,35,43,.05)}}.chart-heading{{display:flex;align-items:center;justify-content:space-between;gap:20px;margin-bottom:8px}}.chart h3,.inventory-grid h3{{font:700 21px/1.1 var(--serif);margin:0}}.chart svg{{width:100%;height:auto;overflow:visible}}.plot-bg,.bar-track{{fill:color-mix(in srgb,var(--paper-2) 55%,transparent)}}.grid{{stroke:var(--line);stroke-width:1;stroke-dasharray:2 5}}.tick{{stroke:var(--muted)}}.axis,.axis-label,.bar-label,.bar-value{{fill:var(--muted);font:10px var(--mono)}}.axis-label{{font-weight:700;letter-spacing:.08em}}.series-line{{fill:none;stroke-width:2.5;stroke-linecap:round;stroke-linejoin:round}}.point{{stroke:var(--panel);stroke-width:2;cursor:crosshair}}.bar{{transform-origin:left;animation:grow .8s ease both}}.bar-value{{font-weight:700;fill:var(--ink)}}.legend{{display:flex;gap:12px;flex-wrap:wrap;font:10px var(--mono);color:var(--muted)}}.legend i{{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--swatch);margin-right:4px}}.chart-note{{margin:4px 0 0;color:var(--muted);font-size:12px;border-top:1px solid var(--line);padding-top:12px}}.empty-chart{{min-height:240px;text-align:center}}.empty-mark{{font:60px var(--serif);color:var(--line);margin-top:38px}}.range{{font:11px var(--mono);color:var(--muted)}}
.topology-map{{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px}}.node{{border:1px solid var(--line);background:var(--panel);padding:18px}}.node header{{display:grid;grid-template-columns:auto auto 1fr;align-items:baseline;gap:9px;border-bottom:1px solid var(--line);padding-bottom:12px}}.node header span{{font:10px var(--mono);color:var(--signal)}}.node header strong{{font:34px var(--serif)}}.node header small{{text-align:right;color:var(--muted)}}.cores{{display:grid;grid-template-columns:repeat(auto-fill,minmax(70px,1fr));gap:5px;padding-top:12px}}.core{{background:var(--paper-2);padding:7px 9px;display:flex;justify-content:space-between;align-items:center;border-left:2px solid var(--teal)}}.core b{{font:11px var(--mono)}}.core em{{font:9px var(--mono);color:var(--muted);font-style:normal}}
.heatmap{{border-collapse:separate;border-spacing:5px;width:100%;min-width:430px}}.heatmap th{{font:10px var(--mono);color:var(--muted);padding:8px}}.heatmap td{{background:var(--cell);padding:18px 8px;text-align:center;color:#07131b;border-radius:3px}}.heatmap td strong{{display:block;font:700 19px var(--serif)}}.heatmap td small{{font:9px var(--mono)}}.heatmap td.missing{{background:var(--paper-2);color:var(--muted)}}
.split{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:24px}}.table-scroll{{overflow:auto}}table{{width:100%}}.data-table{{border-collapse:collapse;background:var(--panel);font-size:13px}}.data-table th{{text-align:left;font:700 10px var(--mono);letter-spacing:.07em;text-transform:uppercase;color:var(--muted);background:var(--paper-2);position:sticky;top:0}}.data-table th,.data-table td{{padding:12px;border-bottom:1px solid var(--line);vertical-align:top}}.data-table td small{{display:block;color:var(--muted);max-width:420px}}.data-table .numeric{{font:700 14px var(--mono);white-space:nowrap}}.compact th,.compact td{{padding:9px;font-size:11px}}.mono{{font-family:var(--mono);font-size:10px;word-break:break-all}}.labels{{max-width:460px}}.group-tag{{font:700 9px var(--mono);letter-spacing:.04em;background:var(--paper-2);padding:4px 6px}}
.filter{{width:100%;border:1px solid var(--line);background:var(--panel);color:var(--ink);font:14px var(--mono);padding:14px 16px;margin-bottom:12px;outline:none}}.filter:focus{{border-color:var(--signal);box-shadow:0 0 0 3px color-mix(in srgb,var(--signal) 18%,transparent)}}
.warning-box{{background:var(--dark);color:#ecf2ed;padding:25px;border-left:7px solid var(--signal)}}.warning-box h3{{font:700 24px var(--serif);margin:0 0 10px}}.warning-box li{{margin:8px 0;color:#bccbd0}}.flags{{display:flex;flex-wrap:wrap;gap:5px}}.flags span{{font:9px var(--mono);padding:5px 7px;border:1px solid var(--line);background:var(--panel)}}details{{border:1px solid var(--line);background:var(--panel);padding:16px;margin-top:14px}}summary{{cursor:pointer;font-weight:700}}pre{{white-space:pre-wrap;word-break:break-word;font:10px/1.6 var(--mono);color:var(--muted);max-height:460px;overflow:auto}}
.coverage{{width:10px;height:10px;border-radius:50%;display:block}}.coverage.yes{{background:#00a67d;box-shadow:0 0 0 3px #00a67d22}}.coverage.no{{background:#8d979b}}footer{{padding:42px 0 70px;display:flex;justify-content:space-between;color:var(--muted);font:11px var(--mono)}}
.reveal{{animation:reveal .65s ease both}}@keyframes reveal{{from{{opacity:0;transform:translateY(12px)}}to{{opacity:1;transform:none}}}}@keyframes grow{{from{{transform:scaleX(0)}}to{{transform:scaleX(1)}}}}
@media(max-width:1100px){{.metrics{{grid-template-columns:repeat(3,minmax(0,1fr))}}.masthead{{grid-template-columns:1fr}}}}@media(max-width:760px){{.shell{{width:min(100% - 22px,1500px)}}.masthead{{padding-top:44px}}.section-head,.split,.chart-grid{{grid-template-columns:1fr}}.metrics{{grid-template-columns:repeat(2,minmax(0,1fr))}}.chart-grid .wide{{grid-column:auto}}section{{padding:50px 0}}footer{{display:block}}}}
@media print{{.nav,.theme{{display:none}}body{{background:white}}.chart,.metric,.node{{break-inside:avoid;box-shadow:none}}section{{padding:30px 0}}}}
</style>
</head>
<body>
<header class="shell masthead">
  <div><div class="kicker">Black-box platform characterization · report 01</div>
    <h1>System <em>Decap</em></h1>
    <p class="lede">Reconstruct a platform from software-visible behavior—without a datasheet. Every number keeps its method, scope, confidence and boundary conditions.</p>
  </div>
  <aside class="specimen"><div class="kicker">Specimen / target</div><h3>{_h(cpu.get('model', 'Unknown CPU'))}</h3>
    <dl><dt>Host</dt><dd>{_h(system.get('hostname'))}</dd><dt>Family</dt><dd>{_h(system.get('platform_family'))}</dd>
      <dt>Kernel</dt><dd>{_h(system.get('kernel'))}</dd><dt>Profile</dt><dd>{_h(run.get('profile'))}</dd>
      <dt>Run</dt><dd>{_h(run.get('started_at', '')[:19])}</dd></dl></aside>
</header>
<nav class="nav"><div class="shell nav-inner"><a href="#overview">Overview</a><a href="#topology">Topology</a><a href="#memory">Memory</a><a href="#numa">NUMA</a><a href="#core">Core</a><a href="#inventory">Inventory</a><a href="#raw">Raw data</a><a href="#method">Method</a><button class="theme" id="theme" title="Toggle theme">◐</button></div></nav>
<main class="shell">
<section id="overview"><div class="section-head"><div><div class="section-kicker">01 / Executive readout</div><h2>Platform<br>fingerprint</h2></div><p class="section-intro">Use these headline metrics for fast machine comparisons; hover to inspect the inference basis. High confidence means direct inventory or stable hardware counting. Medium and low values are black-box lower bounds, proxies or environment-limited results.</p></div>
  <div class="metrics">
    {_metric_card(estimates.get('memory.aggregate_read_bandwidth'), 'Aggregate read')}
    {_metric_card(estimates.get('memory.single_core_read_bandwidth'), 'Single-core read')}
    {_metric_card(estimates.get('memory.random_latency'), 'DRAM latency')}
    {_metric_card(estimates.get('core.max_observed_ipc'), 'Peak observed IPC')}
    {_metric_card(estimates.get('core.frontend_width_lower_bound'), 'Frontend lower bound')}
    {_metric_card(estimates.get('core.rob_capacity_proxy'), 'ROB window proxy')}
    {_metric_card(estimates.get('numa.remote_latency'), 'Remote NUMA')}
    {_metric_card(estimates.get('numa.interconnect_payload_bandwidth'), 'Interconnect payload')}
    {_metric_card(estimates.get('core.integer_add_lanes_lower_bound'), 'Integer add throughput')}
    {_metric_card(estimates.get('coherence.same-numa-different-core'), 'Core handoff')}
    {_metric_card(estimates.get('tlb.first_knee'), 'First TLB knee')}
    {_metric_card(estimates.get('branch.unpredictable_penalty'), 'Random branch cost')}
  </div>
</section>
<section id="topology"><div class="section-head"><div><div class="section-kicker">02 / Placement map</div><h2>Topology</h2></div><p class="section-intro">{_h(machine_name)} · {topology.get('sockets', 0)} socket(s), {topology.get('dies', 0)} die(s), {topology.get('numa_nodes', 0)} NUMA node(s), {topology.get('physical_cores', 0)} accessible physical cores and {topology.get('logical_cpus', 0)} logical CPUs.</p></div>
  {_topology_visual(system)}
</section>
<section id="memory"><div class="section-head"><div><div class="section-kicker">03 / Hierarchy</div><h2>Memory<br>system</h2></div><p class="section-intro">Dependent loads expose the latency hierarchy; parallel streams expose sustainable payload from one core to the accessible system. Working sets, thread counts and affinity remain attached to every raw point.</p></div>
  <div class="chart-grid"><div class="wide">{cache_curve}</div>{tlb_curve}{false_sharing_curve}<div class="wide">{cache_bandwidth_curve}</div>{stride_curve}{mlp_curve}<div class="wide">{bandwidth_curve}</div></div>
</section>
<section id="numa"><div class="section-head"><div><div class="section-kicker">04 / Fabric</div><h2>NUMA &<br>interconnect</h2></div><p class="section-intro">Pages are bound—or placed by pinned first touch—on each memory node, then accessed from every CPU node. Virtualization, containers or denied mbind permissions lower placement confidence.</p></div>
  <div class="chart-grid">{numa_latency}{numa_bandwidth}</div>
</section>
<section id="core"><div class="section-head"><div><div class="section-kicker">05 / Microarchitecture</div><h2>Core<br>engine</h2></div><p class="section-intro">Architecture-specific scalar assembly separates frontend flow, dependency latency and independent-chain throughput. IPC uses perf retired instructions/core cycles; counter-dependent estimates remain blank when the kernel denies access.</p></div>
  <div class="chart-grid">{pipeline_chart}{branch_chart}{core_chart}{rob_curve}{core_matrix}</div>
</section>
<section id="inventory"><div class="section-head"><div><div class="section-kicker">06 / Static evidence</div><h2>Inventory</h2></div><p class="section-intro">Sysfs and procfs are the platform evidence exposed to the OS. Capacity and topology are usually reliable, while firmware defects, cgroup affinity and virtualization can still change the visible scope.</p></div>
  {_inventory_tables(system)}
  <details><summary>ISA feature flags · {len(flags)} entries</summary><div class="flags">{flag_html}</div></details>
  <details><summary>DMI / firmware identity</summary><pre>{_h(json.dumps(dmi, ensure_ascii=False, indent=2))}</pre></details>
  <details><summary>Kernel vulnerability mitigations</summary><pre>{_h(json.dumps(system.get('environment', {}).get('vulnerabilities', {}), ensure_ascii=False, indent=2))}</pre></details>
</section>
<section id="raw"><div class="section-head"><div><div class="section-kicker">07 / Evidence ledger</div><h2>Raw<br>observations</h2></div><p class="section-intro">This run contains {len(report.get('observations', []))} raw observations. Search by group, metric, method or label. Full JSON and CSV sit beside this HTML file.</p></div>
  <input class="filter" id="filter" placeholder="Filter: cache_latency, operation=read, cpu_node=1 …" aria-label="Filter raw observations">
  <div class="table-scroll"><table class="data-table" id="raw-table"><thead><tr><th>Group</th><th>Metric / method</th><th>Value</th><th>Confidence</th><th>Labels</th></tr></thead><tbody>{_raw_table(report)}</tbody></table></div>
</section>
<section id="method"><div class="section-head"><div><div class="section-kicker">08 / Truth conditions</div><h2>Method &<br>limits</h2></div><p class="section-intro">Black-box characterization is not a literal chip decap: it answers what this software environment can observe. Frequency, firmware, kernel policy, virtualization, temperature, page migration and compiler behavior are all experimental conditions.</p></div>
  <div class="warning-box"><h3>Run warnings & qualification</h3><ul>{warning_html}</ul></div>
  <div class="chart-grid"><div class="wide">{os_chart}</div></div>
  <details open><summary>Coverage ledger</summary><div class="table-scroll"><table class="data-table compact"><thead><tr><th>Status</th><th>Category</th><th>Metric</th><th>Kind</th><th>Nominal confidence</th></tr></thead><tbody>{_coverage(report)}</tbody></table></div></details>
  <details><summary>Reproduction command</summary><pre>{_h(' '.join(map(str, run.get('command', []))))}</pre></details>
  <details><summary>Run metadata</summary><pre>{_h(json.dumps({'run': run, 'tool': report.get('tool'), 'native': report.get('native_metadata')}, ensure_ascii=False, indent=2))}</pre></details>
</section>
</main>
<footer class="shell"><span>System Decap v{_h(report.get('tool', {}).get('version', '?'))} · generated {generated}</span><span>{len(report.get('observations', []))} observations · {len(report.get('estimates', []))} estimates · source payload {_human_bytes(raw_json_size)}</span></footer>
<script>
const root=document.documentElement,button=document.getElementById('theme');
button.addEventListener('click',()=>{{root.dataset.theme=root.dataset.theme==='dark'?'light':'dark';}});
const filter=document.getElementById('filter');
filter.addEventListener('input',()=>{{const query=filter.value.toLowerCase().trim();document.querySelectorAll('#raw-table tbody tr').forEach(row=>{{row.hidden=query&&!row.dataset.search.includes(query)}});}});
</script>
</body></html>"""
