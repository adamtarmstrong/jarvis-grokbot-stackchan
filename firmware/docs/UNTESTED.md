# What has not been run on a board

CI is expected to compile this tree with the ESP-IDF 5.5.4 image (`espressif/idf:v5.5.4`) and then run `tools/check_static_ram.py`. The machine that wrote this tree had no Docker daemon, so that image was not executed here. If the image tag does not exist, try `espressif/idf:release-v5.5` and record the exact error. Do not treat a green host test as a firmware build.

Host-only checks (no board, no IDF):

```sh
firmware/host/test.sh
```

These cover the HMAC vector, the canonical string, the brass accent, privacy dots, the eye model's moods, and the spring clamp. They do not link ESP-IDF.

## Untested on hardware

- SCS0009 yaw/pitch, torque-off when idle, and the mechanical tilt versus the software degree limits
- M5 microphone, half-duplex with the speaker, and FFT lip sync (`lip_sync.cpp`)
- PY32 LED mirror of the privacy dots, and the on-screen dots
- The 320×240 eye task (M5GFX). Priority is 1 on purpose
- WakeNet `wn9_jarvis_tts` and the Kconfig symbol `CONFIG_SR_WN_WN9_JARVIS_TTS`
- Hold-to-talk via the Si12T middle/back zones and the screen
- Captive portal, DNS hijack to 192.168.4.1, and BLE provisioning (`WIFI_PROV_SECURITY_1`, proof-of-possession printed once at boot)
- SNTP, HMAC signatures against a deployed Worker, and the device WebSocket
- xAI realtime: `session.update` voice placement, `server_vad`, PCM16 at 16 kHz, `ask_bot`, and read-back via `conversation.item.create`
- OTA (`esp_https_ota`) and rollback. The Worker does not send an `ota` message in this version; the firmware accepts one if a later Worker does
- Component names `m5stack__M5Unified`, `espressif__esp-sr`, and `espressif__esp-dsp`. If the component manager registers the short names, `main/CMakeLists.txt` `REQUIRES` has to change

## Not in this version

- A browser web flasher
- A compiled Emotion Ball face (see `firmware/third_party/emotion-ball/README.md`)
- An esp32-camera driver
- groki-bot's Grok Bot login-voice path and the third-party `gbot` CLI
