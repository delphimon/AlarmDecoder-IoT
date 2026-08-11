#!/usr/bin/env python3
"""Tests for firmware-size budget enforcement."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "tools" / "ci" / "check_firmware_size.py"


def build_summary(ram: int, flash: int) -> str:
    return (
        f"RAM:   [==        ]  19.1% (used {ram} bytes from 327680 bytes)\n"
        f"Flash: [========  ]  84.0% (used {flash} bytes from 1835008 bytes)\n"
    )


class FirmwareSizeTests(unittest.TestCase):
    def run_checker(self, contents: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            log = Path(temporary_directory) / "build.log"
            log.write_text(contents, encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(CHECKER), str(log), *arguments],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

    def test_accepts_firmware_within_budgets(self) -> None:
        result = self.run_checker(build_summary(62_732, 1_541_357))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_flash_growth_beyond_budget(self) -> None:
        result = self.run_checker(build_summary(62_732, 1_650_001))
        self.assertEqual(result.returncode, 1)
        self.assertIn("application flash exceeds", result.stdout)

    def test_rejects_missing_size_summary(self) -> None:
        result = self.run_checker("build stopped before size reporting\n")
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
