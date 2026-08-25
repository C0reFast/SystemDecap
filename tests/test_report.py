import unittest

from system_decap.report import render_report


class ReportTests(unittest.TestCase):
    def test_standalone_report_escapes_target_data(self):
        report = {
            "tool": {"version": "test"},
            "run": {"profile": "smoke", "started_at": "2026-01-01", "command": ["probe"]},
            "system": {
                "hostname": "host<script>", "platform_family": "arm64", "kernel": "test",
                "cpu": {"model": "CPU & model", "flags": []},
                "topology": {"cpus": [], "logical_cpus": 1, "physical_cores": 1,
                             "sockets": 1, "dies": 1, "numa_nodes": 1},
                "caches": [], "numa": [], "dmi": {}, "environment": {},
            },
            "observations": [], "estimates": [], "warnings": [], "metric_catalog": [],
        }
        rendered = render_report(report)
        self.assertIn("<!doctype html>", rendered)
        self.assertIn("host&lt;script&gt;", rendered)
        self.assertNotIn("host<script>", rendered)
        self.assertIn("System <em>Decap</em>", rendered)


if __name__ == "__main__":
    unittest.main()
