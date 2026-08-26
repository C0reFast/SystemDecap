import unittest
import json
import tempfile
from pathlib import Path
from subprocess import CompletedProcess
from unittest.mock import patch

from system_decap.runner import _bandwidth_validation_warnings, execute


class RunnerTests(unittest.TestCase):
    def test_invalid_bandwidth_points_become_visible_report_warnings(self):
        warnings = _bandwidth_validation_warnings({
            "memory_bandwidth": {
                "theoretical_upper_bound_gbps": 614.4,
                "above_theoretical_limit": 1,
                "cache_polluted_or_unverified": 2,
            },
            "numa_bandwidth": {"rejected_points": 3},
        })

        self.assertTrue(any("614.4 GB/s" in warning for warning in warnings))
        self.assertTrue(any("1 个" in warning and "理论上界" in warning for warning in warnings))
        self.assertTrue(any("NUMA" in warning and "3 个" in warning for warning in warnings))

    def test_run_persists_json_as_the_report_without_generating_embedded_html(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            build.mkdir()
            (build / "sdc-native").touch()
            output = root / "reports" / "host-run"
            native = {
                "metadata": {"selected_probes": ["timer"]},
                "observations": [{"group": "timer", "metric": "counter_frequency"}],
                "warnings": [],
            }
            system = {
                "hostname": "host",
                "platform_family": "x86_64",
                "cpu": {"model": "CPU", "flags": []},
                "topology": {},
                "commands": {},
            }
            process = CompletedProcess(
                args=[], returncode=0, stdout=json.dumps(native), stderr=""
            )

            with (
                patch("system_decap.runner.platform.system", return_value="Linux"),
                patch("system_decap.runner.platform.machine", return_value="x86_64"),
                patch("system_decap.runner.discover", return_value=system),
                patch("system_decap.runner.infer", return_value=([], {})),
                patch("system_decap.runner._tool_version", return_value="test"),
                patch("system_decap.runner.subprocess.run", return_value=process),
            ):
                result_dir, report = execute(
                    profile="smoke",
                    output_dir=output,
                    build_dir=build,
                    skip_build=True,
                    only_probes=["timer"],
                )

            self.assertEqual(result_dir, output.resolve())
            self.assertTrue((output / "report.json").is_file())
            self.assertFalse((output / "report.html").exists())
            self.assertEqual(report["observations"], native["observations"])


if __name__ == "__main__":
    unittest.main()
