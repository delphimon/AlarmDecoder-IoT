#!/usr/bin/env python3
"""Regression checks for externally reachable service defaults."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


def read_section(name: str) -> dict[str, str]:
    """Read simple key/value settings from one section of the device INI."""
    settings: dict[str, str] = {}
    active = False
    for raw_line in (ROOT / "data" / "ad2iot.ini").read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line.startswith("[") and line.endswith("]"):
            active = line[1:-1].strip().casefold() == name.casefold()
            continue
        if active and line and not line.startswith(("#", ";")) and "=" in line:
            key, value = line.split("=", 1)
            settings[key.strip().casefold()] = value.strip()
    return settings


class SecurityDefaultsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ftpd = read_section("ftpd")
        cls.netcli = read_section("netcli")

    def test_ftp_is_disabled_and_has_no_shipped_credentials(self) -> None:
        self.assertEqual(self.ftpd.get("enable", "").casefold(), "false")
        self.assertEqual(self.ftpd.get("user", ""), "")
        self.assertEqual(self.ftpd.get("password", ""), "")

    def test_ftp_acl_is_not_allow_all(self) -> None:
        acl = self.ftpd.get("acl", "").replace(" ", "")
        self.assertNotEqual(acl, "")
        self.assertNotIn("0.0.0.0/0", acl.split(","))

    def test_network_cli_is_disabled_and_has_no_shipped_password(self) -> None:
        self.assertEqual(self.netcli.get("enable", "").casefold(), "false")
        self.assertEqual(self.netcli.get("password", ""), "")

    def test_factory_reset_keeps_ftp_disabled_and_loopback_only(self) -> None:
        source = (ROOT / "main" / "device_control.cpp").read_text(encoding="utf-8")
        reset_function = source[source.index("bool hal_factory_reset"):source.index("void hal_init_wifi")]
        self.assertIn('fprintf(f, "enable = false\\r\\n")', reset_function)
        self.assertIn('fprintf(f, "acl = 127.0.0.1\\r\\n")', reset_function)

    def test_wifi_credentials_do_not_use_unbounded_strcpy(self) -> None:
        source = (ROOT / "main" / "device_control.cpp").read_text(encoding="utf-8")
        wifi_function = source[source.index("void hal_init_wifi"):source.index("void hal_init_eth")]
        self.assertNotIn("strcpy", wifi_function)
        self.assertIn("sizeof(sta_config.sta.ssid)", wifi_function)
        self.assertIn("sizeof(sta_config.sta.password)", wifi_function)


if __name__ == "__main__":
    unittest.main()
