#!/usr/bin/env python3
"""Black-box assertions for the independently runnable native probe interface."""

from __future__ import annotations

import json
import subprocess
import sys


def main() -> int:
    binary, mode = sys.argv[1:3]
    if mode == "list":
        result = subprocess.run(
            [binary, "--list-probes"], capture_output=True, text=True, check=True
        )
        names = {line.split("\t", 1)[0] for line in result.stdout.splitlines() if line}
        required = {"timer", "memory-bandwidth", "numa", "core-latency", "pipeline"}
        missing = required - names
        if missing:
            raise AssertionError(f"missing probes: {sorted(missing)}")
        return 0

    if mode == "timer-only":
        result = subprocess.run(
            [binary, "--profile", "smoke", "--only", "timer"],
            capture_output=True,
            text=True,
            check=True,
        )
        payload = json.loads(result.stdout)
        observations = payload.get("observations", [])
        if not observations:
            raise AssertionError("timer probe produced no observations")
        groups = {item["group"] for item in observations}
        if groups != {"timer"}:
            raise AssertionError(f"unexpected observation groups: {sorted(groups)}")
        if payload.get("metadata", {}).get("selected_probes") != ["timer"]:
            raise AssertionError("selected probe metadata is missing or incorrect")
        return 0

    if mode == "all-independent":
        listed = subprocess.run(
            [binary, "--list-probes"], capture_output=True, text=True, check=True
        )
        names = [line.split("\t", 1)[0] for line in listed.stdout.splitlines() if line]
        if not names:
            raise AssertionError("probe registry is empty")
        for name in names:
            result = subprocess.run(
                [
                    binary,
                    "--profile",
                    "smoke",
                    "--memory-mib",
                    "8",
                    "--duration-ms",
                    "15",
                    "--only",
                    name,
                ],
                capture_output=True,
                text=True,
                check=True,
            )
            payload = json.loads(result.stdout)
            selected = payload.get("metadata", {}).get("selected_probes")
            if selected != [name]:
                raise AssertionError(f"{name}: selected_probes={selected!r}")
        return 0

    raise AssertionError(f"unknown assertion mode: {mode}")


if __name__ == "__main__":
    raise SystemExit(main())
