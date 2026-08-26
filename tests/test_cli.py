import unittest

from system_decap.cli import _parser


class CliTests(unittest.TestCase):
    def test_memory_topology_override_options(self):
        args = _parser().parse_args([
            "run", "--memory-channels", "12", "--memory-mtps", "6400",
        ])

        self.assertEqual(args.memory_channels, 12)
        self.assertEqual(args.memory_mtps, 6400)


if __name__ == "__main__":
    unittest.main()
