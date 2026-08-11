#!/usr/bin/env python3
"""Enforce conservative static-RAM and application-flash build budgets."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


RAM_PATTERN = re.compile(r"RAM:\s+\[[^]]*\]\s+[\d.]+%\s+\(used\s+(\d+)\s+bytes")
FLASH_PATTERN = re.compile(r"Flash:\s+\[[^]]*\]\s+[\d.]+%\s+\(used\s+(\d+)\s+bytes")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_log", help="PlatformIO build log path, or - for standard input")
    parser.add_argument("--max-ram", type=int, default=98_304)
    parser.add_argument("--max-flash", type=int, default=1_650_000)
    args = parser.parse_args()

    text = (
        sys.stdin.read()
        if args.build_log == "-"
        else Path(args.build_log).read_text(encoding="utf-8", errors="replace")
    )
    ram = RAM_PATTERN.search(text)
    flash = FLASH_PATTERN.search(text)
    if not ram or not flash:
        print("ERROR: PlatformIO RAM/Flash size summary was not found in the build log")
        return 2

    used_ram = int(ram.group(1))
    used_flash = int(flash.group(1))
    print(
        f"Firmware size: RAM {used_ram:,}/{args.max_ram:,} bytes; "
        f"flash {used_flash:,}/{args.max_flash:,} bytes"
    )
    if used_ram > args.max_ram:
        print(f"ERROR: static RAM exceeds the {args.max_ram:,}-byte CI budget")
        return 1
    if used_flash > args.max_flash:
        print(f"ERROR: application flash exceeds the {args.max_flash:,}-byte CI budget")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
