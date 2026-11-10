# SPDX-License-Identifier: GPL-3.0-or-later
"""One-turn LAN speech bridge. No transcript, URL, credential, or audio logging."""
import asyncio
import base64
import hashlib
import hmac
import json
import logging
import math
import os
from pathlib import Path
import re
import secrets
import time
from urllib.parse import urlsplit
import uuid

from aiohttp import ClientSession, ClientTimeout, WSMsgType, web

RATE = 16000
MAX_AUDIO = RATE * 2 * 20
MAX_TEXT = 2000
TURN_SECONDS = 120
IDLE_SECONDS = 600  # keep a persistent /voice socket; close only after this much silence
ALAN = "en_GB-alan-medium"
ALAN_BASE = "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_GB/alan/medium/"
LOG = logging.getLogger("jarvis")


def secure_url(value, scheme="https"):
    parsed = urlsplit(value)
    if parsed.scheme != scheme or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError("invalid endpoint configuration")
    return value.rstrip("/")


def worker_headers(options, method, path, body=b"", *, timestamp=None, nonce=None):
    timestamp = str(int(time.time())) if timestamp is None else timestamp
    nonce = secrets.token_urlsafe(24) if nonce is None else nonce
    canonical = "\n".join(("JARVIS1", method, path, options["device_id"], timestamp, nonce, hashlib.sha256(body).hexdigest()))
    signature = hmac.new(options["hmac_key"].encode(), canonical.encode(), hashlib.sha256).hexdigest()
    return {"Authorization": "Bearer " + options["device_token"], "X-Jarvis-Device-Id": options["device_id"],
            "X-Jarvis-Timestamp": timestamp, "X-Jarvis-Nonce": nonce, "X-Jarvis-Signature": signature}



WORKER_STATUS_REASON = {
    400: "bad_request",
    401: "auth_failed",
    404: "not_found",
    409: "duplicate",
    429: "rate_limited",
    500: "server_error",
    502: "upstream_failed",
}


async def worker_reject_message(response):
    """Build a log-safe rejection summary. Worker bare responses have no body."""
    code = WORKER_STATUS_REASON.get(response.status, "rejected")
    detail = code
    try:
        raw = await response.read()
        if raw and len(raw) <= 256 and response.content_type and "json" in response.content_type:
            payload = json.loads(raw)
            if isinstance(payload, dict):
                candidate = payload.get("reason") or payload.get("error") or payload.get("code")
                if isinstance(candidate, str) and candidate.replace("_", "").isalnum() and len(candidate) <= 32:
                    detail = candidate
    except (ValueError, TypeError, json.JSONDecodeError):
        pass
    return f"worker request rejected status={response.status} reason={detail}"


def redact_reason(reason, options):
    """Strip credentials from exception text. Skip empty/short secrets (''.replace inserts ***)."""
    text = str(reason).split("://")[0]
    for key in ("bridge_key", "device_token", "hmac_key", "webhook_key"):
        secret = options.get(key) or ""
        if len(secret) >= 8:
            text = text.replace(secret, "***")
    if len(text) > 120:
        text = text[:120]
    return text


class WorkerRelay:
    """Use the existing Worker /ask -> webhook -> /bot/reply -> /ws route."""
    def __init__(self, options, session):
        self.options, self.session = options, session
        self.socket = None
        self.request_id = None
        self.base = secure_url(options["worker_url"])

    async def ask(self, text):
        self.ask_seconds = 0.0
        self.jarvis_wait_seconds = 0.0
        ws_url = self.base.replace("https://", "wss://", 1) + "/ws"
        ask_started = time.monotonic()
        self.socket = await self.session.ws_connect(
            ws_url, headers=worker_headers(self.options, "GET", urlsplit(ws_url).path), max_msg_size=8192)
        body = json.dumps({"text": text}, separators=(",", ":")).encode()
        url = self.base + "/ask"
        async with self.session.post(url, data=body, allow_redirects=False,
                headers={**worker_headers(self.options, "POST", urlsplit(url).path, body), "Content-Type": "application/json"}) as response:
            if response.status != 200:
                raise RuntimeError(await worker_reject_message(response))
            self.request_id = (await response.json())["request_id"]
        self.ask_seconds = time.monotonic() - ask_started
        wait_started = time.monotonic()
        try:
            async with asyncio.timeout(self.options["reply_timeout"]):
                async for message in self.socket:
                    if message.type != WSMsgType.TEXT:
                        raise RuntimeError("worker disconnected")
                    event = json.loads(message.data)
                    if event.get("request_id") != self.request_id:
                        continue
                    if event.get("type") == "state" and event.get("state") == "timed_out":
                        raise TimeoutError("reply_wait_timeout")
                    if event.get("type") == "reply":
                        text = checked_text(event.get("text"))
                        self.jarvis_wait_seconds = time.monotonic() - wait_started
                        await self.ack("received")
                        return text
        except TimeoutError as error:
            if str(error) != "reply_wait_timeout":
                raise TimeoutError("reply_wait_timeout") from error
            raise
        raise RuntimeError("worker disconnected")

    async def ack(self, state):
        if self.socket and self.request_id:
            await self.socket.send_json({"type": "state", "request_id": self.request_id, "state": state})

    async def close(self):
        if self.socket:
            await self.socket.close()


def checked_text(text):
    if not isinstance(text, str) or not text.strip() or len(text) > MAX_TEXT:
        raise ValueError("invalid text")
    return text.strip()


class DirectReplies:
    """Optional direct webhook mode; requires an operator-provided HTTPS callback tunnel."""
    def __init__(self):
        self.pending = {}

    async def ask(self, options, session, text):
        request_id, token = str(uuid.uuid4()), secrets.token_urlsafe(32)
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = (hashlib.sha256(token.encode()).digest(), future)
        try:
            async with session.post(secure_url(options["webhook_url"]), allow_redirects=False,
                    headers={"Authorization": "Bearer " + options["webhook_key"]},
                    json={"text": text, "request_id": request_id, "reply_token": token,
                          "reply_url": secure_url(options["public_reply_url"])}) as response:
                if not 200 <= response.status < 300:
                    raise RuntimeError("webhook rejected")
                await response.read()
            try:
                return await asyncio.wait_for(future, options["reply_timeout"])
            except TimeoutError as error:
                raise TimeoutError("reply_wait_timeout") from error
        finally:
            self.pending.pop(request_id, None)

    async def callback(self, request):
        try:
            body = await request.json()
            request_id, token = body["request_id"], body["reply_token"]
            if not isinstance(request_id, str) or not isinstance(token, str) or len(token) > 256:
                raise ValueError()
            record = self.pending.get(request_id)
            if not record or not hmac.compare_digest(record[0], hashlib.sha256(token.encode()).digest()) or record[1].done():
                return web.Response(status=401)
            text = checked_text(body.get("text"))
            self.pending.pop(request_id)
            record[1].set_result(text)
            return web.Response(status=204)
        except (ValueError, KeyError, TypeError):
            return web.Response(status=401)


class Endpointing:
    """Energy endpointing tuned for StackChan mic mag 8; STT also runs Silero VAD.

    RMS 400 treated room noise as continuous speech (bar ran the full 15 s). At mag 8,
    desk speech is typically multi-thousand RMS; require that, then ~1.2 s of quiet.
    """
    # Keep in sync with firmware kHaVoiceRms / kHaSilenceSamples intent.
    VOICE_RMS = 2500
    MIN_VOICED = RATE // 4          # 250 ms of speech before silence can end
    SILENCE_SAMPLES = int(RATE * 1.2)  # ~1.2 s quiet ends the turn
    GRACE_BYTES = int(RATE * 2 * 1.0)  # 1.0 s before silence-end is allowed

    def __init__(self):
        self.audio = bytearray()
        self.voiced = 0
        self.silent = 0
        self.frames = 0

    def append(self, data):
        import numpy as np
        if len(data) % 2 or not data or len(data) > 3200 or len(self.audio) + len(data) > MAX_AUDIO:
            raise ValueError("invalid audio")
        self.audio.extend(data)
        self.frames += 1
        samples = np.frombuffer(data, dtype="<i2").astype(np.float32)
        if float(np.sqrt(np.mean(samples * samples))) >= self.VOICE_RMS:
            self.voiced += len(samples)
            self.silent = 0
        else:
            self.silent += len(samples)
        if len(self.audio) < self.GRACE_BYTES:
            return False
        return self.voiced >= self.MIN_VOICED and self.silent >= self.SILENCE_SAMPLES


def private_model(name, share=Path("/share/piper")):
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,96}", name):
        raise ValueError("custom_voice must be a filename stem, not a path")
    root = share.resolve()
    model = (root / (name + ".onnx")).resolve()
    config = (root / (name + ".onnx.json")).resolve()
    if model.parent != root or config.parent != root or not model.is_file() or not config.is_file():
        raise ValueError("custom voice files are missing or outside /share/piper")
    return model


class Speech:
    def __init__(self, stt, voice):
        self.stt, self.voice = stt, voice

    @classmethod
    async def load(cls, options, session, data_dir):
        custom = options.get("custom_voice", "")
        if custom:
            model = private_model(custom)
        else:
            model = data_dir / (ALAN + ".onnx")
            for filename in (ALAN + ".onnx", ALAN + ".onnx.json", "MODEL_CARD"):
                target = data_dir / filename
                if not target.exists():
                    temporary = target.with_suffix(target.suffix + ".part")
                    try:
                        async with session.get(ALAN_BASE + filename) as response:
                            response.raise_for_status()
                            with temporary.open("wb") as output:
                                async for chunk in response.content.iter_chunked(65536):
                                    output.write(chunk)
                        temporary.replace(target)
                    finally:
                        temporary.unlink(missing_ok=True)
        def load_models():
            from faster_whisper import WhisperModel
            from piper import PiperVoice
            stt = WhisperModel(options["stt_model"], device="cpu", compute_type="int8", cpu_threads=2,
                               num_workers=1, download_root=str(data_dir / "whisper"))
            return cls(stt, PiperVoice.load(str(model)))
        return await asyncio.to_thread(load_models)

    def transcribe(self, audio):
        import numpy as np
        samples = np.frombuffer(audio, dtype="<i2").astype(np.float32) / 32768.0
        segments, _ = self.stt.transcribe(samples, language="en", beam_size=1, vad_filter=True)
        text = " ".join(segment.text.strip() for segment in segments)
        # Firmware strips ~700 ms of trailing "stop"; still drop a lone trailing stop word.
        parts = text.strip().split()
        if len(parts) >= 1 and parts[-1].lower().strip(".,!?") == "stop":
            parts = parts[:-1]
            text = " ".join(parts)
        return checked_text(text)

    def synthesize(self, text):
        import numpy as np
        from scipy.signal import resample_poly
        chunks, size = [], 0
        for chunk in self.voice.synthesize(text):
            divisor = math.gcd(chunk.sample_rate, RATE)
            samples = np.frombuffer(chunk.audio_int16_bytes, dtype="<i2").astype(np.float32)
            pcm = np.clip(resample_poly(samples, RATE // divisor, chunk.sample_rate // divisor), -32768, 32767).astype("<i2").tobytes()
            size += len(pcm)
            if size > RATE * 2 * 45:
                raise ValueError("reply audio exceeds 45 seconds")
            chunks.append(pcm)
        return b"".join(chunks)


async def inference(function, value):
    task = asyncio.create_task(asyncio.to_thread(function, value))
    try:
        return await asyncio.shield(task)
    finally:
        if not task.done():
            await task


async def recv_event(ws, timeout):
    """Next JSON event, or None if the client closed. Heartbeats are handled by aiohttp."""
    message = await ws.receive(timeout=timeout)
    if message.type in (WSMsgType.CLOSE, WSMsgType.CLOSING, WSMsgType.CLOSED):
        return None
    if message.type == WSMsgType.ERROR:
        raise ConnectionError("socket error")
    if message.type != WSMsgType.TEXT:
        raise ValueError("audio required")
    return json.loads(message.data)


class Bridge:
    def __init__(self, options, speech, session):
        self.options, self.speech, self.session = options, speech, session
        self.busy = False
        self.idle_ws = None
        self.accept = asyncio.Lock()
        self.replies = DirectReplies()

    async def health(self, request):
        return web.json_response({"ready": True, "protocol": 1})

    async def _run_turn(self, ws, start, timing):
        relay = None
        try:
            async with asyncio.timeout(TURN_SECONDS):
                if (start.get("type") != "start_listen" or start.get("sample_rate") != RATE
                        or start.get("channels") != 1 or start.get("format") != "s16le"):
                    raise ValueError("start_listen required")
                await ws.send_json({"type": "ready", "sample_rate": RATE, "format": "s16le", "channels": 1})
                endpoint = Endpointing()
                listen_started = time.monotonic()
                started = listen_started
                while True:
                    event = await recv_event(ws, 8 if not endpoint.voiced else 3)
                    if event is None:
                        raise ConnectionError("client closed")
                    if event.get("type") == "end_listen":
                        break
                    if event.get("type") != "audio" or not isinstance(event.get("audio"), str):
                        raise ValueError("audio required")
                    data = base64.b64decode(event["audio"], validate=True)
                    if endpoint.append(data) or len(endpoint.audio) >= MAX_AUDIO:
                        break
                    if not endpoint.voiced and time.monotonic() - started > 8:
                        raise ValueError("no speech")
                timing["listen"] = time.monotonic() - listen_started
                await ws.send_json({"type": "processing"})
                if len(endpoint.audio) < RATE:  # < 0.5 s of PCM
                    raise ValueError("audio too short")
                stt_started = time.monotonic()
                try:
                    text = await inference(self.speech.transcribe, bytes(endpoint.audio))
                except ValueError:
                    LOG.warning("turn ended (ValueError: empty transcript)")
                    text = "I did not catch that. Please try again."
                timing["stt"] = time.monotonic() - stt_started
                if self.options.get("worker_url") and text != "I did not catch that. Please try again.":
                    relay = WorkerRelay(self.options, self.session)
                    reply = await relay.ask(text)
                    timing["ask"] = getattr(relay, "ask_seconds", 0.0)
                    timing["jarvis_wait"] = getattr(relay, "jarvis_wait_seconds", 0.0)
                elif self.options.get("worker_url"):
                    reply = text
                else:
                    reply = await self.replies.ask(self.options, self.session, text)
                tts_started = time.monotonic()
                pcm = await inference(self.speech.synthesize, reply)
                await ws.send_json({"type": "play", "sample_rate": RATE, "format": "s16le", "channels": 1})
                first_delta = True
                for offset in range(0, len(pcm), 3200):
                    await ws.send_json({"type": "response.output_audio.delta", "delta": base64.b64encode(pcm[offset:offset + 3200]).decode()})
                    if first_delta:
                        timing["tts_first_audio"] = time.monotonic() - tts_started
                        first_delta = False
                    await asyncio.sleep(0.09)
                if first_delta:
                    timing["tts_first_audio"] = time.monotonic() - tts_started
                await ws.send_json({"type": "response.done"})
                # Audio already in flight from the robot may follow processing. Drain it.
                while True:
                    event = await recv_event(ws, 10)
                    if event is None:
                        raise ConnectionError("client closed")
                    if event.get("type") == "played":
                        if relay:
                            await relay.ack("played")
                        break
        except TimeoutError as error:
            if str(error) == "reply_wait_timeout":
                raise
            raise TimeoutError("turn_budget_timeout") from error
        finally:
            if relay:
                await relay.close()

    async def socket(self, request):
        supplied = request.headers.get("X-Jarvis-Bridge-Key", "")
        if not hmac.compare_digest(supplied.encode(), self.options["bridge_key"].encode()):
            return web.Response(status=401)
        async with self.accept:
            if self.busy:
                return web.Response(status=409)
            stale = self.idle_ws
            self.idle_ws = None
        ws = web.WebSocketResponse(max_msg_size=8192, heartbeat=20)
        await ws.prepare(request)
        if stale is not None and stale is not ws and not stale.closed:
            try:
                await stale.close()
            except ConnectionError:
                pass
        self.idle_ws = ws
        try:
            while not ws.closed:
                self.idle_ws = ws
                try:
                    start = await recv_event(ws, IDLE_SECONDS)
                except TimeoutError:
                    break
                except (ConnectionError, json.JSONDecodeError, ValueError):
                    if ws.closed:
                        break
                    try:
                        await ws.send_json({"type": "error", "code": "turn_failed"})
                    except ConnectionError:
                        break
                    continue
                if start is None:
                    break
                self.busy = True
                self.idle_ws = None
                timing = {"listen": 0.0, "stt": 0.0, "ask": 0.0, "jarvis_wait": 0.0, "tts_first_audio": 0.0, "total": 0.0}
                turn_started = time.monotonic()
                try:
                    await self._run_turn(ws, start, timing)
                    timing["total"] = time.monotonic() - turn_started
                    LOG.warning(
                        "turn timing listen=%.1fs stt=%.1fs ask=%.1fs jarvis_wait=%.1fs tts_first_audio=%.1fs total=%.1fs",
                        timing["listen"], timing["stt"], timing["ask"], timing["jarvis_wait"],
                        timing["tts_first_audio"], timing["total"],
                    )
                except (Exception, asyncio.CancelledError) as error:
                    reason = redact_reason(error, self.options)
                    LOG.warning("turn ended (%s: %s)", type(error).__name__, reason or "no message")
                    if timing["total"] == 0.0:
                        timing["total"] = time.monotonic() - turn_started
                    if any(timing[k] for k in ("listen", "stt", "ask", "jarvis_wait", "tts_first_audio", "total")):
                        LOG.warning(
                            "turn timing listen=%.1fs stt=%.1fs ask=%.1fs jarvis_wait=%.1fs tts_first_audio=%.1fs total=%.1fs",
                            timing["listen"], timing["stt"], timing["ask"], timing["jarvis_wait"],
                            timing["tts_first_audio"], timing["total"],
                        )
                    if ws.prepared and not ws.closed:
                        try:
                            await ws.send_json({"type": "error", "code": "turn_failed"})
                        except ConnectionError:
                            pass
                    if isinstance(error, asyncio.CancelledError):
                        raise
                    if not ws.prepared or ws.closed:
                        break
                finally:
                    self.busy = False
        finally:
            if self.idle_ws is ws:
                self.idle_ws = None
            if not ws.closed:
                try:
                    await ws.close()
                except ConnectionError:
                    pass
        return ws

    def app(self):
        app = web.Application(client_max_size=8192)
        app.router.add_get("/health", self.health)
        app.router.add_get("/voice", self.socket)
        app.router.add_post("/bot/reply", self.replies.callback)
        return app


def validate_options(options):
    if len(options.get("bridge_key", "")) < 16:
        raise ValueError("set a bridge_key of at least 16 characters")
    if options.get("stt_model") not in ("tiny.en", "base.en", "small.en") or options.get("piper_voice") != ALAN:
        raise ValueError("invalid speech model selection")
    if not 1 <= options["reply_timeout"] <= 50 or options["bind_port"] != 10700:
        raise ValueError("use container bind_port 10700; change the host port in Supervisor Network")
    if options.get("worker_url"):
        secure_url(options["worker_url"])
        if not all(options.get(key) for key in ("device_id", "device_token", "hmac_key")):
            raise ValueError("worker credentials required")
    else:
        secure_url(options["webhook_url"])
        secure_url(options["public_reply_url"])
        if not options.get("webhook_key"):
            raise ValueError("webhook key required")


async def serve():
    options = json.loads(Path(os.environ.get("JARVIS_OPTIONS", "/data/options.json")).read_text())
    validate_options(options)
    data_dir = Path(os.environ.get("JARVIS_DATA", "/data/models"))
    data_dir.mkdir(parents=True, exist_ok=True)
    async with ClientSession(timeout=ClientTimeout(total=120)) as session:
        LOG.info("Loading local speech models; first start may download official Alan and Whisper")
        speech = await Speech.load(options, session, data_dir)
        bridge = Bridge(options, speech, session)
        runner = web.AppRunner(bridge.app(), access_log=None)
        await runner.setup()
        try:
            await web.TCPSite(runner, "0.0.0.0", options["bind_port"]).start()
            LOG.info("Local speech pipeline ready on port 10700")
            await asyncio.Event().wait()
        finally:
            await runner.cleanup()


if __name__ == "__main__":
    logging.basicConfig(level=logging.WARNING, format="%(asctime)s %(levelname)s %(name)s: %(message)s", datefmt="%Y-%m-%d %H:%M:%S")
    LOG.setLevel(logging.INFO)
    # Third-party debug logs can contain text, phonemes, or signed URLs.
    for name in ("aiohttp", "piper", "faster_whisper", "httpx", "huggingface_hub"):
        logging.getLogger(name).setLevel(logging.CRITICAL)
    try:
        asyncio.run(serve())
    except Exception as error:
        LOG.error("Startup failed (%s); check configuration and model availability", type(error).__name__)
        raise SystemExit(1)
