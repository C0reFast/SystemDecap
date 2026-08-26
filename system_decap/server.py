"""HTTP report repository and browser application server."""

from __future__ import annotations

import hashlib
import json
import mimetypes
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit
import webbrowser


WEB_ROOT = Path(__file__).with_name("web")


class ReportRepository:
    """Read report JSON files from one configured repository root."""

    def __init__(self, root: Path):
        self.root = root.resolve()
        self._catalog_cache: dict[Path, tuple[int, int, dict[str, Any]]] = {}
        self._cache_lock = threading.RLock()

    @staticmethod
    def _report_id(relative_path: Path) -> str:
        return hashlib.sha256(relative_path.as_posix().encode("utf-8")).hexdigest()[:16]

    def _paths(self) -> list[tuple[str, Path]]:
        if not self.root.is_dir():
            return []
        paths: list[tuple[str, Path]] = []
        for path in self.root.rglob("report.json"):
            try:
                relative = path.resolve().relative_to(self.root)
            except (OSError, ValueError):
                continue
            paths.append((self._report_id(relative), path))
        return paths

    def list_reports(self) -> list[dict[str, Any]]:
        reports: list[dict[str, Any]] = []
        active_paths: set[Path] = set()
        for report_id, path in self._paths():
            try:
                resolved = path.resolve()
                stat = resolved.stat()
                active_paths.add(resolved)
                with self._cache_lock:
                    cached = self._catalog_cache.get(resolved)
                if cached and cached[0] == stat.st_mtime_ns and cached[1] == stat.st_size:
                    reports.append(dict(cached[2]))
                    continue
                report = json.loads(path.read_text(encoding="utf-8"))
                system = report.get("system", {})
                run = report.get("run", {})
                cpu = system.get("cpu", {})
                topology = system.get("topology", {})
                item = {
                    "id": report_id,
                    "name": path.parent.name,
                    "hostname": system.get("hostname", path.parent.name),
                    "cpu_model": cpu.get("model", ""),
                    "platform_family": system.get("platform_family", ""),
                    "profile": run.get("profile", ""),
                    "started_at": run.get("started_at", ""),
                    "physical_cores": topology.get("physical_cores"),
                    "logical_cpus": topology.get("logical_cpus"),
                    "observation_count": len(report.get("observations", [])),
                    "estimate_count": len(report.get("estimates", [])),
                    "schema_version": report.get("schema_version", ""),
                }
                reports.append(item)
                with self._cache_lock:
                    self._catalog_cache[resolved] = (stat.st_mtime_ns, stat.st_size, item)
            except (OSError, ValueError, TypeError):
                continue
        with self._cache_lock:
            self._catalog_cache = {
                path: cached
                for path, cached in self._catalog_cache.items()
                if path in active_paths
            }
        reports.sort(key=lambda item: str(item.get("started_at", "")), reverse=True)
        return reports

    def load_report(self, report_id: str) -> dict[str, Any] | None:
        for candidate_id, path in self._paths():
            if candidate_id != report_id:
                continue
            try:
                report = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError, TypeError):
                return None
            return report if isinstance(report, dict) else None
        return None


class ReportHTTPServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], repository: ReportRepository):
        super().__init__(address, ReportRequestHandler)
        self.repository = repository


class ReportRequestHandler(BaseHTTPRequestHandler):
    server: ReportHTTPServer

    def _json(self, payload: Any, status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.end_headers()
        self.wfile.write(body)

    def _asset(self, path: Path) -> None:
        try:
            body = path.read_bytes()
        except OSError:
            self._json({"error": "not found"}, HTTPStatus.NOT_FOUND)
            return
        content_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", f"{content_type}; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "connect-src 'self'; img-src 'self' data:; object-src 'none'; "
            "base-uri 'none'; frame-ancestors 'none'",
        )
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = urlsplit(self.path).path
        if path == "/api/reports":
            self._json({"reports": self.server.repository.list_reports()})
            return
        prefix = "/api/reports/"
        if path.startswith(prefix):
            report_id = path[len(prefix):]
            report = self.server.repository.load_report(report_id)
            if report is not None:
                self._json(report)
                return
        assets = {
            "/": WEB_ROOT / "index.html",
            "/index.html": WEB_ROOT / "index.html",
            "/assets/app.js": WEB_ROOT / "app.js",
            "/assets/model.js": WEB_ROOT / "model.js",
            "/assets/ui.js": WEB_ROOT / "ui.js",
            "/assets/charts.js": WEB_ROOT / "charts.js",
            "/assets/single-report.js": WEB_ROOT / "single-report.js",
            "/assets/comparison.js": WEB_ROOT / "comparison.js",
            "/assets/styles.css": WEB_ROOT / "styles.css",
        }
        if path in assets:
            self._asset(assets[path])
            return
        self._json({"error": "not found"}, HTTPStatus.NOT_FOUND)

    def log_message(self, format: str, *args: Any) -> None:
        return


def create_server(
    reports_dir: Path, host: str = "127.0.0.1", port: int = 8000
) -> ReportHTTPServer:
    return ReportHTTPServer((host, port), ReportRepository(reports_dir))


def serve_reports(
    reports_dir: Path,
    host: str = "127.0.0.1",
    port: int = 8000,
    open_browser: bool = False,
) -> None:
    server = create_server(reports_dir, host=host, port=port)
    browser_host = "127.0.0.1" if host in {"0.0.0.0", "::"} else host
    url = f"http://{browser_host}:{server.server_port}/"
    print(f"[system-decap] 报告仓库：{reports_dir.resolve()}")
    print(f"[system-decap] 浏览器地址：{url}")
    if host not in {"127.0.0.1", "::1", "localhost"}:
        print("[system-decap] 警告：当前监听非本机地址，内置服务不提供身份认证")
    if open_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[system-decap] 报告服务已停止")
    finally:
        server.server_close()
