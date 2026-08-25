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
             "unit": "GB/s", "labels": {"operation": "read", "threads": "1"}},
            {"group": "memory_bandwidth", "metric": "stream_bandwidth", "value": 41.0,
             "unit": "GB/s", "labels": {"operation": "read", "threads": "2"}},
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

    def test_cache_inventory_becomes_estimate(self):
        estimates, diagnostics = infer(self.system, self.native)
        by_key = {item["key"]: item for item in estimates}
        self.assertEqual(by_key["cache.l1.capacity"]["value"], 32768)
        self.assertTrue(diagnostics["cache_knees"])


if __name__ == "__main__":
    unittest.main()
