import unittest
from pathlib import Path
from unittest.mock import patch

from system_decap.cli import _parser, main


class CliTests(unittest.TestCase):
    def test_memory_topology_override_options(self):
        args = _parser().parse_args([
            "run", "--memory-channels", "12", "--memory-mtps", "6400",
        ])

        self.assertEqual(args.memory_channels, 12)
        self.assertEqual(args.memory_mtps, 6400)

    def test_standard_profile_defaults_to_eight_gibibytes_per_array(self):
        with patch("system_decap.cli.execute") as execute:
            execute.return_value = (Path("/tmp/report"), {"observations": [], "estimates": []})

            status = main(["run", "--profile", "standard", "--skip-build"])

        self.assertEqual(status, 0)
        self.assertEqual(execute.call_args.kwargs["memory_mib"], 8192)

    def test_explicit_memory_size_overrides_standard_default(self):
        with patch("system_decap.cli.execute") as execute:
            execute.return_value = (Path("/tmp/report"), {"observations": [], "estimates": []})

            status = main([
                "run", "--profile", "standard", "--memory-mib", "4096", "--skip-build",
            ])

        self.assertEqual(status, 0)
        self.assertEqual(execute.call_args.kwargs["memory_mib"], 4096)

    def test_only_probes_are_forwarded_to_runner(self):
        with patch("system_decap.cli.execute") as execute:
            execute.return_value = (Path("/tmp/report"), {"observations": [], "estimates": []})

            status = main([
                "run", "--profile", "smoke", "--only", "timer,numa",
                "--only", "core-latency", "--skip-build",
            ])

        self.assertEqual(status, 0)
        self.assertEqual(
            execute.call_args.kwargs["only_probes"],
            ["timer,numa", "core-latency"],
        )


if __name__ == "__main__":
    unittest.main()
