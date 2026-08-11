#!/usr/bin/env python3
"""Append the AlarmDecoder RSA/SHA-256 signature trailer to an ESP32 image."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

PADDING = b"\xff" * 6
RSA_SIGNATURE_SIZE = 256
TRAILER_SIZE = len(PADDING) * 2 + RSA_SIGNATURE_SIZE


def package_image(image: Path, private_key: Path, output: Path, openssl: str = "openssl") -> None:
    firmware = image.read_bytes()
    if not firmware:
        raise ValueError("firmware image is empty")
    with tempfile.TemporaryDirectory(prefix="ad2iot-ota-sign-") as directory:
        signature_path = Path(directory) / "signature.bin"
        # openssl hashes the firmware once and emits the RSA PKCS#1 v1.5
        # SHA-256 signature that mbedtls_pk_verify expects on the device.
        subprocess.run(
            [openssl, "dgst", "-sha256", "-sign", str(private_key), "-out", str(signature_path), str(image)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        signature = signature_path.read_bytes()
    if len(signature) != RSA_SIGNATURE_SIZE:
        raise ValueError("the signing key must be a 2048-bit RSA private key")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(firmware + PADDING + signature + PADDING)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--private-key", required=True, type=Path, help="external RSA-2048 PEM key")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args()
    package_image(args.image.resolve(), args.private_key.resolve(), args.output.resolve(), args.openssl)
    print(f"Created signed OTA image {args.output} ({args.output.stat().st_size} bytes).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
