#!/usr/bin/env python3
"""Verify the ESP application descriptor embedded in firmware.bin."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[2]
APP_DESCRIPTION_OFFSET = 0x20
APP_DESCRIPTION_MAGIC = 0xABCD5432


def descriptor_text(image: bytes, offset: int, length: int) -> str:
    value = image[offset : offset + length].split(b"\0", 1)[0]
    return value.decode("ascii", errors="strict")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", type=Path)
    args = parser.parse_args()

    image = args.firmware.read_bytes()
    if len(image) < APP_DESCRIPTION_OFFSET + 0x80:
        print("ERROR: firmware image is too short to contain an ESP application descriptor")
        return 2
    magic = struct.unpack_from("<I", image, APP_DESCRIPTION_OFFSET)[0]
    if magic != APP_DESCRIPTION_MAGIC:
        print(f"ERROR: invalid ESP application descriptor magic 0x{magic:08x}")
        return 2

    version = descriptor_text(image, APP_DESCRIPTION_OFFSET + 0x10, 32)
    project = descriptor_text(image, APP_DESCRIPTION_OFFSET + 0x30, 32)
    build_time = descriptor_text(image, APP_DESCRIPTION_OFFSET + 0x50, 16)
    build_date = descriptor_text(image, APP_DESCRIPTION_OFFSET + 0x60, 16)
    expected = (ROOT / "version.txt").read_text(encoding="utf-8").strip()
    if version != expected:
        print(f"ERROR: firmware embeds {version!r}, but version.txt contains {expected!r}")
        return 1
    if project != "alarmdecoder_ad2iot_esp32":
        print(f"ERROR: unexpected firmware project name {project!r}")
        return 1
    if not build_date or not build_time:
        print("ERROR: firmware build date/time is missing")
        return 1

    print(f"Firmware identity verified: {version} - {build_date} {build_time}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
