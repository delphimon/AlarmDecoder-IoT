from __future__ import annotations

import base64
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


provision = load_tool("provision_stnv")
signer = load_tool("package_signed_ota")


class PanelCommandDouble:
    """Records commands without ever touching an AlarmDecoder panel."""

    def __init__(self):
        self.commands = []

    def send(self, command):
        self.commands.append(command)


class ThreePressCommandDouble:
    def __init__(self, panel, command):
        self.panel = panel
        self.command = command
        self.count = 0

    def press(self):
        self.count += 1
        if self.count >= 3:
            self.panel.send(self.command)
            self.count = 0


class ProvisioningTests(unittest.TestCase):
    def valid_identity(self):
        encoded = base64.b64encode(bytes(range(32))).decode()
        return {"serialNumber": "AD2IOTV10-0001", "publicKey": encoded, "privateKey": encoded}

    def test_stnv_csv_contains_expected_namespace_and_fields(self):
        text = provision.stnv_csv(self.valid_identity())
        self.assertIn("stdk,namespace", text)
        self.assertIn("PKType,data,string,ED25519", text)
        self.assertIn("PublicKey,data,string", text)
        self.assertIn("PrivateKey,data,string", text)
        self.assertIn("SerialNum,data,string,AD2IOTV10-0001", text)

    def test_identity_validation_rejects_wrong_key_length(self):
        identity = self.valid_identity()
        identity["privateKey"] = base64.b64encode(b"short").decode()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "identity.json"
            import json
            path.write_text(json.dumps(identity), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "exactly 32 bytes"):
                provision.load_identity(path)


class SigningTests(unittest.TestCase):
    def test_package_uses_expected_268_byte_trailer(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            image = directory / "app.bin"
            key = directory / "external.pem"
            output = directory / "signed.bin"
            image.write_bytes(b"firmware")
            key.write_text("external", encoding="ascii")

            def fake_run(command, **kwargs):
                self.assertEqual(Path(command[-1]), image)
                signature_path = Path(command[command.index("-out") + 1])
                signature_path.write_bytes(b"S" * signer.RSA_SIGNATURE_SIZE)
                return subprocess.CompletedProcess(command, 0)

            with mock.patch.object(signer.subprocess, "run", side_effect=fake_run):
                signer.package_image(image, key, output)
            packaged = output.read_bytes()
            self.assertEqual(packaged[:8], b"firmware")
            self.assertEqual(len(packaged), 8 + signer.TRAILER_SIZE)
            self.assertEqual(packaged[8:14], b"\xff" * 6)
            self.assertEqual(packaged[-6:], b"\xff" * 6)


class FirmwareContractTests(unittest.TestCase):
    def test_partition_layout_and_slot_size(self):
        text = (ROOT / "partitions.smartthings.4MB.csv").read_text(encoding="utf-8").replace(" ", "")
        expected = (
            ("stnv", "0x009000", "0x004000"), ("nvs", "0x00d000", "0x004000"),
            ("otadata", "0x011000", "0x002000"), ("phy_init", "0x013000", "0x001000"),
            ("nvs_key", "0x014000", "0x001000"), ("spiffs", "0x015000", "0x00b000"),
            ("coredump", "0x020000", "0x010000"), ("ota_0", "0x030000", "0x1e0000"),
            ("ota_1", "0x210000", "0x1e0000"),
        )
        for name, offset, size in expected:
            self.assertIn(f"{name},", text)
            self.assertIn(offset, text)
            self.assertIn(size, text)

    def test_identity_is_not_writable_or_logged_by_smartthings_cli(self):
        source = (ROOT / "components" / "stsdk" / "stsdk_main.cpp").read_text(encoding="utf-8")
        self.assertNotIn("STSDK_SUBCMD_SERIAL", source)
        self.assertNotIn("STSDK_SUBCMD_PUBLIC_KEY", source)
        self.assertNotIn("STSDK_SUBCMD_PRIVATE_KEY", source)
        self.assertIn("identity=STNV", source)

    def test_cloud_connected_is_the_only_ready_lifecycle(self):
        source = (ROOT / "components" / "stsdk" / "stsdk_main.cpp").read_text(encoding="utf-8")
        ready_block = "if (status == ST_DEVICE_STATUS_CLOUD_CONNECTED) {\n        hal_set_network_connected(true);"
        self.assertIn(ready_block, source)
        self.assertIn("else {\n        hal_set_network_connected(false);", source)

    def test_dangerous_commands_retain_three_press_guards(self):
        source = (ROOT / "components" / "stsdk" / "stsdk_main.cpp").read_text(encoding="utf-8")
        for counter in ("fire_trigger_count", "panic_trigger_count", "aux_trigger_count"):
            self.assertIn(f"if ({counter} >= 3)", source)
        for reset in ("fire_trigger_clear(nullptr)", "panic_trigger_clear(nullptr)",
                      "aux_trigger_clear(nullptr)"):
            self.assertIn(reset, source)

    def test_dangerous_commands_use_isolated_three_press_doubles(self):
        for command in ("fire", "panic", "auxiliary"):
            panel = PanelCommandDouble()
            guard = ThreePressCommandDouble(panel, command)
            guard.press()
            guard.press()
            self.assertEqual(panel.commands, [])
            guard.press()
            self.assertEqual(panel.commands, [command])
            guard.press()
            self.assertEqual(panel.commands, [command])

    def test_safe_commands_route_to_real_alarmdecoder_helpers(self):
        source = (ROOT / "components" / "stsdk" / "stsdk_main.cpp").read_text(encoding="utf-8")
        for helper in ("ad2_chime_toggle(", "ad2_arm_stay(", "ad2_arm_away(",
                       "ad2_disarm(", "ad2_exit_now("):
            self.assertIn(helper, source)

    def test_refresh_resyncs_all_panel_state_without_ready_recursion(self):
        source = (ROOT / "components" / "stsdk" / "stsdk_main.cpp").read_text(encoding="utf-8")
        for callback in ("on_arm_cb", "on_disarm_cb", "on_chime_change_cb",
                         "on_ready_to_arm_change_cb", "on_fire_change_cb", "on_power_cb",
                         "on_low_battery_cb", "on_alarm_change_cb",
                         "on_zone_bypassed_change_cb", "on_exit_now_change_cb"):
            self.assertIn(f"{callback}(&statestr, s, nullptr)", source)
        self.assertIn('const bool is_refresh = msg && (*msg == "REFRESH")', source)
        self.assertIn("refresh_cmd_cb(nullptr, nullptr, nullptr);\n            return;", source)

    def test_change_events_drive_normal_capability_publication(self):
        source = (ROOT / "components" / "stsdk" / "stsdk_main.cpp").read_text(encoding="utf-8")
        subscriptions = {
            "ON_CHIME_CHANGE": "on_chime_change_cb",
            "ON_FIRE_CHANGE": "on_fire_change_cb",
            "ON_POWER_CHANGE": "on_power_cb",
            "ON_LOW_BATTERY": "on_low_battery_cb",
            "ON_ALARM_CHANGE": "on_alarm_change_cb",
            "ON_ZONE_BYPASSED_CHANGE": "on_zone_bypassed_change_cb",
            "ON_EXIT_CHANGE": "on_exit_now_change_cb",
            "ON_READY_CHANGE": "on_ready_to_arm_change_cb",
        }
        for event, callback in subscriptions.items():
            self.assertIn(f"AD2Parse.subscribeTo({event}, {callback}", source)

    def test_ota_rejects_malformed_unsafe_and_concurrent_updates(self):
        source = (ROOT / "components" / "otaupdate" / "ota_util.cpp").read_text(encoding="utf-8")
        for guard in ("!cJSON_IsObject(root)", "!cJSON_IsString(item)",
                      "firmware_len > update_partition->size",
                      "total_read_len != firmware_len",
                      "sig_len != OTA_DEFAULT_SIGNATURE_BUF_SIZE",
                      "_check_firmware_validation", "esp_ota_abort(update_handle)",
                      "if (ota_check_task_handle || ota_task_handle)"):
            self.assertIn(guard, source)
        failure_cleanup = source[source.index("clean_up:"):source.index("static void ota_task_func")]
        self.assertNotIn("esp_restart", failure_cleanup)

    def test_ser2sock_parser_accepts_hostname_ipv4_and_bracketed_ipv6(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("no host C++ compiler")
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            harness = directory / "endpoint_test.cpp"
            executable = directory / "endpoint_test"
            harness.write_text(
                '#include "ser2sock_endpoint.h"\n'
                'int main(){ Ser2sockEndpoint e; '
                'if(!ad2_parse_ser2sock_endpoint("ad2iot.lan:10000",e)||e.host!="ad2iot.lan") return 1; '
                'if(!ad2_parse_ser2sock_endpoint("192.0.2.4:10000",e)) return 2; '
                'if(!ad2_parse_ser2sock_endpoint("[2001:db8::1]:10000",e)||e.host!="2001:db8::1") return 3; '
                'if(ad2_parse_ser2sock_endpoint("2001:db8::1:10000",e)) return 4; '
                'if(ad2_parse_ser2sock_endpoint("host:70000",e)) return 5; return 0; }',
                encoding="utf-8",
            )
            subprocess.run(
                [compiler, "-std=c++11", "-I", str(ROOT / "main"), str(harness),
                 str(ROOT / "main" / "ser2sock_endpoint.cpp"), "-o", str(executable)], check=True
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
