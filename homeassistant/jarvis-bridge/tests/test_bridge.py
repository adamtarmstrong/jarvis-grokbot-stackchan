# SPDX-License-Identifier: GPL-3.0-or-later
import asyncio
import base64
import hashlib
import hmac
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from aiohttp import ClientSession, WSServerHandshakeError, web
from aiohttp.test_utils import TestClient, TestServer, make_mocked_request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bridge
import usage


OPTIONS = {"bridge_key": "test-pairing-not-a-real-secret", "worker_url": "https://worker.invalid/prefix123",
           "device_id": "test-device", "device_token": "test-token-not-a-real-secret", "hmac_key": "test-hmac-not-a-real-secret",
           "reply_timeout": 2, "bind_port": 10700, "stt_model": "tiny.en", "piper_voice": bridge.ALAN}
START = {"type": "start_listen", "sample_rate": 16000, "channels": 1, "format": "s16le"}
HEADERS = {"X-Jarvis-Bridge-Key": OPTIONS["bridge_key"]}
VOICE = struct.pack("<800h", *([5000] * 800))  # above Endpointing.VOICE_RMS (2500)
SILENCE = bytes(1600)


class SpeechStub:
    def __init__(self):
        self.input = None
        self.reply = None

    def transcribe(self, audio):
        self.input = audio
        return "Test request"

    def synthesize(self, text):
        self.reply = text
        return VOICE


class UnitTests(unittest.TestCase):
    def test_signature_known_vector(self):
        result = bridge.worker_headers(OPTIONS, "POST", "/prefix123/ask", b'{"text":"hi"}', timestamp="1794160000", nonce="abcdefghijklmnop")
        canonical = 'JARVIS1\nPOST\n/prefix123/ask\ntest-device\n1794160000\nabcdefghijklmnop\n' + hashlib.sha256(b'{"text":"hi"}').hexdigest()
        expected = hmac.new(OPTIONS["hmac_key"].encode(), canonical.encode(), hashlib.sha256).hexdigest()
        self.assertEqual(result["X-Jarvis-Signature"], expected)

    def test_turn_budget_covers_reply_timeout(self):
        self.assertGreaterEqual(bridge.TURN_SECONDS, 120)
        self.assertGreater(bridge.TURN_SECONDS, 50)  # max reply_timeout schema
        self.assertGreaterEqual(bridge.IDLE_SECONDS, 600)

    def test_options_fail_closed(self):
        bridge.validate_options(OPTIONS)
        for overrides in ({"bridge_key": ""}, {"worker_url": "http://bad"}, {"hmac_key": ""},
                          {"bind_port": 12345}, {"stt_model": "arbitrary"}, {"reply_timeout": 300}):
            with self.assertRaises(ValueError):
                bridge.validate_options({**OPTIONS, **overrides})

    def test_endpoint_pause_and_bounds(self):
        endpoint = bridge.Endpointing()
        # Warm past the 1.0 s grace, then pause ~1.2 s of silence to end.
        for _ in range(20):
            self.assertFalse(endpoint.append(VOICE))
        for _ in range(23):
            self.assertFalse(endpoint.append(SILENCE))
        self.assertTrue(endpoint.append(SILENCE))
        for data in (b"x", b"", bytes(3202)):
            with self.assertRaises(ValueError):
                bridge.Endpointing().append(data)
        endpoint.audio = bytearray(bridge.MAX_AUDIO)
        with self.assertRaises(ValueError):
            endpoint.append(VOICE)

    def test_private_models_confined_to_share(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "private-high.onnx").touch()
            (root / "private-high.onnx.json").touch()
            self.assertEqual(bridge.private_model("private-high", root), root / "private-high.onnx")
            for name in ("../outside", "/tmp/voice", "not-installed", "with space"):
                with self.assertRaises(ValueError):
                    bridge.private_model(name, root)
            (root / "escape.onnx").symlink_to("/etc/hosts")
            (root / "escape.onnx.json").touch()
            with self.assertRaises(ValueError):
                bridge.private_model("escape", root)


    def test_redact_skips_empty_and_short_secrets(self):
        options = {**OPTIONS, "webhook_key": "", "bridge_key": "x"}
        reason = bridge.redact_reason("worker request rejected status=502 reason=upstream_failed", options)
        self.assertEqual(reason, "worker request rejected status=502 reason=upstream_failed")
        self.assertNotIn("***w***", reason)

    def test_redact_masks_long_secrets(self):
        options = {**OPTIONS, "webhook_key": "test-webhook-not-real"}
        reason = bridge.redact_reason("leak test-webhook-not-real here", options)
        self.assertEqual(reason, "leak *** here")



class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.worker_ws = None
        self.acks = []
        self.played_ack = asyncio.Event()
        self.requests = []
        self.direct_requests = []
        self.fail_ask = False
        self.omit_reply = False
        self.worker = web.Application()
        self.worker.router.add_get("/prefix123/ws", self.worker_socket)
        self.worker.router.add_post("/prefix123/ask", self.worker_ask)
        self.worker.router.add_post("/webhook", self.webhook)
        self.worker_server = TestServer(self.worker)
        await self.worker_server.start_server()
        self.session = ClientSession()
        self.options = {**OPTIONS, "worker_url": str(self.worker_server.make_url("/prefix123"))}
        self.speech = SpeechStub()
        self.usage_dir = tempfile.TemporaryDirectory()
        self.bridge = bridge.Bridge(self.options, self.speech, self.session)
        self.bridge.usage = usage.UsageStore(self.usage_dir.name)
        self.client = TestClient(TestServer(self.bridge.app()))
        await self.client.start_server()
        # Only tests bypass public HTTPS endpoint validation for loopback fake servers.
        self.validation = patch.object(bridge, "secure_url", lambda url: url.rstrip("/"))
        self.validation.start()

    async def asyncTearDown(self):
        self.validation.stop()
        self.usage_dir.cleanup()
        await self.client.close()
        await self.session.close()
        await self.worker_server.close()

    async def worker_socket(self, request):
        self.assertEqual(request.headers["X-Jarvis-Device-Id"], OPTIONS["device_id"])
        self.worker_ws = web.WebSocketResponse()
        await self.worker_ws.prepare(request)
        async for msg in self.worker_ws:
            self.acks.append(json.loads(msg.data))
            if self.acks[-1].get("state") == "played":
                self.played_ack.set()
        return self.worker_ws

    async def worker_ask(self, request):
        body = await request.read()
        self.requests.append(json.loads(body))
        expected = bridge.worker_headers(OPTIONS, "POST", request.path, body,
                    timestamp=request.headers["X-Jarvis-Timestamp"], nonce=request.headers["X-Jarvis-Nonce"])
        self.assertEqual(request.headers["X-Jarvis-Signature"], expected["X-Jarvis-Signature"])
        if self.fail_ask:
            return web.Response(status=401)
        if not self.omit_reply:
            await self.worker_ws.send_json({"type": "reply", "request_id": "turn-test", "text": "Test answer"})
        return web.json_response({"request_id": "turn-test", "status": "pending"})

    async def webhook(self, request):
        payload = await request.json()
        self.direct_requests.append(payload)
        self.assertEqual(request.headers["Authorization"], "Bearer test-webhook-not-real")
        async with self.session.post(payload["reply_url"], json={"request_id": payload["request_id"],
                "reply_token": payload["reply_token"], "text": "Direct test answer"}) as response:
            self.assertEqual(response.status, 204)
        return web.Response(status=202)

    async def test_direct_webhook_full_roundtrip(self):
        self.options.update(worker_url="", webhook_url=str(self.worker_server.make_url("/webhook")),
                            webhook_key="test-webhook-not-real", public_reply_url=str(self.client.make_url("/bot/reply")))
        ws = await self.connect()
        await self.speak_and_end(ws)
        self.assertEqual((await ws.receive_json())["type"], "processing")
        while (await ws.receive_json())["type"] != "response.done":
            pass
        await self.send_played(ws)
        self.assertEqual(self.speech.reply, "Direct test answer")
        self.assertEqual(self.direct_requests[0]["text"], "Test request")
        self.assertEqual(self.bridge.replies.pending, {})
        self.assertEqual(self.requests, [])

    async def test_wrong_input_format_fails_before_webhook(self):
        ws = await self.client.ws_connect("/voice", headers=HEADERS)
        await self.drain_usage(ws)
        await ws.send_json({**START, "sample_rate": 24000})
        self.assertEqual((await ws.receive_json())["type"], "error")
        await ws.close()
        self.assertEqual(self.requests, [])


    async def speak_and_end(self, ws):
        # RATE bytes (~0.5 s) required after end_listen; one VOICE frame is only 1600 B.
        for _ in range(12):
            await ws.send_json({"type": "audio", "audio": base64.b64encode(VOICE).decode()})
        await ws.send_json({"type": "end_listen"})

    async def drain_usage(self, ws):
        message = await ws.receive_json()
        self.assertEqual(message["type"], "usage_snapshot")
        return message

    async def connect(self):
        ws = await self.client.ws_connect("/voice", headers=HEADERS)
        await self.drain_usage(ws)
        await ws.send_json(START)
        self.assertEqual((await ws.receive_json())["type"], "ready")
        return ws

    async def wait_until_idle(self, timeout=2):
        deadline = asyncio.get_running_loop().time() + timeout
        while self.bridge.busy:
            if asyncio.get_running_loop().time() > deadline:
                self.fail("bridge stayed busy after the turn")
            await asyncio.sleep(0.01)

    async def send_played(self, ws):
        await ws.send_json({"type": "played"})
        await self.wait_until_idle()
        self.assertFalse(ws.closed)

    async def test_worker_pcm_roundtrip_and_played_ack(self):
        ws = await self.connect()
        await self.speak_and_end(ws)
        self.assertEqual((await ws.receive_json())["type"], "processing")
        self.assertEqual((await ws.receive_json())["type"], "play")
        audio = await ws.receive_json()
        self.assertEqual(base64.b64decode(audio["delta"]), VOICE)
        self.assertEqual((await ws.receive_json())["type"], "response.done")
        # Already-buffered mic frames must not be mistaken for played confirmation.
        await ws.send_json({"type": "audio", "audio": "AAA="})
        await self.send_played(ws)
        await asyncio.wait_for(self.played_ack.wait(), 1)
        self.assertEqual(self.requests, [{"text": "Test request"}])
        self.assertEqual(self.speech.input, VOICE * 12)
        self.assertEqual(self.speech.reply, "Test answer")
        self.assertEqual([event["state"] for event in self.acks], ["received", "played"])

    async def test_pause_automatically_ends_input(self):
        ws = await self.connect()
        for data in [VOICE] * 30 + [SILENCE] * 26:
            await ws.send_json({"type": "audio", "audio": base64.b64encode(data).decode()})
        self.assertEqual((await ws.receive_json())["type"], "processing")
        while (await ws.receive_json())["type"] != "response.done":
            pass
        await ws.send_json({"type": "played"})
        await ws.close()

    async def test_auth_busy_and_invalid_audio(self):
        with self.assertRaises(WSServerHandshakeError) as error:
            await self.client.ws_connect("/voice")
        self.assertEqual(error.exception.status, 401)
        ws = await self.connect()
        with self.assertRaises(WSServerHandshakeError) as error:
            await self.client.ws_connect("/voice", headers=HEADERS)
        self.assertEqual(error.exception.status, 409)
        await ws.send_json({"type": "audio", "audio": "invalid base64!"})
        self.assertEqual((await ws.receive_json())["type"], "error")
        await ws.close()
        self.assertEqual(self.requests, [])

    async def test_worker_rejection_never_falls_back(self):
        self.fail_ask = True
        with self.assertLogs("jarvis", level="WARNING") as logs:
            ws = await self.connect()
            await self.speak_and_end(ws)
            self.assertEqual((await ws.receive_json())["type"], "processing")
            self.assertEqual((await ws.receive_json())["type"], "error")
            self.assertIsNone(self.speech.reply)
            await ws.close()
        joined = "\n".join(logs.output)
        self.assertIn("status=401", joined)
        self.assertIn("reason=auth_failed", joined)
        self.assertNotIn("***w***", joined)

    async def test_reply_timeout(self):
        self.omit_reply = True
        self.options["reply_timeout"] = 0.05
        with self.assertLogs("jarvis", level="WARNING") as logs:
            ws = await self.connect()
            await self.speak_and_end(ws)
            self.assertEqual((await ws.receive_json())["type"], "processing")
            self.assertEqual((await ws.receive_json())["type"], "error")
            await ws.close()
        joined = "\n".join(logs.output)
        self.assertIn("reply_wait_timeout", joined)
        self.assertNotIn("turn_budget_timeout", joined)

    async def test_turn_timing_log_on_success(self):
        with self.assertLogs("jarvis", level="WARNING") as logs:
            ws = await self.connect()
            await self.speak_and_end(ws)
            self.assertEqual((await ws.receive_json())["type"], "processing")
            while (await ws.receive_json())["type"] != "response.done":
                pass
            await self.send_played(ws)
        joined = "\n".join(logs.output)
        self.assertIn("turn timing listen=", joined)
        self.assertIn("stt=", joined)
        self.assertIn("ask=", joined)
        self.assertIn("jarvis_wait=", joined)
        self.assertIn("tts_first_audio=", joined)
        self.assertIn("total=", joined)

    async def test_multi_turn_on_one_socket(self):
        ws = await self.connect()
        await self.speak_and_end(ws)
        self.assertEqual((await ws.receive_json())["type"], "processing")
        while (await ws.receive_json())["type"] != "response.done":
            pass
        await self.send_played(ws)
        await ws.send_json(START)
        self.assertEqual((await ws.receive_json())["type"], "ready")
        await self.speak_and_end(ws)
        self.assertEqual((await ws.receive_json())["type"], "processing")
        while (await ws.receive_json())["type"] != "response.done":
            pass
        await self.send_played(ws)
        self.assertEqual(self.requests, [{"text": "Test request"}, {"text": "Test request"}])
        await ws.close()

    async def test_stale_idle_socket_is_replaced(self):
        idle = await self.client.ws_connect("/voice", headers=HEADERS)
        await self.drain_usage(idle)
        self.assertFalse(idle.closed)
        ws = await self.connect()
        closed = await idle.receive()
        self.assertTrue(idle.closed or closed.type.name in ("CLOSE", "CLOSING", "CLOSED"))
        await self.speak_and_end(ws)
        self.assertEqual((await ws.receive_json())["type"], "processing")
        while (await ws.receive_json())["type"] != "response.done":
            pass
        await self.send_played(ws)
        await ws.close()

    async def test_idle_socket_does_not_block_second_connect_after_turn(self):
        first = await self.connect()
        await self.speak_and_end(first)
        self.assertEqual((await first.receive_json())["type"], "processing")
        while (await first.receive_json())["type"] != "response.done":
            pass
        await self.send_played(first)
        second = await self.client.ws_connect("/voice", headers=HEADERS)
        await self.drain_usage(second)
        closed = await first.receive()
        self.assertTrue(first.closed or closed.type.name in ("CLOSE", "CLOSING", "CLOSED"))
        self.assertFalse(second.closed)
        await second.close()

    async def test_direct_callback_one_time_and_expiry(self):
        future = asyncio.get_running_loop().create_future()
        digest = hashlib.sha256(b"one-time-test").digest()
        self.bridge.replies.pending["direct"] = (digest, future)
        url = "/bot/reply"
        body = {"request_id": "direct", "reply_token": "wrong", "text": "answer"}
        self.assertEqual((await self.client.post(url, json=body)).status, 401)
        body["reply_token"] = "one-time-test"
        self.assertEqual((await self.client.post(url, json=body)).status, 204)
        self.assertEqual(await future, "answer")
        self.assertEqual((await self.client.post(url, json=body)).status, 401)
        future = asyncio.get_running_loop().create_future()
        future.cancel()
        self.bridge.replies.pending["direct"] = (digest, future)
        self.assertEqual((await self.client.post(url, json=body)).status, 401)


    async def test_usage_snapshot_on_connect_and_request(self):
        sample = {
            "type": "usage_snapshot",
            "schema": "stackchan.usage.v1",
            "fetched_at": "2026-11-10T18:00:00-05:00",
            "period": {"used_pct": 62, "resets_at": "2026-11-16T00:00:00-05:00", "on_demand": "off"},
            "days": [{"d": "Mon", "used_pct": 7}],
            "bots": [{
                "id": "hulk", "name": "Hulk", "avatar_shape": "rounded_hexagon",
                "avatar_color": "#1FCB72", "share_pct": 38, "routines": [], "days": [],
            }],
        }
        self.assertIsNotNone(self.bridge.usage.accept(sample))
        ws = await self.client.ws_connect("/voice", headers=HEADERS)
        hello = await self.drain_usage(ws)
        self.assertEqual(hello["period"]["used_pct"], 62)
        self.assertEqual(hello["bots"][0]["name"], "Hulk")
        await ws.send_json({"type": "usage_request"})
        again = await self.drain_usage(ws)
        self.assertEqual(again["fetched_at"], sample["fetched_at"])
        await ws.send_json(START)
        self.assertEqual((await ws.receive_json())["type"], "ready")
        await self.speak_and_end(ws)
        self.assertEqual((await ws.receive_json())["type"], "processing")
        while (await ws.receive_json())["type"] != "response.done":
            pass
        await self.send_played(ws)
        self.assertEqual(self.requests, [{"text": "Test request"}])
        await ws.close()

    async def test_usage_request_on_empty_cache_is_waiting(self):
        ws = await self.client.ws_connect("/voice", headers=HEADERS)
        hello = await self.drain_usage(ws)
        self.assertTrue(hello.get("waiting"))
        await ws.close()
        self.assertEqual(self.requests, [])


if __name__ == "__main__":
    unittest.main()
