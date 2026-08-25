import unittest

from system_decap.report import render_report


class ReportTests(unittest.TestCase):
    @staticmethod
    def _base_report(profile="smoke"):
        return {
            "tool": {"version": "test"},
            "run": {"profile": profile, "started_at": "2026-01-01", "command": ["probe"]},
            "system": {
                "hostname": "test-host", "platform_family": "arm64", "kernel": "test",
                "cpu": {"model": "Test CPU", "flags": []},
                "topology": {"cpus": [], "logical_cpus": 1, "physical_cores": 1,
                             "sockets": 1, "dies": 1, "numa_nodes": 1},
                "caches": [], "numa": [], "dmi": {}, "environment": {},
            },
            "observations": [], "estimates": [], "warnings": [], "metric_catalog": [],
        }

    def test_standalone_report_escapes_target_data(self):
        report = self._base_report()
        report["system"]["hostname"] = "host<script>"
        report["system"]["cpu"]["model"] = "CPU & model"
        rendered = render_report(report)
        self.assertIn("<!doctype html>", rendered)
        self.assertIn("host&lt;script&gt;", rendered)
        self.assertNotIn("host<script>", rendered)
        self.assertIn("System <em>Decap</em>", rendered)

    def test_standard_report_is_chinese_and_renders_physical_core_matrix(self):
        report = self._base_report("standard")
        report["observations"] = [
            {
                "group": "core_latency", "metric": "cacheline_handoff_latency",
                "value": value, "unit": "ns/one-way", "confidence": "high",
                "method": "release/acquire cache-line ping-pong",
                "labels": {
                    "cpu_a": str(cpu_a), "cpu_b": str(cpu_b), "node_a": "0", "node_b": "0",
                    "relation": "same-numa-different-core", "matrix_scope": "physical-core",
                },
            }
            for cpu_a, cpu_b, value in ((0, 2, 18.0), (0, 4, 20.0), (2, 4, 22.0))
        ]
        rendered = render_report(report)
        self.assertIn("01 / 核心摘要", rendered)
        self.assertIn("核间延迟矩阵（CPU × CPU）", rendered)
        self.assertIn('data-rows="3" data-columns="3"', rendered)
        self.assertEqual(rendered.count('<td class="measured"'), 6)
        self.assertIn("共实测 3 个无向核心对", rendered)


if __name__ == "__main__":
    unittest.main()
