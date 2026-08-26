import unittest

from system_decap.discover import (
    _memory_bandwidth_theoretical,
    _parse_smbios_memory_device,
    memory_bandwidth_override,
)


class DiscoverMemoryTests(unittest.TestCase):
    @staticmethod
    def _ddr5_device() -> bytes:
        raw = bytearray(0x5C)
        raw[0] = 17
        raw[1] = len(raw)
        raw[0x0A:0x0C] = (64).to_bytes(2, "little")
        raw[0x0C:0x0E] = (0x7FFF).to_bytes(2, "little")
        raw[0x1C:0x20] = (32768).to_bytes(4, "little")
        raw[0x10] = 1
        raw[0x11] = 2
        raw[0x12] = 0x22
        raw[0x15:0x17] = (6400).to_bytes(2, "little")
        raw[0x20:0x22] = (6400).to_bytes(2, "little")
        return bytes(raw) + b"DIMM_A1\0BANK_0\0\0"

    def test_parse_smbios_ddr5_device(self):
        device = _parse_smbios_memory_device(self._ddr5_device())

        self.assertEqual(device["locator"], "DIMM_A1")
        self.assertEqual(device["bank_locator"], "BANK_0")
        self.assertEqual(device["type"], "DDR5")
        self.assertEqual(device["size_bytes"], 32 * 1024**3)
        self.assertEqual(device["data_width_bits"], 64)
        self.assertEqual(device["configured_speed_mtps"], 6400)

    def test_twelve_ddr5_6400_dimms_produce_614_gbps_upper_bound(self):
        devices = [_parse_smbios_memory_device(self._ddr5_device()) for _ in range(12)]

        result = _memory_bandwidth_theoretical(devices)

        self.assertEqual(result["installed_devices"], 12)
        self.assertEqual(result["rated_devices"], 12)
        self.assertAlmostEqual(result["upper_bound_gbps"], 614.4)

    def test_manual_channel_and_rate_override(self):
        result = memory_bandwidth_override(12, 6400)

        self.assertAlmostEqual(result["upper_bound_gbps"], 614.4)
        self.assertEqual(result["channels"], 12)
        self.assertEqual(result["configured_speed_mtps"], 6400)
        self.assertIn("命令行", result["source"])


if __name__ == "__main__":
    unittest.main()
