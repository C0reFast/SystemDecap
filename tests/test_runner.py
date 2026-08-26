import unittest

from system_decap.runner import _bandwidth_validation_warnings


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


if __name__ == "__main__":
    unittest.main()
