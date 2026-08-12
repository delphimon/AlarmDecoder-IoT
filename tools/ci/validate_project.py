#!/usr/bin/env python3
"""Fast, dependency-free checks for release-critical repository inputs."""

from __future__ import annotations

from html.parser import HTMLParser
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
VERSION_PATTERN = re.compile(r"AD2IOT-\d+")


class IdCollector(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: list[str] = []

    def handle_starttag(self, _tag: str, attrs: list[tuple[str, str | None]]) -> None:
        for name, value in attrs:
            if name == "id" and value:
                self.ids.append(value)


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)


def main() -> int:
    errors = 0
    version = (ROOT / "version.txt").read_text(encoding="utf-8").strip()
    if not VERSION_PATTERN.fullmatch(version):
        fail(f"version.txt must match AD2IOT-<number>; found {version!r}")
        errors += 1

    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    if version not in changelog:
        fail(f"CHANGELOG.md does not mention current version {version}")
        errors += 1

    web_root = ROOT / "contrib" / "webUI" / "flash-drive" / "www"
    required_web_files = ("app.html", "app.css", "app.js", "index.html", "alarmdecoder.yaml")
    for filename in required_web_files:
        if not (web_root / filename).is_file():
            fail(f"required web asset is missing: {filename}")
            errors += 1

    html_file = web_root / "app.html"
    collector = IdCollector()
    collector.feed(html_file.read_text(encoding="utf-8"))
    duplicates = sorted({element_id for element_id in collector.ids if collector.ids.count(element_id) > 1})
    if duplicates:
        fail(f"duplicate HTML ids in app.html: {', '.join(duplicates)}")
        errors += 1

    for xml_name in ("AlarmDecoder.xml", "device_description.xml"):
        try:
            ET.parse(web_root / xml_name)
        except ET.ParseError as exc:
            fail(f"invalid XML in {xml_name}: {exc}")
            errors += 1

    ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    if "platform = espressif32@=7.0.0" not in ini:
        fail("platformio.ini must pin Espressif32 7.0.0")
        errors += 1

    if errors:
        return 1
    print(f"Repository inputs validated for {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
