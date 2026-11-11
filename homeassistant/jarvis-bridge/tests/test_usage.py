# SPDX-License-Identifier: GPL-3.0-or-later
import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import usage


SAMPLE = {
    "type": "usage_snapshot",
    "schema": "stackchan.usage.v1",
    "fetched_at": "2026-11-10T18:00:00-05:00",
    "period": {
        "used_pct": 62,
        "resets_at": "2026-11-16T00:00:00-05:00",
        "resets_label": "Resets Mon 12:00 AM ET",
        "on_demand": "off",
    },
    "days": [{"d": "Mon", "used_pct": 7}, {"d": "Tue", "used_pct": 11}],
    "bots": [{
        "id": "hulk",
        "name": "Hulk",
        "avatar_shape": "rounded_hexagon",
        "avatar_color": "#1fcb72",
        "share_pct": 38,
        "activity": {"chats_pct": 52, "routines_pct": 28, "background_pct": 14, "browser_pct": 6},
        "routines": [{"name": "Voice path", "share_pct": 22}],
        "days": [{"d": "Mon", "used_pct": 3}],
    }],
}


class UsageUnitTests(unittest.TestCase):
    def test_sanitize_normalizes_and_rejects(self):
        out = usage.sanitize(SAMPLE)
        self.assertEqual(out["bots"][0]["avatar_color"], "#1FCB72")
        self.assertEqual(out["period"]["used_pct"], 62)
        self.assertIsNone(usage.sanitize({**SAMPLE, "bots": [{**SAMPLE["bots"][0], "avatar_shape": "circle"}]}))
        self.assertIsNone(usage.sanitize({**SAMPLE, "period": {**SAMPLE["period"], "used_pct": -1}}))

    def test_example_payloads_are_not_cached_for_the_robot(self):
        with tempfile.TemporaryDirectory() as directory:
            store = usage.UsageStore(directory)
            self.assertIsNone(store.accept({**SAMPLE, "_example": True}))
            self.assertIsNone(store.message())
            self.assertFalse(store.path.exists())

    def test_append_dedupes_same_fetched_at_and_prunes_old_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            store = usage.UsageStore(directory)
            self.assertIsNotNone(store.accept(SAMPLE))
            self.assertIsNotNone(store.accept(SAMPLE))
            lines = store.path.read_text().splitlines()
            self.assertEqual(len(lines), 1)
            old = {**SAMPLE, "fetched_at": "2020-01-01T00:00:00+00:00", "period": {**SAMPLE["period"], "used_pct": 1}}
            store.path.write_text(json.dumps(old) + "\n" + lines[0] + "\n")
            later = {**SAMPLE, "fetched_at": "2026-11-10T19:00:00-05:00"}
            self.assertIsNotNone(store.accept(later))
            kept = [json.loads(line) for line in store.path.read_text().splitlines()]
            self.assertEqual([row["fetched_at"] for row in kept], [SAMPLE["fetched_at"], later["fetched_at"]])

    def test_sensor_payloads_include_color_and_shape(self):
        snapshot = usage.sanitize(SAMPLE)
        entities = dict(usage.sensor_payloads(snapshot))
        self.assertEqual(entities["sensor.grokbot_usage_used_pct"]["state"], 62)
        bot = entities["sensor.grokbot_usage_bot_hulk"]
        self.assertEqual(bot["state"], 38)
        self.assertEqual(bot["attributes"]["color"], "#1FCB72")
        self.assertEqual(bot["attributes"]["shape"], "rounded_hexagon")
        routine = entities["sensor.grokbot_usage_routine_hulk_voice_path"]
        self.assertEqual(routine["state"], 22)
        self.assertEqual(routine["attributes"]["bot"], "Hulk")

    def test_publish_skips_without_supervisor_token(self):
        async def run():
            await usage.publish_sensors(None, usage.sanitize(SAMPLE))
        import asyncio
        with patch.dict("os.environ", {}, clear=False):
            # Ensure missing token is a no-op even if a leftover env var exists in the test process.
            import os
            os.environ.pop("SUPERVISOR_TOKEN", None)
            asyncio.run(run())


if __name__ == "__main__":
    unittest.main()
