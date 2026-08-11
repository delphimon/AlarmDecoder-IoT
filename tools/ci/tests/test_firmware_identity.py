#!/usr/bin/env python3
"""Tests for embedded ESP firmware identity validation."""

from __future__ import annotations

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "tools" / "ci" / "verify_firmware_identity.py"


def test_image(version: str, magic: int = 0xABCD5432) -> bytes:
    image = bytearray(0xA0)
    struct.pack_into("<I", image, 0x20, magic)
    image[0x30 : 0x30 + len(version)] = version.encode("ascii")
    project = b"alarmdecoder_ad2iot_esp32"
    image[0x50 : 0x50 + len(project)] = project
    image[0x70 : 0x78] = b"13:18:32"
    image[0x80 : 0x8B] = b"Aug 11 2026"
    return bytes(image)


class FirmwareIdentityTests(unittest.TestCase):
    def run_checker(self, image: bytes) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            firmware = Path(temporary_directory) / "firmware.bin"
            firmware.write_bytes(image)
            return subprocess.run(
                [sys.executable, str(CHECKER), str(firmware)],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

    def test_accepts_matching_embedded_version(self) -> None:
        expected = (ROOT / "version.txt").read_text(encoding="utf-8").strip()
        result = self.run_checker(test_image(expected))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_stale_embedded_version(self) -> None:
        result = self.run_checker(test_image("AD2IOT-1112"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("version.txt", result.stdout)

    def test_rejects_invalid_application_descriptor(self) -> None:
        result = self.run_checker(test_image("AD2IOT-1113", magic=0))
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
