#!/usr/bin/env python3
"""Smoke-test the release package layout and checksum manifest."""

from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
FIRMWARE_FILES = (
    "firmware.bin",
    "spiffs.bin",
    "bootloader.bin",
    "partitions.bin",
    "ota_data_initial.bin",
)


class ReleasePackagingTests(unittest.TestCase):
    def test_package_contains_required_files_with_valid_checksums(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            build_dir = temporary / "build"
            output_dir = temporary / "release"
            build_dir.mkdir()
            for index, filename in enumerate(FIRMWARE_FILES):
                (build_dir / filename).write_bytes(f"test-binary-{index}".encode("ascii"))

            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "ci" / "package_release.py"),
                    "--build-dir",
                    str(build_dir),
                    "--output-dir",
                    str(output_dir),
                ],
                cwd=ROOT,
                check=True,
            )

            firmware_dir = output_dir / "esp32" / "esp32-poe-iso-webui"
            for filename in FIRMWARE_FILES:
                self.assertTrue((firmware_dir / filename).is_file(), filename)
            self.assertTrue((firmware_dir / "sd-card" / "www" / "app.html").is_file())
            self.assertTrue((firmware_dir / "sd-card" / "www" / "activity.js").is_file())

            checksum_lines = (output_dir / "SHA256SUMS").read_text(encoding="utf-8").splitlines()
            self.assertGreater(len(checksum_lines), len(FIRMWARE_FILES))
            for line in checksum_lines:
                expected, relative_path = line.split("  ", 1)
                packaged_file = output_dir / Path(relative_path)
                actual = hashlib.sha256(packaged_file.read_bytes()).hexdigest()
                self.assertEqual(actual, expected, relative_path)


if __name__ == "__main__":
    unittest.main()
