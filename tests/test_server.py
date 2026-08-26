import json
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.request import urlopen

from system_decap.server import create_server


class ReportServerTests(unittest.TestCase):
    def test_catalog_discovers_existing_json_reports_over_http(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report_dir = root / "turin-standard"
            report_dir.mkdir()
            (report_dir / "report.json").write_text(
                json.dumps({
                    "schema_version": "1.0",
                    "run": {"profile": "standard", "started_at": "2026-08-26T10:00:00+08:00"},
                    "system": {
                        "hostname": "turin-01",
                        "cpu": {"model": "AMD EPYC 9575F"},
                        "topology": {"physical_cores": 128, "logical_cpus": 256},
                    },
                    "observations": [{"metric": "stream_bandwidth"}],
                    "estimates": [{"key": "memory.aggregate_read_bandwidth"}],
                }),
                encoding="utf-8",
            )

            server = create_server(root, host="127.0.0.1", port=0)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                with urlopen(
                    f"http://127.0.0.1:{server.server_port}/api/reports", timeout=2
                ) as response:
                    payload = json.load(response)
            finally:
                server.shutdown()
                server.server_close()
                worker.join(timeout=2)

        self.assertEqual(len(payload["reports"]), 1)
        item = payload["reports"][0]
        self.assertEqual(item["hostname"], "turin-01")
        self.assertEqual(item["cpu_model"], "AMD EPYC 9575F")
        self.assertEqual(item["profile"], "standard")
        self.assertEqual(item["observation_count"], 1)
        self.assertTrue(item["id"])

    def test_report_detail_returns_the_selected_complete_json(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report_dir = root / "arm-quick"
            report_dir.mkdir()
            expected = {
                "schema_version": "1.0",
                "run": {"profile": "quick"},
                "system": {"hostname": "arm-01", "cpu": {"model": "Neoverse V2"}},
                "observations": [{"group": "timer", "value": 7.5}],
                "estimates": [],
            }
            (report_dir / "report.json").write_text(
                json.dumps(expected), encoding="utf-8"
            )

            server = create_server(root, host="127.0.0.1", port=0)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                with urlopen(
                    f"http://127.0.0.1:{server.server_port}/api/reports", timeout=2
                ) as response:
                    report_id = json.load(response)["reports"][0]["id"]
                with urlopen(
                    f"http://127.0.0.1:{server.server_port}/api/reports/{report_id}",
                    timeout=2,
                ) as response:
                    actual = json.load(response)
            finally:
                server.shutdown()
                server.server_close()
                worker.join(timeout=2)

        self.assertEqual(actual, expected)

    def test_browser_app_is_static_and_loads_report_data_from_the_api(self):
        with tempfile.TemporaryDirectory() as directory:
            server = create_server(Path(directory), host="127.0.0.1", port=0)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                base = f"http://127.0.0.1:{server.server_port}"
                with urlopen(base + "/", timeout=2) as response:
                    html = response.read().decode("utf-8")
                with urlopen(base + "/assets/app.js", timeout=2) as response:
                    script = response.read().decode("utf-8")
                for module in (
                    "model.js", "ui.js", "charts.js",
                    "single-report.js", "comparison.js",
                ):
                    with urlopen(base + f"/assets/{module}", timeout=2) as response:
                        self.assertEqual(response.status, 200)
            finally:
                server.shutdown()
                server.server_close()
                worker.join(timeout=2)

        self.assertIn('<main id="app"', html)
        self.assertIn("/assets/app.js", html)
        self.assertNotIn('"observations":', html)
        self.assertIn('fetch("/api/reports")', script)
        self.assertIn("Promise.all", script)
        self.assertIn("renderComparison", script)


if __name__ == "__main__":
    unittest.main()
