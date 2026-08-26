import unittest

from system_decap.infer import infer


class InferTests(unittest.TestCase):
    def setUp(self):
        self.system = {
            "topology": {
                "allowed_cpu_list": [0, 1], "logical_cpus": 2, "physical_cores": 2,
                "sockets": 1, "numa_nodes": 2,
            },
            "caches": [
                {"level": 1, "type": "Data", "size_bytes": 32768, "shared_cpus": [0]},
                {"level": 2, "type": "Unified", "size_bytes": 1048576, "shared_cpus": [0]},
            ],
        }
        self.native = {"observations": [
            {"group": "cache_latency", "metric": "random_load_latency", "value": 1.0,
             "unit": "ns/access", "labels": {"working_set_bytes": "16384"}},
            {"group": "cache_latency", "metric": "random_load_latency", "value": 4.0,
             "unit": "ns/access", "labels": {"working_set_bytes": "65536"}},
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 12.5,
             "unit": "GB/s", "labels": {"operation": "read", "threads": "1",
                                                  "working_set_exceeds_llc": "true"}},
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 41.0,
             "unit": "GB/s", "labels": {"operation": "read", "threads": "2",
                                                  "working_set_exceeds_llc": "true"}},
            {"group": "numa", "metric": "load_latency", "value": 80.0,
             "unit": "ns/access", "labels": {"local": "true"}},
            {"group": "numa", "metric": "load_latency", "value": 144.0,
             "unit": "ns/access", "labels": {"local": "false"}},
        ]}

    def test_bandwidth_and_numa_summary(self):
        estimates, _ = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}
        self.assertEqual(by_key["memory.single_core_read_bandwidth"]["value"], 12.5)
        self.assertEqual(by_key["memory.aggregate_read_bandwidth"]["value"], 41.0)
        self.assertAlmostEqual(by_key["numa.remote_latency_penalty"]["value"], 1.8)

    def test_cache_sized_bandwidth_is_not_published_as_dram(self):
        for item in self.native["observations"]:
            if item["group"] == "memory_bandwidth":
                item["labels"].update({
                    "working_set_bytes": "268435456",
                    "aggregate_llc_bytes": "524288000",
                    "working_set_exceeds_llc": "false",
                })

        estimates, _ = infer(self.system, self.native)
        aggregate = next(
            item for item in estimates
            if item["key"] == "memory.aggregate_read_bandwidth"
        )

        self.assertFalse(aggregate["available"])
        self.assertIsNone(aggregate["value"])
        self.assertIn("没有工作集明确越过", aggregate["basis"])
        single = next(
            item for item in estimates
            if item["key"] == "memory.single_core_read_bandwidth"
        )
        self.assertFalse(single["available"])
        self.assertIsNone(single["value"])

    def test_cache_polluted_peak_is_excluded_from_dram_summary(self):
        self.native["observations"].extend([
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 1452.0,
             "unit": "GB/s", "confidence": "low",
             "labels": {"operation": "read", "threads": "128",
                        "working_set_exceeds_llc": "false"}},
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 580.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"operation": "read", "threads": "128",
                        "working_set_exceeds_llc": "true"}},
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 900.0,
             "unit": "GB/s", "confidence": "low",
             "labels": {"operation": "read", "threads": "1",
                        "working_set_exceeds_llc": "false"}},
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 30.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"operation": "read", "threads": "1",
                        "working_set_exceeds_llc": "true"}},
        ])

        estimates, _ = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}

        self.assertEqual(by_key["memory.aggregate_read_bandwidth"]["value"], 580.0)
        self.assertEqual(by_key["memory.single_core_read_bandwidth"]["value"], 30.0)

    def test_bandwidth_above_inventory_upper_bound_is_excluded(self):
        self.system["memory_bandwidth_theoretical"] = {
            "upper_bound_gbps": 614.4,
            "source": "SMBIOS 已安装内存设备数据位宽 × 配置速率之和",
        }
        self.native["observations"].extend([
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 1452.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"operation": "read", "threads": "128",
                        "working_set_exceeds_llc": "true"}},
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 580.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"operation": "read", "threads": "128",
                        "working_set_exceeds_llc": "true"}},
        ])

        estimates, diagnostics = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}

        self.assertEqual(by_key["memory.theoretical_peak_bandwidth"]["value"], 614.4)
        self.assertEqual(by_key["memory.aggregate_read_bandwidth"]["value"], 580.0)
        self.assertIn("排除 1 个超出", by_key["memory.aggregate_read_bandwidth"]["caveat"])
        self.assertEqual(diagnostics["memory_bandwidth"]["above_theoretical_limit"], 1)

    def test_legacy_two_x_llc_label_is_revalidated_against_four_x_threshold(self):
        self.native["observations"].extend([
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 1452.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"operation": "read", "threads": "128",
                        "working_set_bytes": str(512 * 1024**2),
                        "aggregate_llc_bytes": str(256 * 1024**2),
                        "working_set_exceeds_llc": "true"}},
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 580.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"operation": "read", "threads": "128",
                        "working_set_bytes": str(1024 * 1024**2),
                        "aggregate_llc_bytes": str(256 * 1024**2),
                        "working_set_exceeds_llc": "true"}},
        ])

        estimates, _ = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}

        self.assertEqual(by_key["memory.aggregate_read_bandwidth"]["value"], 580.0)

    def test_only_above_theoretical_points_make_summary_unavailable(self):
        self.system["memory_bandwidth_theoretical"] = {"upper_bound_gbps": 614.4}
        self.native["observations"] = [
            item for item in self.native["observations"]
            if item["group"] != "memory_bandwidth"
        ]
        self.native["observations"].append({
            "group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 1452.0,
            "unit": "GB/s", "confidence": "high",
            "labels": {"operation": "read", "threads": "128",
                       "working_set_exceeds_llc": "true"},
        })

        estimates, _ = infer(self.system, self.native)
        aggregate = next(
            item for item in estimates
            if item["key"] == "memory.aggregate_read_bandwidth"
        )

        self.assertFalse(aggregate["available"])
        self.assertIn("全部超过", aggregate["basis"])

    def test_cache_inventory_becomes_estimate(self):
        estimates, diagnostics = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}
        self.assertEqual(by_key["cache.l1.capacity"]["value"], 32768)
        self.assertTrue(diagnostics["cache_knees"])

    def test_single_page_count_latency_spike_is_not_a_tlb_knee(self):
        self.native["observations"].extend(
            {
                "group": "tlb_latency",
                "metric": "page_random_load_latency",
                "value": latency,
                "unit": "ns/access",
                "labels": {"pages": str(pages)},
            }
            for pages, latency in ((8, 1.0), (16, 3.0), (32, 1.1), (64, 1.1))
        )

        estimates, diagnostics = infer(self.system, self.native)
        keys = {item["key"] for item in estimates}

        self.assertEqual(diagnostics["tlb_knees"], [])
        self.assertNotIn("tlb.first_knee", keys)

    def test_sustained_page_latency_increase_is_a_tlb_knee(self):
        self.native["observations"].extend(
            {
                "group": "tlb_latency",
                "metric": "page_random_load_latency",
                "value": latency,
                "unit": "ns/access",
                "labels": {"pages": str(pages)},
            }
            for pages, latency in (
                (32, 1.0), (64, 1.1), (128, 2.2), (256, 2.4), (512, 2.5)
            )
        )

        estimates, diagnostics = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}

        self.assertEqual(diagnostics["tlb_knees"][0]["x"], 128)
        self.assertEqual(by_key["tlb.first_knee"]["value"], 128)

    def test_new_memory_and_branch_proxies_remain_qualified(self):
        self.native["observations"].extend([
            {"group": "page_policy", "metric": "random_load_latency", "value": 100.0,
             "labels": {"policy": "base-page-advised"}},
            {"group": "page_policy", "metric": "random_load_latency", "value": 80.0,
             "labels": {"policy": "thp-advised"}},
            {"group": "loaded_memory_latency", "metric": "random_load_latency_under_load",
             "value": 90.0, "labels": {"load_threads": "0"}},
            {"group": "loaded_memory_latency", "metric": "random_load_latency_under_load",
             "value": 135.0, "labels": {"load_threads": "4"}},
            {"group": "store_forwarding", "metric": "store_load_latency", "value": 1.0,
             "labels": {"case": "exact-8-to-8"}},
            {"group": "store_forwarding", "metric": "store_load_latency", "value": 4.0,
             "labels": {"case": "partial-4-to-8"}},
        ])
        self.native["observations"].extend(
            {"group": "branch_structure", "metric": "btb_branch_latency", "value": value,
             "labels": {"branch_count": str(count), "spacing_bytes": "16",
                        "branch_type": "unconditional"}}
            for count, value in ((16, 1.0), (32, 1.0), (64, 1.0), (128, 1.6))
        )
        estimates, diagnostics = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}
        self.assertAlmostEqual(by_key["memory.thp_latency_ratio"]["value"], 0.8)
        self.assertAlmostEqual(by_key["memory.loaded_latency_slowdown"]["value"], 1.5)
        self.assertAlmostEqual(
            by_key["memory.store_forwarding_penalty.partial-4-to-8"]["value"], 3.0
        )
        self.assertEqual(by_key["branch.btb_footprint_knee"]["value"], 128)
        self.assertEqual(by_key["branch.btb_footprint_knee"]["confidence"], "low")
        self.assertEqual(diagnostics["branch_structure_knees"]["btb"]["x"], 128)

    def test_numa_same_socket_and_cross_socket_paths_are_reported_separately(self):
        self.native["observations"].extend([
            {"group": "numa", "metric": "load_latency", "value": 125.0,
             "unit": "ns/access", "confidence": "high",
             "labels": {"local": "false", "relation": "cross-numa-same-socket"}},
            {"group": "numa", "metric": "load_latency", "value": 255.0,
             "unit": "ns/access", "confidence": "high",
             "labels": {"local": "false", "relation": "cross-socket"}},
            {"group": "numa", "metric": "read_bandwidth", "value": 180.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"local": "false", "relation": "cross-numa-same-socket",
                        "working_set_exceeds_llc": "true"}},
            {"group": "numa", "metric": "read_bandwidth", "value": 92.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"local": "false", "relation": "cross-socket",
                        "working_set_exceeds_llc": "true"}},
        ])

        estimates, _ = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}

        self.assertEqual(by_key["numa.same_socket_remote_latency"]["value"], 125.0)
        self.assertEqual(by_key["numa.cross_socket_latency"]["value"], 255.0)
        self.assertEqual(by_key["numa.same_socket_remote_payload_bandwidth"]["value"], 180.0)
        self.assertEqual(by_key["numa.cross_socket_payload_bandwidth"]["value"], 92.0)

    def test_numa_cache_polluted_and_impossible_bandwidth_points_are_excluded(self):
        self.system["memory_bandwidth_theoretical"] = {"upper_bound_gbps": 614.4}
        self.native["observations"].extend([
            {"group": "numa", "metric": "read_bandwidth", "value": 2081.0,
             "unit": "GB/s", "confidence": "low",
             "labels": {"local": "false", "relation": "cross-numa-same-socket",
                        "working_set_exceeds_llc": "false"}},
            {"group": "numa", "metric": "read_bandwidth", "value": 900.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"local": "false", "relation": "cross-numa-same-socket",
                        "working_set_exceeds_llc": "true"}},
            {"group": "numa", "metric": "read_bandwidth", "value": 180.0,
             "unit": "GB/s", "confidence": "high",
             "labels": {"local": "false", "relation": "cross-numa-same-socket",
                        "working_set_exceeds_llc": "true"}},
        ])

        estimates, diagnostics = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}

        self.assertEqual(by_key["numa.same_socket_remote_payload_bandwidth"]["value"], 180.0)
        self.assertEqual(by_key["numa.interconnect_payload_bandwidth"]["value"], 180.0)
        self.assertEqual(diagnostics["numa_bandwidth"]["rejected_points"], 2)

    def test_pipeline_unavailable_estimates_include_pmu_runtime_failure(self):
        self.native["metadata"] = {
            "pmu_core_available": False,
            "pmu_core_error": "time_running 为 0（事件无法调度）",
            "perf_event_paranoid": "2",
            "nmi_watchdog": "1",
        }

        estimates, _ = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}

        for key in (
            "core.max_observed_ipc",
            "core.frontend_width_lower_bound",
            "core.integer_add_lanes_lower_bound",
        ):
            self.assertIn("time_running 为 0", by_key[key]["basis"])
            self.assertIn("perf_event_paranoid=2", by_key[key]["caveat"])
            self.assertIn("nmi_watchdog=1", by_key[key]["caveat"])


if __name__ == "__main__":
    unittest.main()
