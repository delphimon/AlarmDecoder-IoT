"""Regression checks for authenticated outbound TLS and trusted boot time."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


class TlsPolicyTests(unittest.TestCase):
    def test_insecure_global_tls_fallback_is_disabled(self) -> None:
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        board = (ROOT / "sdkconfig.esp32-poe-iso").read_text(encoding="utf-8")

        for config in (defaults, board):
            self.assertNotIn("CONFIG_ESP_TLS_INSECURE=y", config)
            self.assertNotIn("CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y", config)
            self.assertIn("# CONFIG_ESP_TLS_INSECURE is not set", config)

    def test_legacy_unconstrained_custom_ca_is_not_shipped(self) -> None:
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        board = (ROOT / "sdkconfig.esp32-poe-iso").read_text(encoding="utf-8")

        for config in (defaults, board):
            self.assertNotIn("CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE=y", config)
            self.assertIn("# CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE is not set", config)
        self.assertFalse((ROOT / "certs" / "dummy_server_root.pem").exists())

    def test_http_clients_use_the_certificate_bundle(self) -> None:
        utilities = (ROOT / "main" / "ad2_utils.cpp").read_text(encoding="utf-8")
        ota = (ROOT / "components" / "otaupdate" / "ota_util.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("client_config->crt_bundle_attach = esp_crt_bundle_attach", utilities)
        self.assertIn("client_config->skip_cert_common_name_check = false", utilities)
        self.assertIn("hal_wait_for_time_sync(30000)", utilities)
        self.assertEqual(ota.count("ad2_configure_http_client_tls(config);"), 2)

    def test_secure_mqtt_uses_bundle_and_waits_for_time(self) -> None:
        mqtt = (ROOT / "components" / "ad2mqtt" / "ad2mqtt.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn('rfind("mqtts://", 0)', mqtt)
        self.assertIn('rfind("wss://", 0)', mqtt)
        self.assertIn("mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach", mqtt)
        self.assertIn("hal_wait_for_time_sync(30000)", mqtt)

    def test_managed_sntp_is_started_and_visible(self) -> None:
        control = (ROOT / "main" / "device_control.cpp").read_text(encoding="utf-8")
        main = (ROOT / "main" / "alarmdecoder_main.cpp").read_text(encoding="utf-8")
        webui = (ROOT / "components" / "webUI" / "webUI.cpp").read_text(encoding="utf-8")
        app = (ROOT / "contrib" / "webUI" / "flash-drive" / "www" / "app.js").read_text(
            encoding="utf-8"
        )

        self.assertIn("esp_netif_sntp_init(&config)", control)
        self.assertIn("hal_init_time_sync();", main)
        self.assertNotIn("esp_tls_init_global_ca_store", main)
        self.assertIn('"time_synchronized"', webui)
        self.assertIn('"unix_time"', webui)
        self.assertIn('byId("diagTrustedClock")', app)

    def test_shipped_and_factory_configs_have_time_server(self) -> None:
        settings = (ROOT / "main" / "ad2_settings.h").read_text(encoding="utf-8")
        control = (ROOT / "main" / "device_control.cpp").read_text(encoding="utf-8")
        shipped = (ROOT / "data" / "ad2iot.ini").read_text(encoding="utf-8")

        self.assertIn('#define AD2_DEFAULT_TIME_SERVER "pool.ntp.org"', settings)
        self.assertIn('"timeserver = " AD2_DEFAULT_TIME_SERVER', control)
        self.assertIn("timeserver = pool.ntp.org", shipped)


if __name__ == "__main__":
    unittest.main()
