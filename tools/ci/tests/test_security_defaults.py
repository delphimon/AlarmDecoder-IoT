#!/usr/bin/env python3
"""Regression checks for externally reachable service defaults."""

from __future__ import annotations

from pathlib import Path
import re
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
        cls.webui = read_section("webui")
        cls.webui_source = (ROOT / "components" / "webUI" / "webUI.cpp").read_text(
            encoding="utf-8"
        )

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

    def test_webui_is_disabled_and_has_no_shipped_credentials(self) -> None:
        self.assertEqual(self.webui.get("enable", "").casefold(), "false")
        self.assertEqual(self.webui.get("user", ""), "")
        self.assertEqual(self.webui.get("password", ""), "")

    def test_webui_component_default_acl_is_loopback_only(self) -> None:
        match = re.search(
            r'#define\s+WEBUI_DEFAULT_ACL\s+"([^"]+)"', self.webui_source
        )
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), "127.0.0.1")

    def test_webui_requires_valid_credentials_before_starting(self) -> None:
        init = self.webui_source[self.webui_source.index("void webui_init(void)") :]
        self.assertIn("webui_load_credentials()", init)
        self.assertIn("acl.empty()", init)
        self.assertIn("refusing to start", init)

    def test_webui_http_routes_share_the_authentication_guard(self) -> None:
        handlers = (
            "webui_state_handler",
            "webui_history_handler",
            "webui_system_handler",
            "webui_firmware_handler",
            "webui_action_handler",
            "webui_config_handler",
            "webui_logs_handler",
            "file_get_handler",
        )
        for handler in handlers:
            start = self.webui_source.index(f"{handler}(httpd_req_t *req)")
            prologue = self.webui_source[start : start + 300]
            self.assertIn("webui_authorize_request(req)", prologue, handler)

    def test_webui_http_rejections_flush_the_error_response(self) -> None:
        handlers = (
            "webui_state_handler",
            "webui_history_handler",
            "webui_system_handler",
            "webui_firmware_handler",
            "webui_action_handler",
            "webui_config_handler",
            "webui_logs_handler",
            "file_get_handler",
        )
        for handler in handlers:
            start = self.webui_source.index(f"{handler}(httpd_req_t *req)")
            prologue = self.webui_source[start : start + 300]
            auth_branch = prologue[prologue.index("webui_authorize_request(req)") :]
            self.assertIn("return ESP_OK;", auth_branch, handler)

    def test_websocket_commands_require_an_authenticated_session(self) -> None:
        handler = self.webui_source[
            self.webui_source.index("esp_err_t ad2ws_handler") :
            self.webui_source.index("static int webui_query_int")
        ]
        self.assertIn("webui_authorize_request(req)", handler)
        self.assertIn("authenticated", handler)
        self.assertIn("synced", handler)
        self.assertIn("webui_origin_allowed(req)", handler)

    def test_websocket_history_does_not_duplicate_the_printed_json(self) -> None:
        start = self.webui_source.index('std::string key_history = "!HISTORY:"')
        history = self.webui_source[start : start + 1200]
        self.assertIn("webui_ws_send_text(req, history, strlen(history))", history)
        self.assertNotIn("std::string response = history", history)

    def test_maintenance_actions_require_origin_and_custom_header(self) -> None:
        start = self.webui_source.index("webui_action_handler(httpd_req_t *req)")
        handler = self.webui_source[start : start + 2600]
        self.assertIn("webui_authorize_request(req)", handler)
        self.assertIn("webui_origin_allowed(req)", handler)
        self.assertIn('"X-AD2IoT-Action"', handler)
        self.assertIn("strcmp(guard, action_item->valuestring) != 0", handler)

    def test_session_cookie_uses_persistent_secure_storage(self) -> None:
        self.assertIn("webui_session_cookie_https", self.webui_source)
        self.assertIn("HttpOnly; SameSite=Strict", self.webui_source)
        self.assertIn('webui_session_cookie_http + "; Secure"', self.webui_source)
        setter_start = self.webui_source.index("static void webui_set_session_cookie")
        setter = self.webui_source[setter_start : setter_start + 700]
        self.assertIn("webui_session_cookie_https", setter)
        self.assertNotIn("std::string cookie =", setter)

    def test_config_redaction_is_in_place_and_scrubs_reused_secrets(self) -> None:
        start = self.webui_source.index("static void webui_redact_config")
        redactor = self.webui_source[
            start : self.webui_source.index("static bool webui_read_file", start)
        ]
        self.assertIn("contents.replace", redactor)
        self.assertIn("sensitive_values.push_back(value)", redactor)
        self.assertIn("contents.find(value", redactor)
        self.assertNotIn("std::string output", redactor)

    def test_active_config_uses_the_bounded_boot_source_file(self) -> None:
        start = self.webui_source.index("static esp_err_t webui_config_handler")
        handler = self.webui_source[start : start + 2200]
        self.assertIn('source == "active"', handler)
        self.assertIn("ad2_config_uses_sd()", handler)
        self.assertIn("webui_send_redacted_config_file", handler)
        self.assertNotIn("ad2_get_config_snapshot", handler)

    def test_config_files_are_redacted_and_sent_in_bounded_chunks(self) -> None:
        start = self.webui_source.index("static esp_err_t webui_send_redacted_config_file")
        streamer = self.webui_source[
            start : self.webui_source.index("static bool webui_resolve_sd_path", start)
        ]
        self.assertIn("webui_read_config_line", streamer)
        self.assertIn("webui_collect_sensitive_config_value", streamer)
        self.assertIn("webui_scrub_sensitive_values", streamer)
        self.assertIn("httpd_resp_send_chunk", streamer)
        self.assertNotIn("std::string config", streamer)

    def test_browser_rest_requests_send_same_origin_credentials(self) -> None:
        app = (ROOT / "contrib" / "webUI" / "flash-drive" / "www" / "app.js").read_text(
            encoding="utf-8"
        )
        self.assertGreaterEqual(app.count('credentials: "same-origin"'), 5)

    def test_webui_static_path_rejects_traversal(self) -> None:
        start = self.webui_source.index("esp_err_t file_get_handler")
        handler = self.webui_source[start : start + 7000]
        self.assertIn('strstr(filename, "..")', handler)
        self.assertIn("strchr(filename, '\\\\')", handler)
        self.assertIn('"Content-Security-Policy"', handler)
        self.assertIn('"X-Frame-Options", "DENY"', handler)

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
