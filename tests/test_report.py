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
        self.assertIn("data-matrix-viewer", rendered)
        self.assertIn('data-zoom-action="fit"', rendered)
        self.assertIn('data-zoom-action="actual"', rendered)
        self.assertIn('min="1" max="200" step="1" value="100" data-zoom-range', rendered)
        self.assertEqual(rendered.count('data-default-hidden="true" hidden'), 3)
        self.assertIn("显示核间延迟明细（3 条）", rendered)
        self.assertIn("当前显示 0 / 总计 3 条", rendered)

    def test_256_cpu_core_matrix_remains_complete_and_raw_rows_start_hidden(self):
        report = self._base_report("deep")
        report["observations"] = [
            {
                "group": "core_latency", "metric": "cacheline_handoff_latency",
                "value": 18.0 + cpu_b / 100, "unit": "ns/one-way", "confidence": "high",
                "method": "release/acquire cache-line ping-pong",
                "labels": {
                    "cpu_a": "0", "cpu_b": str(cpu_b), "node_a": "0", "node_b": "0",
                    "relation": "same-numa-different-core", "matrix_scope": "logical-cpu",
                },
            }
            for cpu_b in range(1, 256)
        ]

        rendered = render_report(report)

        self.assertIn('data-rows="256" data-columns="256"', rendered)
        self.assertIn("256 × 256 · 拖动平移", rendered)
        self.assertEqual(rendered.count('<td class="measured"'), 510)
        self.assertEqual(rendered.count('data-default-hidden="true" hidden'), 255)
        self.assertIn("显示核间延迟明细（255 条）", rendered)
        self.assertIn("当前显示 0 / 总计 255 条", rendered)

    def test_microbenchmark_parity_charts_have_chinese_titles(self):
        report = self._base_report("quick")
        report["observations"] = [
            {"group": "loaded_memory_latency", "metric": "random_load_latency_under_load",
             "value": 91.0, "unit": "ns/access", "confidence": "medium", "method": "probe",
             "labels": {"load_threads": "0", "measured_load_gbps": "0"}},
            {"group": "page_policy", "metric": "random_load_latency", "value": 80.0,
             "unit": "ns/access", "confidence": "medium", "method": "probe",
             "labels": {"policy": "thp-advised", "anon_huge_bytes": "2097152"}},
            {"group": "store_forwarding", "metric": "store_load_latency", "value": 3.2,
             "unit": "ns/pair", "confidence": "medium", "method": "probe",
             "labels": {"case": "partial-4-to-8"}},
            {"group": "instruction_fetch", "metric": "code_delivery_bandwidth", "value": 48.0,
             "unit": "GB/s", "confidence": "medium", "method": "probe",
             "labels": {"working_set_bytes": "32768", "instruction_bytes": "4"}},
            {"group": "compute_scaling", "metric": "integer_add_throughput", "value": 20.0,
             "unit": "Gop/s", "confidence": "medium", "method": "probe",
             "labels": {"threads": "1", "scope": "physical-cores"}},
            {"group": "branch_structure", "metric": "btb_branch_latency", "value": 0.5,
             "unit": "ns/branch", "confidence": "low", "method": "probe",
             "labels": {"branch_count": "64", "spacing_bytes": "16",
                        "branch_type": "unconditional"}},
        ]
        rendered = render_report(report)
        for title in (
            "带宽压力下的随机内存延迟", "基础页与透明大页策略对比",
            "Store-to-load forwarding 对齐代价", "指令侧代码输送吞吐与足迹",
            "整数微内核的多核与 SMT 吞吐扩展", "BTB 无条件跳转足迹压力",
            "BTB 始终跳转条件分支足迹压力",
            "返回地址栈（RAS）深度压力", "间接分支目标数量压力",
        ):
            self.assertIn(title, rendered)

    def test_unavailable_metric_card_shows_the_specific_failure_reason(self):
        report = self._base_report("standard")
        report["estimates"] = [{
            "key": "core.max_observed_ipc",
            "name": "最大实测退休 IPC",
            "value": None,
            "unit": "instructions/cycle",
            "confidence": "unavailable",
            "basis": "核心 PMU 组运行失败：time_running 为 0（事件无法调度）",
            "caveat": "perf_event_paranoid=2，nmi_watchdog=1",
            "category": "core",
            "available": False,
        }]

        rendered = render_report(report)

        self.assertIn("核心 PMU 组运行失败：time_running 为 0（事件无法调度）", rendered)
        self.assertIn("perf_event_paranoid=2，nmi_watchdog=1", rendered)
        self.assertNotIn("请查看运行警告", rendered)

    def test_memory_inventory_and_theoretical_limit_are_rendered_in_chinese(self):
        report = self._base_report("standard")
        report["system"]["memory_devices"] = [{
            "locator": "DIMM_A1", "bank_locator": "BANK_0", "type": "DDR5",
            "size_bytes": 32 * 1024**3, "data_width_bits": 64,
            "configured_speed_mtps": 6400,
        }]
        report["system"]["memory_bandwidth_theoretical"] = {
            "upper_bound_gbps": 614.4, "installed_devices": 12, "rated_devices": 12,
            "source": "SMBIOS 已安装内存设备数据位宽 × 配置速率之和",
        }

        rendered = render_report(report)

        self.assertIn("内存设备与理论上界", rendered)
        self.assertIn("DIMM_A1", rendered)
        self.assertIn("6400 MT/s", rendered)
        self.assertIn("614.4 GB/s", rendered)


if __name__ == "__main__":
    unittest.main()
