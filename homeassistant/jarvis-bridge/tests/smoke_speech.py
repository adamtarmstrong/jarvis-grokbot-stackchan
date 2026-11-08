# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional real-model smoke test. Downloads ONLY official Alan and Whisper tiny.en.
Usage: python tests/smoke_speech.py /absolute/scratch/models
"""
import asyncio
from pathlib import Path
import sys
import wave
from aiohttp import ClientSession, ClientTimeout
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bridge import Speech, ALAN


async def main(directory):
    directory.mkdir(parents=True, exist_ok=True)
    async with ClientSession(timeout=ClientTimeout(total=300)) as session:
        speech = await Speech.load({"stt_model": "tiny.en", "piper_voice": ALAN}, session, directory)
        pcm = await asyncio.to_thread(speech.synthesize, "Hello. This is a test of the local voice bridge.")
        assert len(pcm) > 3200 and len(pcm) % 2 == 0
        with wave.open(str(directory / "alan-smoke.wav"), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(16000)
            output.writeframes(pcm)
        transcript = await asyncio.to_thread(speech.transcribe, pcm)
        assert "test" in transcript.lower(), "STT did not recognize test phrase"
        print("PASS: official Alan -> 16kHz PCM -> real Whisper tiny.en; generated test phrase recognized")


if __name__ == "__main__":
    asyncio.run(main(Path(sys.argv[1])))
