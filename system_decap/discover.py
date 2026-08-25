"""Linux system inventory using procfs/sysfs and optional common utilities."""

from __future__ import annotations

import glob
import json
import os
import platform
import re
import shutil
import socket
import subprocess
from pathlib import Path
from typing import Any


def _read(path: str | Path, default: str = "") -> str:
    try:
        return Path(path).read_text(errors="replace").strip()
    except (OSError, PermissionError):
        return default


def _integer(path: str | Path, default: int | None = None) -> int | None:
    try:
        return int(_read(path))
    except (TypeError, ValueError):
        return default


def _size_bytes(value: str) -> int | None:
    match = re.fullmatch(r"\s*(\d+)\s*([KMGT]?)B?\s*", value, re.I)
    if not match:
        return None
    shifts = {"": 0, "K": 10, "M": 20, "G": 30, "T": 40}
    return int(match.group(1)) << shifts[match.group(2).upper()]


def _cpu_list(value: str) -> list[int]:
    result: list[int] = []
    for part in value.strip().split(","):
        if not part:
            continue
        bounds = part.split("-", 1)
        try:
            if len(bounds) == 1:
                result.append(int(bounds[0]))
            else:
                result.extend(range(int(bounds[0]), int(bounds[1]) + 1))
        except ValueError:
            continue
    return sorted(set(result))


def _command(argv: list[str], timeout: float = 5.0) -> str:
    if not shutil.which(argv[0]):
        return ""
    try:
        return subprocess.run(
            argv,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
            env={**os.environ, "LC_ALL": "C"},
        ).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def _cpuinfo() -> dict[str, Any]:
    blocks = _read("/proc/cpuinfo").split("\n\n")
    records: list[dict[str, str]] = []
    for block in blocks:
        record: dict[str, str] = {}
        for line in block.splitlines():
            if ":" in line:
                key, value = line.split(":", 1)
                record[key.strip()] = value.strip()
        if record:
            records.append(record)
    first = records[0] if records else {}
    flags = first.get("flags", first.get("Features", "")).split()
    vendor = first.get("vendor_id", first.get("CPU implementer", "unknown"))
    model = first.get("model name", first.get("Processor", first.get("Hardware", "unknown")))
    return {
        "vendor": vendor,
        "model": model,
        "family": first.get("cpu family", first.get("CPU architecture", "")),
        "model_id": first.get("model", first.get("CPU part", "")),
        "stepping": first.get("stepping", first.get("CPU revision", "")),
        "microcode": first.get("microcode", ""),
        "flags": sorted(set(flags)),
        "reported_processors": len(records),
    }


def _platform_family(machine: str, vendor: str) -> str:
    lower = vendor.lower()
    if machine in ("x86_64", "amd64"):
        if "hygon" in lower:
            return "c86-hygon"
        return "x86_64"
    if machine in ("aarch64", "arm64"):
        return "arm64"
    return machine


def _topology() -> dict[str, Any]:
    try:
        allowed = sorted(os.sched_getaffinity(0))
    except AttributeError:
        allowed = list(range(os.cpu_count() or 1))
    online = _cpu_list(_read("/sys/devices/system/cpu/online", "0"))
    cpus: list[dict[str, Any]] = []
    for cpu in online:
        root = Path(f"/sys/devices/system/cpu/cpu{cpu}")
        topo = root / "topology"
        node = None
        for candidate in root.glob("node[0-9]*"):
            try:
                node = int(candidate.name[4:])
                break
            except ValueError:
                pass
        cpus.append(
            {
                "cpu": cpu,
                "allowed": cpu in allowed,
                "online": _integer(root / "online", 1) != 0,
                "socket": _integer(topo / "physical_package_id", 0),
                "die": _integer(topo / "die_id", 0),
                "cluster": _integer(topo / "cluster_id", None),
                "core": _integer(topo / "core_id", cpu),
                "node": node if node is not None else 0,
                "thread_siblings": _cpu_list(_read(topo / "thread_siblings_list", str(cpu))),
                "core_siblings": _cpu_list(_read(topo / "core_siblings_list", str(cpu))),
                "capacity": _integer(root / "cpu_capacity", None),
            }
        )
    allowed_records = [c for c in cpus if c["allowed"] and c["online"]]
    cores = {(c["socket"], c["die"], c["core"]) for c in allowed_records}
    dies = {(c["socket"], c["die"]) for c in allowed_records}
    sockets = {c["socket"] for c in allowed_records}
    nodes = {c["node"] for c in allowed_records}
    return {
        "online_cpu_list": online,
        "allowed_cpu_list": allowed,
        "logical_cpus": len(allowed_records),
        "physical_cores": len(cores),
        "dies": len(dies),
        "sockets": len(sockets),
        "numa_nodes": len(nodes),
        "threads_per_core": round(len(allowed_records) / max(1, len(cores)), 2),
        "cpus": cpus,
    }


def _caches(allowed: set[int]) -> list[dict[str, Any]]:
    found: dict[tuple[Any, ...], dict[str, Any]] = {}
    for path in glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cache/index[0-9]*"):
        root = Path(path)
        shared = _cpu_list(_read(root / "shared_cpu_list"))
        if shared and not allowed.intersection(shared):
            continue
        item = {
            "level": _integer(root / "level", 0),
            "type": _read(root / "type", "Unknown"),
            "size_bytes": _size_bytes(_read(root / "size", "0")) or 0,
            "line_bytes": _integer(root / "coherency_line_size", None),
            "ways": _integer(root / "ways_of_associativity", None),
            "sets": _integer(root / "number_of_sets", None),
            "partitions": _integer(root / "physical_line_partition", None),
            "shared_cpus": shared,
        }
        key = (item["level"], item["type"], item["size_bytes"], tuple(shared))
        found[key] = item
    return sorted(found.values(), key=lambda x: (x["level"], x["type"], x["shared_cpus"]))


def _numa() -> list[dict[str, Any]]:
    nodes: list[dict[str, Any]] = []
    for path in sorted(glob.glob("/sys/devices/system/node/node[0-9]*")):
        root = Path(path)
        try:
            node_id = int(root.name[4:])
        except ValueError:
            continue
        meminfo: dict[str, int] = {}
        for line in _read(root / "meminfo").splitlines():
            match = re.search(r"Node\s+\d+\s+(\S+):\s+(\d+)\s*kB", line)
            if match:
                meminfo[match.group(1)] = int(match.group(2)) * 1024
        nodes.append(
            {
                "node": node_id,
                "cpus": _cpu_list(_read(root / "cpulist")),
                "distance": [int(x) for x in _read(root / "distance").split() if x.isdigit()],
                "memory": meminfo,
                "hugepages": {
                    Path(p).name: _integer(Path(p) / "nr_hugepages", 0)
                    for p in glob.glob(str(root / "hugepages" / "hugepages-*"))
                },
            }
        )
    return nodes


def _frequencies(allowed: list[int]) -> dict[str, Any]:
    policies: dict[str, Any] = {}
    for path in glob.glob("/sys/devices/system/cpu/cpufreq/policy[0-9]*"):
        root = Path(path)
        policies[root.name] = {
            "cpus": _cpu_list(_read(root / "affected_cpus")),
            "driver": _read(root / "scaling_driver"),
            "governor": _read(root / "scaling_governor"),
            "min_khz": _integer(root / "scaling_min_freq", None),
            "max_khz": _integer(root / "scaling_max_freq", None),
            "hardware_min_khz": _integer(root / "cpuinfo_min_freq", None),
            "hardware_max_khz": _integer(root / "cpuinfo_max_freq", None),
        }
    current = {
        str(cpu): _integer(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq", None)
        for cpu in allowed
    }
    return {"policies": policies, "current_khz": current}


def _dmi() -> dict[str, str]:
    root = Path("/sys/class/dmi/id")
    fields = [
        "sys_vendor", "product_name", "product_version", "product_serial",
        "board_vendor", "board_name", "board_version", "bios_vendor",
        "bios_version", "bios_date", "chassis_type",
    ]
    return {field: _read(root / field) for field in fields if _read(root / field)}


def _environment() -> dict[str, Any]:
    vulnerabilities = {
        Path(path).name: _read(path)
        for path in glob.glob("/sys/devices/system/cpu/vulnerabilities/*")
    }
    thermal = {
        Path(path).parent.name: _integer(path, None)
        for path in glob.glob("/sys/class/thermal/thermal_zone*/temp")
    }
    block = []
    for path in sorted(glob.glob("/sys/block/*")):
        root = Path(path)
        sectors = _integer(root / "size", 0) or 0
        block.append(
            {
                "name": root.name,
                "size_bytes": sectors * 512,
                "rotational": _integer(root / "queue/rotational", None),
                "model": _read(root / "device/model"),
                "numa_node": _integer(root / "device/numa_node", None),
            }
        )
    network = []
    for path in sorted(glob.glob("/sys/class/net/*")):
        root = Path(path)
        network.append(
            {
                "name": root.name,
                "operstate": _read(root / "operstate"),
                "speed_mbps": _integer(root / "speed", None),
                "mtu": _integer(root / "mtu", None),
                "numa_node": _integer(root / "device/numa_node", None),
            }
        )
    return {
        "page_size": os.sysconf("SC_PAGE_SIZE"),
        "clock_ticks": os.sysconf("SC_CLK_TCK"),
        "perf_event_paranoid": _integer("/proc/sys/kernel/perf_event_paranoid", None),
        "kptr_restrict": _integer("/proc/sys/kernel/kptr_restrict", None),
        "transparent_hugepage": _read("/sys/kernel/mm/transparent_hugepage/enabled"),
        "clocksource": _read("/sys/devices/system/clocksource/clocksource0/current_clocksource"),
        "vulnerabilities": vulnerabilities,
        "thermal_millicelsius": thermal,
        "block_devices": block,
        "network_interfaces": network,
        "container": bool(Path("/.dockerenv").exists() or "container" in _read("/proc/1/environ")),
    }


def discover() -> dict[str, Any]:
    """Return a JSON-serializable, best-effort inventory of the current Linux host."""
    cpu = _cpuinfo()
    machine = platform.machine().lower()
    topology = _topology()
    allowed = set(topology["allowed_cpu_list"])
    meminfo: dict[str, int] = {}
    for line in _read("/proc/meminfo").splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        match = re.search(r"(\d+)", value)
        if match:
            meminfo[key] = int(match.group(1)) * (1024 if "kB" in value else 1)
    return {
        "hostname": socket.gethostname(),
        "architecture": machine,
        "platform_family": _platform_family(machine, cpu["vendor"]),
        "kernel": platform.release(),
        "operating_system": platform.platform(),
        "python": platform.python_version(),
        "cpu": cpu,
        "topology": topology,
        "caches": _caches(allowed),
        "numa": _numa(),
        "memory": meminfo,
        "frequency": _frequencies(topology["allowed_cpu_list"]),
        "dmi": _dmi(),
        "environment": _environment(),
        "commands": {
            "lscpu": _command(["lscpu"]),
            "numactl_hardware": _command(["numactl", "--hardware"]),
        },
    }


if __name__ == "__main__":
    print(json.dumps(discover(), ensure_ascii=False, indent=2))
