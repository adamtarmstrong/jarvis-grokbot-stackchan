# SPDX-License-Identifier: GPL-3.0-or-later
"""Usage snapshot cache, JSONL history, and Home Assistant sensors."""
import json
import logging
import os
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path
from urllib.parse import urlsplit

from aiohttp import ClientSession, ClientTimeout, web

LOG = logging.getLogger("jarvis")
SCHEMA = "stackchan.usage.v1"
RETENTION_DAYS = 90
SHAPES = {
    "rounded_hexagon", "rounded_square", "blob", "wedge", "house",
    "heart", "sparkle", "star", "pill",
}
ON_DEMAND = {"off", "available", "active"}
SLUG = re.compile(r"[^a-z0-9]+")


def _pct(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    n = int(round(value))
    return n if 0 <= n <= 100 else None


def _text(value, max_len):
    if not isinstance(value, str):
        return None
    text = value.strip()
    if not text or len(text) > max_len or "\0" in text:
        return None
    return text


def _color(value):
    if not isinstance(value, str) or not re.fullmatch(r"#[0-9A-Fa-f]{6}", value):
        return None
    return value.upper()


def _days(value, limit=7):
    if not isinstance(value, list) or len(value) > limit:
        return None
    days = []
    for item in value:
        if not isinstance(item, dict):
            return None
        label = _text(item.get("d"), 8)
        used = _pct(item.get("used_pct"))
        if label is None or used is None:
            return None
        days.append({"d": label, "used_pct": used})
    return days


def sanitize(payload):
    """Return a sanitized snapshot, or None if it is not usable."""
    if not isinstance(payload, dict):
        return None
    if payload.get("type") not in (None, "usage_snapshot"):
        return None
    if payload.get("schema") not in (None, SCHEMA):
        return None
    fetched = _text(payload.get("fetched_at"), 40)
    period = payload.get("period")
    if fetched is None or not isinstance(period, dict):
        return None
    used = _pct(period.get("used_pct"))
    resets_at = _text(period.get("resets_at"), 40)
    on_demand = _text(period.get("on_demand"), 16)
    if used is None or resets_at is None or on_demand not in ON_DEMAND:
        return None
    resets_label = period.get("resets_label")
    if resets_label is not None:
        resets_label = _text(resets_label, 80)
        if resets_label is None:
            return None
    days = _days(payload.get("days"), 7)
    bots_in = payload.get("bots")
    if days is None or not isinstance(bots_in, list) or not 1 <= len(bots_in) <= 16:
        return None
    bots = []
    for item in bots_in:
        if not isinstance(item, dict):
            return None
        bot_id = _text(item.get("id"), 48)
        name = _text(item.get("name"), 40)
        shape = _text(item.get("avatar_shape"), 24)
        color = _color(item.get("avatar_color"))
        share = _pct(item.get("share_pct"))
        if not all((bot_id, name, shape, color is not None, share is not None)):
            return None
        if shape not in SHAPES or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,47}", bot_id):
            return None
        bot_days = _days(item.get("days") or [], 7)
        if bot_days is None:
            return None
        bot = {
            "id": bot_id,
            "name": name,
            "avatar_shape": shape,
            "avatar_color": color,
            "share_pct": share,
            "routines": [],
            "days": bot_days,
        }
        activity = item.get("activity")
        if isinstance(activity, dict):
            act = {}
            for key in ("chats_pct", "routines_pct", "background_pct", "browser_pct"):
                n = _pct(activity.get(key))
                if n is None:
                    act = None
                    break
                act[key] = n
            if act is not None:
                bot["activity"] = act
        routines = item.get("routines") or []
        if isinstance(routines, list):
            for routine in routines[:8]:
                if not isinstance(routine, dict):
                    continue
                rname = _text(routine.get("name"), 48)
                rshare = _pct(routine.get("share_pct"))
                if rname is not None and rshare is not None:
                    bot["routines"].append({"name": rname, "share_pct": rshare})
        bots.append(bot)
    out = {
        "type": "usage_snapshot",
        "schema": SCHEMA,
        "fetched_at": fetched,
        "period": {"used_pct": used, "resets_at": resets_at, "on_demand": on_demand},
        "days": days,
        "bots": bots,
    }
    if resets_label:
        out["period"]["resets_label"] = resets_label
    if payload.get("_example") is True:
        out["_example"] = True
    return out


def slug(value):
    text = SLUG.sub("_", (value or "").lower()).strip("_")
    return (text or "item")[:48]


def log_path(data_root=None):
    root = Path(data_root or os.environ.get("JARVIS_USAGE_DIR") or "/data/usage")
    return root / "snapshots.jsonl"


class UsageStore:
    def __init__(self, data_root=None):
        self.path = log_path(data_root)
        self.latest = None

    def load_latest(self):
        if self.latest is not None:
            return self.latest
        if not self.path.is_file():
            return None
        try:
            lines = self.path.read_text().splitlines()
        except OSError:
            return None
        for line in reversed(lines):
            line = line.strip()
            if not line:
                continue
            try:
                snapshot = sanitize(json.loads(line))
            except (ValueError, TypeError, json.JSONDecodeError):
                continue
            if snapshot and snapshot.get("_example") is not True:
                self.latest = snapshot
                return snapshot
        return None

    def accept(self, payload):
        snapshot = sanitize(payload)
        if snapshot is None or snapshot.get("_example") is True:
            return None
        previous = self.latest or self.load_latest()
        if previous and previous.get("fetched_at") == snapshot["fetched_at"]:
            self.latest = snapshot
            return snapshot
        self.latest = snapshot
        try:
            self._append(snapshot)
        except OSError as error:
            LOG.warning("usage log write failed error=%s", type(error).__name__)
        return snapshot

    def _append(self, snapshot):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        cutoff = datetime.now(timezone.utc) - timedelta(days=RETENTION_DAYS)
        kept = []
        if self.path.is_file():
            try:
                for line in self.path.read_text().splitlines():
                    if not line.strip():
                        continue
                    try:
                        row = json.loads(line)
                        fetched = _parse_time(row.get("fetched_at"))
                    except (ValueError, TypeError, json.JSONDecodeError):
                        continue
                    if fetched is None or fetched >= cutoff:
                        kept.append(json.dumps(row, separators=(",", ":")))
            except OSError:
                kept = []
        kept.append(json.dumps(snapshot, separators=(",", ":")))
        self.path.write_text("\n".join(kept) + "\n")

    def message(self):
        return self.latest or self.load_latest()


def _parse_time(value):
    if not isinstance(value, str):
        return None
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed


def sensor_payloads(snapshot):
    """Entity id + Home Assistant state documents for one snapshot."""
    period = snapshot["period"]
    fetched = snapshot["fetched_at"]
    payloads = [
        ("sensor.grokbot_usage_used_pct", {
            "state": period["used_pct"],
            "attributes": {
                "friendly_name": "Grok Bot weekly usage",
                "unit_of_measurement": "%",
                "icon": "mdi:chart-donut",
                "fetched_at": fetched,
                "resets_at": period["resets_at"],
                "on_demand": period["on_demand"],
            },
        }),
        ("sensor.grokbot_usage_resets_at", {
            "state": period["resets_at"],
            "attributes": {
                "friendly_name": "Grok Bot usage resets",
                "device_class": "timestamp",
                "fetched_at": fetched,
            },
        }),
        ("sensor.grokbot_usage_on_demand", {
            "state": period["on_demand"],
            "attributes": {
                "friendly_name": "Grok Bot on-demand",
                "fetched_at": fetched,
            },
        }),
    ]
    for bot in snapshot["bots"]:
        entity = "sensor.grokbot_usage_bot_" + slug(bot["id"])
        payloads.append((entity, {
            "state": bot["share_pct"],
            "attributes": {
                "friendly_name": f"Grok Bot share {bot['name']}",
                "unit_of_measurement": "%",
                "name": bot["name"],
                "bot_id": bot["id"],
                "color": bot["avatar_color"],
                "shape": bot["avatar_shape"],
                "share_pct": bot["share_pct"],
                "fetched_at": fetched,
            },
        }))
        for routine in bot.get("routines") or []:
            entity = "sensor.grokbot_usage_routine_" + slug(bot["id"] + "_" + routine["name"])
            payloads.append((entity, {
                "state": routine["share_pct"],
                "attributes": {
                    "friendly_name": f"{bot['name']} {routine['name']}",
                    "unit_of_measurement": "%",
                    "name": routine["name"],
                    "bot": bot["name"],
                    "bot_id": bot["id"],
                    "share_pct": routine["share_pct"],
                    "fetched_at": fetched,
                },
            }))
    return payloads


async def publish_sensors(session, snapshot):
    token = os.environ.get("SUPERVISOR_TOKEN") or ""
    if not token or not snapshot:
        return
    headers = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}
    for entity, body in sensor_payloads(snapshot):
        url = "http://supervisor/core/api/states/" + entity
        try:
            async with session.post(url, json=body, headers=headers, allow_redirects=False) as response:
                await response.read()
                if response.status >= 400:
                    LOG.warning("usage sensor publish failed entity=%s status=%s", entity, response.status)
        except Exception as error:
            LOG.warning("usage sensor publish failed entity=%s error=%s", entity, type(error).__name__)
            return


async def fetch_snapshot(options, session):
    if not options.get("worker_url"):
        return None
    # Imported lazily to avoid a cycle at module import in tests that stub worker_headers.
    from bridge import worker_headers
    url = options["worker_url"].rstrip("/") + "/usage"
    headers = worker_headers(options, "GET", urlsplit(url).path, b"")
    try:
        async with session.get(url, headers=headers, allow_redirects=False) as response:
            if response.status == 404:
                return None
            if response.status != 200:
                LOG.warning("usage fetch rejected status=%s", response.status)
                await response.read()
                return None
            payload = await response.json(content_type=None)
    except Exception as error:
        LOG.warning("usage fetch failed error=%s", type(error).__name__)
        return None
    return sanitize(payload)


def empty_waiting():
    return {"type": "usage_snapshot", "schema": SCHEMA, "waiting": True}
