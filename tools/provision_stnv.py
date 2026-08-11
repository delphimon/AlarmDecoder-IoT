#!/usr/bin/env python3
"""Generate and flash a SmartThings STNV identity without persisting secrets."""

from __future__ import annotations

import argparse
import base64
import csv
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

STNV_OFFSET = "0x9000"
STNV_SIZE = "0x4000"


def load_identity(path: Path) -> dict[str, str]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    identity = raw.get("deviceInfo", raw)
    required = ("serialNumber", "publicKey", "privateKey")
    if not isinstance(identity, dict) or any(not isinstance(identity.get(k), str) for k in required):
        raise ValueError("identity JSON must contain serialNumber, publicKey, and privateKey strings")
    serial = identity["serialNumber"]
    if not 8 <= len(serial) <= 30 or any(ch in serial for ch in "\r\n,"):
        raise ValueError("serialNumber must be 8-30 characters and contain no comma or newline")
    for key_name in ("publicKey", "privateKey"):
        try:
            decoded = base64.b64decode(identity[key_name], validate=True)
        except (ValueError, base64.binascii.Error) as exc:
            raise ValueError(f"{key_name} must be valid base64") from exc
        if len(decoded) != 32:
            raise ValueError(f"{key_name} must decode to exactly 32 bytes")
    return {key: identity[key] for key in required}


def stnv_csv(identity: dict[str, str]) -> str:
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(("key", "type", "encoding", "value"))
    writer.writerow(("stdk", "namespace", "", ""))
    writer.writerow(("PKType", "data", "string", "ED25519"))
    # SDK v2.3.2 reads these base64-encoded values with nvs_get_str().
    writer.writerow(("PublicKey", "data", "string", identity["publicKey"]))
    writer.writerow(("PrivateKey", "data", "string", identity["privateKey"]))
    writer.writerow(("SerialNum", "data", "string", identity["serialNumber"]))
    return output.getvalue()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("identity", type=Path, help="external, untracked SmartThings identity JSON")
    parser.add_argument("--port", required=True, help="serial port, for example COM7")
    parser.add_argument("--baud", default="460800")
    parser.add_argument("--idf-path", type=Path, default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()

    if not args.idf_path:
        parser.error("--idf-path or IDF_PATH is required")
    idf_path = Path(args.idf_path).resolve()
    generator = idf_path / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
    esptool = idf_path / "components" / "esptool_py" / "esptool" / "esptool.py"
    if not generator.is_file() or not esptool.is_file():
        parser.error("the supplied ESP-IDF path does not contain the NVS generator and esptool")

    identity = load_identity(args.identity.resolve())
    masked_serial = "*" * max(0, len(identity["serialNumber"]) - 4) + identity["serialNumber"][-4:]
    with tempfile.TemporaryDirectory(prefix="ad2iot-stnv-") as directory:
        work = Path(directory)
        csv_path = work / "stnv.csv"
        image_path = work / "stnv.bin"
        csv_path.write_text(stnv_csv(identity), encoding="utf-8", newline="")
        subprocess.run(
            [sys.executable, str(generator), "generate", str(csv_path), str(image_path), STNV_SIZE, "--version", "1"],
            check=True,
        )
        subprocess.run(
            [sys.executable, str(esptool), "--chip", "esp32", "--port", args.port, "--baud", args.baud,
             "write_flash", STNV_OFFSET, str(image_path)],
            check=True,
        )
    print(f"Provisioned SmartThings STNV identity {masked_serial} on {args.port}; temporary key material removed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
