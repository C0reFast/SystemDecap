"""Build, execute, and persist a complete System Decap run."""

from __future__ import annotations

import csv
import json
import os
import platform
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

from . import __version__
from .catalog import as_dicts
from .discover import discover, memory_bandwidth_override
from .infer import infer


ROOT = Path(__file__).resolve().parent.parent


def _tool_version(command: list[str]) -> str:
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=10, check=False)
        text = (result.stdout or result.stderr).strip()
        return text.splitlines()[0] if text else ""
    except (OSError, subprocess.TimeoutExpired):
        return ""


def build_native(build_dir: Path, clean: bool = False) -> Path:
    if clean and build_dir.exists():
        resolved = build_dir.resolve()
        protected = {Path("/").resolve(), Path.home().resolve(), ROOT.resolve()}
        cache = resolved / "CMakeCache.txt"
        expected_marker = f"CMAKE_HOME_DIRECTORY:INTERNAL={ROOT.resolve()}"
        if resolved in protected or not cache.is_file():
            raise RuntimeError(f"refusing to clean an unverified build directory: {resolved}")
        if expected_marker not in cache.read_text(errors="replace"):
            raise RuntimeError(f"refusing to clean a build directory owned by another source tree: {resolved}")
        shutil.rmtree(build_dir)
    generator = ["-G", "Ninja"] if shutil.which("ninja") else []
    configure = [
        "cmake", "-S", str(ROOT), "-B", str(build_dir), *generator,
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
    ]
    print("[system-decap] configure:", " ".join(configure), file=sys.stderr)
    subprocess.run(configure, check=True)
    print("[system-decap] build native probes", file=sys.stderr)
    subprocess.run(["cmake", "--build", str(build_dir), "--parallel"], check=True)
    binary = build_dir / "sdc-native"
    if not binary.is_file():
        raise RuntimeError(f"native binary was not produced: {binary}")
    return binary


def _write_csv(path: Path, observations: list[dict[str, Any]]) -> None:
    label_keys = sorted({key for item in observations for key in item.get("labels", {})})
    fields = ["group", "metric", "value", "unit", "confidence", "method", *label_keys]
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for item in observations:
            row = {key: item.get(key, "") for key in fields}
            row.update(item.get("labels", {}))
            writer.writerow(row)


def _default_output(hostname: str) -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    safe_host = "".join(c if c.isalnum() or c in "-." else "-" for c in hostname)
    return ROOT / "reports" / f"{safe_host}-{stamp}"


def _bandwidth_validation_warnings(diagnostics: dict[str, Any]) -> list[str]:
    warnings: list[str] = []
    memory = diagnostics.get("memory_bandwidth", {})
    upper = memory.get("theoretical_upper_bound_gbps")
    above = int(memory.get("above_theoretical_limit") or 0)
    cache_points = int(memory.get("cache_polluted_or_unverified") or 0)
    if upper is not None and above:
        warnings.append(
            f"内存配置理论上界为 {float(upper):g} GB/s；已从 DRAM 摘要排除 {above} 个"
            "超过上界 5% 容差的观测点"
        )
    if cache_points:
        warnings.append(
            f"已从 DRAM 摘要排除 {cache_points} 个工作集未确认达到整机 LLC 4 倍的读取点；"
            "这些点仍保留在原始曲线中"
        )
    numa_rejected = int(diagnostics.get("numa_bandwidth", {}).get("rejected_points") or 0)
    if numa_rejected:
        warnings.append(
            f"NUMA/互联带宽摘要排除了 {numa_rejected} 个未通过 4× LLC 或配置理论上界校验的点"
        )
    return warnings


def execute(
    profile: str = "standard",
    output_dir: Path | None = None,
    build_dir: Path | None = None,
    memory_mib: int | None = None,
    duration_ms: int | None = None,
    seed: int = 0x5DEC4A9,
    skip_build: bool = False,
    memory_channels: int | None = None,
    memory_mtps: int | None = None,
) -> tuple[Path, dict[str, Any]]:
    if platform.system() != "Linux":
        raise RuntimeError("System Decap currently requires Linux procfs/sysfs and perf_event_open")
    if platform.machine().lower() not in ("x86_64", "amd64", "aarch64", "arm64"):
        raise RuntimeError(f"unsupported architecture: {platform.machine()}")
    if (memory_channels is None) != (memory_mtps is None):
        raise ValueError("--memory-channels and --memory-mtps must be provided together")
    build_dir = (build_dir or ROOT / "build").resolve()
    binary = build_dir / "sdc-native"
    if not skip_build or not binary.exists():
        binary = build_native(build_dir)

    print("[system-decap] inventory procfs/sysfs", file=sys.stderr)
    started = datetime.now().astimezone()
    system = discover()
    if memory_channels is not None and memory_mtps is not None:
        system["memory_bandwidth_theoretical"] = memory_bandwidth_override(
            memory_channels, memory_mtps
        )
    output_dir = (output_dir or _default_output(system["hostname"])).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    command = [str(binary), "--profile", profile, "--seed", str(seed)]
    if memory_mib is not None:
        command += ["--memory-mib", str(memory_mib)]
    if duration_ms is not None:
        command += ["--duration-ms", str(duration_ms)]
    print("[system-decap] execute:", " ".join(command), file=sys.stderr)
    process = subprocess.run(command, stdout=subprocess.PIPE, stderr=None, text=True, check=False)
    if process.returncode != 0:
        (output_dir / "native-failed-output.txt").write_text(process.stdout, encoding="utf-8")
        raise RuntimeError(f"native probe exited with status {process.returncode}")
    try:
        native = json.loads(process.stdout)
    except json.JSONDecodeError as error:
        (output_dir / "native-invalid-output.txt").write_text(process.stdout, encoding="utf-8")
        raise RuntimeError(f"native probe produced invalid JSON: {error}") from error

    estimates, diagnostics = infer(system, native)
    report_warnings = list(native.get("warnings", []))
    report_warnings.extend(_bandwidth_validation_warnings(diagnostics))
    finished = datetime.now().astimezone()
    report = {
        "schema_version": "1.0",
        "tool": {
            "name": "System Decap",
            "version": __version__,
            "source_root": str(ROOT),
            "compiler": _tool_version([os.environ.get("CXX", "c++"), "--version"]),
            "cmake": _tool_version(["cmake", "--version"]),
        },
        "run": {
            "started_at": started.isoformat(),
            "finished_at": finished.isoformat(),
            "duration_seconds": (finished - started).total_seconds(),
            "profile": profile,
            "memory_channels_override": memory_channels,
            "memory_mtps_override": memory_mtps,
            "command": command,
            "output_directory": str(output_dir),
        },
        "system": system,
        "native_metadata": native.get("metadata", {}),
        "observations": native.get("observations", []),
        "estimates": estimates,
        "diagnostics": diagnostics,
        "warnings": report_warnings,
        "metric_catalog": as_dicts(),
    }
    (output_dir / "native-raw.json").write_text(
        json.dumps(native, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    (output_dir / "report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    _write_csv(output_dir / "observations.csv", report["observations"])
    (output_dir / "lscpu.txt").write_text(system.get("commands", {}).get("lscpu", ""), encoding="utf-8")
    from .report import render_report

    (output_dir / "report.html").write_text(render_report(report), encoding="utf-8")
    return output_dir, report
