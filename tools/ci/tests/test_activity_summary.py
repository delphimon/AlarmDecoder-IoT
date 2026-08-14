"""Behavior checks for the summary-first Web UI activity feed."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[3]
ACTIVITY_JS = ROOT / "contrib" / "webUI" / "flash-drive" / "www" / "activity.js"


class ActivitySummaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.node = os.environ.get("NODE_EXE") or shutil.which("node")
        if not cls.node:
            raise unittest.SkipTest("Node.js is required for Web UI behavior tests")

    def run_activity(self, expression: str):
        script = (
            "const activity = require(" + json.dumps(str(ACTIVITY_JS)) + ");"
            "console.log(JSON.stringify(" + expression + "));"
        )
        result = subprocess.run(
            [self.node, "-e", script],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        return json.loads(result.stdout)

    def test_related_panel_burst_becomes_one_meaningful_event(self) -> None:
        history = [
            {"event": "BEEPS", "uptime_ms": 10000, "partition": 1, "alpha": "FAULT 04 MOTION DETECTOR"},
            {"event": "ZONE", "uptime_ms": 9000, "partition": 1, "zone": 4, "alpha": "FAULT 04 MOTION DETECTOR"},
            {"event": "READY", "uptime_ms": 8000, "partition": 1, "alpha": "FAULT 04 MOTION DETECTOR"},
            {"event": "ALPHA MSG.", "uptime_ms": 7000, "partition": 1, "alpha": "FAULT 04 MOTION DETECTOR"},
            {"event": "ZONE", "uptime_ms": 1000, "partition": 1, "alpha": "DISARMED BYPASS READY TO ARM"},
        ]
        summaries = self.run_activity(
            "activity.summarizeHistory(" + json.dumps(history) + ", false)"
        )

        self.assertEqual(len(summaries), 2)
        self.assertEqual(summaries[0]["event"], "ZONE")
        self.assertEqual(summaries[0]["update_count"], 4)
        self.assertEqual(summaries[0]["events"], ["BEEPS", "ZONE", "READY", "ALPHA MSG."])

    def test_descriptions_surface_zone_and_system_meaning(self) -> None:
        fault = self.run_activity(
            'activity.describe({event:"ZONE", alpha:"FAULT 04 MOTION DETECTOR"})'
        )
        ready = self.run_activity(
            'activity.describe({event:"READY", alpha:"DISARMED BYPASS READY TO ARM"})'
        )

        self.assertEqual(fault, {"title": "Zone 04 faulted", "detail": "Motion Detector"})
        self.assertEqual(ready, {"title": "System ready to arm", "detail": "Disarmed · Bypass active"})

    def test_technical_mode_preserves_every_raw_update(self) -> None:
        history = [
            {"event": "ZONE", "uptime_ms": 2000, "partition": 1, "alpha": "FAULT 01 FRONT DOOR"},
            {"event": "READY", "uptime_ms": 2000, "partition": 1, "alpha": "FAULT 01 FRONT DOOR"},
        ]
        technical = self.run_activity(
            "activity.summarizeHistory(" + json.dumps(history) + ", true)"
        )

        self.assertEqual(len(technical), 2)
        self.assertTrue(all(item["technical"] for item in technical))
        self.assertTrue(all(item["update_count"] == 1 for item in technical))

    def test_activity_asset_loads_before_application(self) -> None:
        html = (ACTIVITY_JS.parent / "app.html").read_text(encoding="utf-8")
        self.assertLess(html.index('src="activity.js"'), html.index('src="app.js"'))
        self.assertIn('id="activityMode"', html)


if __name__ == "__main__":
    unittest.main()
