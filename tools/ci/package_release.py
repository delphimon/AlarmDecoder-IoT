#!/usr/bin/env python3
"""Create the distributable ESP32 package from a completed PlatformIO build."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil


ROOT = Path(__file__).resolve().parents[2]
VERSION_PATTERN = re.compile(r"AD2IOT-\d+")
FIRMWARE_FILES = (
    "firmware.bin",
    "spiffs.bin",
    "bootloader.bin",
    "partitions.bin",
    "ota_data_initial.bin",
)


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise FileNotFoundError(f"Required release file is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    output_dir = args.output_dir.resolve()
    version = (ROOT / "version.txt").read_text(encoding="utf-8").strip()
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError(f"version.txt must match AD2IOT-<number>; found {version!r}")

    if output_dir.exists():
        shutil.rmtree(output_dir)

    firmware_dir = output_dir / "esp32" / "esp32-poe-iso-webui"
    for filename in FIRMWARE_FILES:
        copy_file(build_dir / filename, firmware_dir / filename)

    for filename in ("README.md", "CHANGELOG.md", "LICENSE", "version.txt"):
        copy_file(ROOT / filename, output_dir / filename)
    copy_file(ROOT / "contrib" / "README-FLASH-ESP32.md", output_dir / "esp32" / "README-FLASH-ESP32.md")
    copy_file(
        ROOT / "contrib" / "ESP32-DOWNLOAD-TOOL-UPLOADING-FIRMWARE.png",
        output_dir / "esp32" / "ESP32-DOWNLOAD-TOOL-UPLOADING-FIRMWARE.png",
    )

    sd_source = ROOT / "contrib" / "webUI" / "flash-drive"
    sd_destination = firmware_dir / "sd-card"
    if not sd_source.is_dir():
        raise FileNotFoundError(f"SD-card web bundle is missing: {sd_source}")
    shutil.copytree(sd_source, sd_destination)

    packaged_files = sorted(path for path in output_dir.rglob("*") if path.is_file())
    checksum_file = output_dir / "SHA256SUMS"
    checksum_file.write_text(
        "".join(f"{sha256(path)}  {path.relative_to(output_dir).as_posix()}\n" for path in packaged_files),
        encoding="utf-8",
        newline="\n",
    )

    artifact_name = f"{version}-Release-Package"
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with Path(github_output).open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(f"artifact_name={artifact_name}\n")
            stream.write(f"firmware_version={version}\n")

    print(f"Packaged {len(packaged_files)} files for {version} in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

