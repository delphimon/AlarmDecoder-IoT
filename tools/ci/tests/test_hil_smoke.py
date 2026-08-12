import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = ROOT / "tools" / "hil" / "smoke_webui.py"
SPEC = importlib.util.spec_from_file_location("smoke_webui", MODULE_PATH)
assert SPEC and SPEC.loader
smoke_webui = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = smoke_webui
SPEC.loader.exec_module(smoke_webui)


class HilSmokeTests(unittest.TestCase):
    def test_assignment_leaks_ignores_comments_and_accepts_redaction(self) -> None:
        secrets = {"webui.password": "unique-secret"}
        body = """# example = unique-secret
[webui]
password = [redacted]
[generic]
value = harmless
"""
        self.assertEqual([], smoke_webui.assignment_leaks(body, secrets))

    def test_assignment_leaks_reports_reused_secret_by_label(self) -> None:
        secrets = {"webui.password": "unique-secret"}
        body = """[webui]
password = [redacted]
[generic]
value = unique-secret
"""
        self.assertEqual(
            ["webui.password"], smoke_webui.assignment_leaks(body, secrets)
        )

    def test_sensitive_assignments_must_be_redacted(self) -> None:
        body = """[netcli]
password = exposed
"""
        self.assertEqual(
            ["netcli.password"], smoke_webui.assignment_leaks(body, {})
        )

    def test_websocket_client_frames_are_masked(self) -> None:
        frame = smoke_webui.WebSocketProbe._frame(b"!PING:00000000")
        self.assertEqual(0x81, frame[0])
        self.assertTrue(frame[1] & 0x80)

    def test_smoke_requires_device_reported_trusted_clock(self) -> None:
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertIn('network.get("time_synchronized") is not True', source)
        self.assertIn("clock_skew > 120", source)


if __name__ == "__main__":
    unittest.main()
