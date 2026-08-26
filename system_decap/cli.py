"""Command-line interface."""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from pathlib import Path

from .catalog import as_dicts
from .discover import discover
from .runner import ROOT, build_native, execute
from .server import serve_reports


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="system-decap",
        description="Black-box CPU, cache, memory, NUMA and interconnect characterization for Linux servers.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    run = subparsers.add_parser("run", help="build probes, execute a suite, and generate reports")
    run.add_argument("--profile", choices=["smoke", "quick", "standard", "deep"], default="standard")
    run.add_argument(
        "--only",
        action="append",
        help="run only the named native probe(s); accepts comma-separated names and may repeat",
    )
    run.add_argument("--output", type=Path, help="report directory (default: reports/<host>-<time>)")
    run.add_argument("--build-dir", type=Path, default=ROOT / "build")
    run.add_argument("--memory-mib", type=int, help="bytes per STREAM array, in MiB")
    run.add_argument("--memory-channels", type=int, help="installed 64-bit memory channels")
    run.add_argument("--memory-mtps", type=int, help="configured memory transfer rate, MT/s")
    run.add_argument("--duration-ms", type=int, help="minimum duration of each throughput point")
    run.add_argument("--seed", type=int, default=0x5DEC4A9)
    run.add_argument("--skip-build", action="store_true")

    build = subparsers.add_parser("build", help="compile the native probe binary")
    build.add_argument("--build-dir", type=Path, default=ROOT / "build")
    build.add_argument("--clean", action="store_true")

    serve = subparsers.add_parser(
        "serve", help="serve JSON reports in the browser comparison workspace"
    )
    serve.add_argument("--reports-dir", type=Path, default=ROOT / "reports")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8000)
    serve.add_argument("--open-browser", action="store_true")

    inventory = subparsers.add_parser("inventory", help="print system inventory JSON without benchmarks")
    inventory.add_argument("--output", type=Path)

    subparsers.add_parser("list-metrics", help="print the measurement catalog")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "build":
            binary = build_native(args.build_dir.resolve(), args.clean)
            print(binary)
            return 0
        if args.command == "inventory":
            text = json.dumps(discover(), ensure_ascii=False, indent=2)
            if args.output:
                args.output.write_text(text, encoding="utf-8")
                print(args.output.resolve())
            else:
                print(text)
            return 0
        if args.command == "serve":
            serve_reports(
                args.reports_dir,
                host=args.host,
                port=args.port,
                open_browser=args.open_browser,
            )
            return 0
        if args.command == "list-metrics":
            for item in as_dicts():
                print(f"{item['category']:12} {item['kind']:10} {item['nominal_confidence']:8} {item['metric']}")
            return 0
        if args.command == "run":
            memory_mib = args.memory_mib
            if memory_mib is None and args.profile == "standard":
                memory_mib = 8192
            output, report = execute(
                profile=args.profile,
                output_dir=args.output,
                build_dir=args.build_dir,
                memory_mib=memory_mib,
                duration_ms=args.duration_ms,
                seed=args.seed,
                skip_build=args.skip_build,
                memory_channels=args.memory_channels,
                memory_mtps=args.memory_mtps,
                only_probes=args.only,
            )
            print(f"JSON: {output / 'report.json'}")
            print(f"CSV:  {output / 'observations.csv'}")
            print(
                "Web:  ./system-decap serve "
                f"--reports-dir {shlex.quote(str(output.parent))} --open-browser"
            )
            print(f"Observations: {len(report['observations'])}, estimates: {len(report['estimates'])}")
            return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"system-decap: {error}", file=sys.stderr)
        return 1
    return 2
