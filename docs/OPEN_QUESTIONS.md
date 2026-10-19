# Open questions

1. `POST /v1/realtime/client_secrets` accepts `expires_after` and does not accept a model or a voice. The device sends `session.update` with voice `leo` and `grok-voice-3.0`. That call was not made against a live xAI account.
2. Whether xAI bills the whole `expires_after` window, or only audio time, is unmeasured. The Worker charges the daily minute cap for the whole window up front.
3. Durable Objects on the Cloudflare free plan versus Workers Paid ($5) was not deployed, so it is unconfirmed.
4. A Grok Bot webhook sandbox could `curl` an arbitrary `reply_url` in the owner's test on 18 October 2026. That was not repeated here. How to hand off to other bots without the routine echoing `reply_token` into the visible transcript depends on the bot product's tool UI.
5. The esp-sr 2.6 Kconfig symbol `CONFIG_SR_WN_WN9_JARVIS_TTS` (`wn9_jarvis_tts`) has to be confirmed when the component manager resolves the pin. The firmware build was not run here.
6. Hardware is untested: servos, microphone, speaker, PY32 LEDs, wake word, OTA, captive portal, BLE, display, lip sync, and a live xAI session. See `firmware/docs/UNTESTED.md`.
7. The M5GFX I2C lock and the AXP2101 behaviour with the base attached still apply if M5Unified is used the same way as groki-bot. Eyes and privacy share a core as a mitigation. Untested.
8. `check_static_ram.py` uses a 116 KiB floor. The plan text said 96 KiB. The check is static only and depends on `esp_idf_size` JSON field `diram_remain`.
9. Servo software limits are groki-bot's relative degrees. The mechanical tilt is about 5–85°. Those are not the same number. Calibrate on the assembled head.
10. A second WebSocket from the same device id replaces the first (close code 4000). A different device id is a 401 even with a valid HMAC. Both are tested in `worker/test`.
11. A custom MCP server is not in this version.
12. There is no web flasher.
13. How much of a Grok Bot subscription one webhook run consumes is unknown.
14. `speak_text` sends `conversation.item.create` with `input_text`, then `response.create`. Hold-to-talk streams with `server_vad` and does not send `input_audio_buffer.commit`. Neither was checked against a live session. xAI's October 2026 notes say commit is only for `turn_detection` null.
15. `REQUIRES` names `m5stack__M5Unified`, `espressif__esp-sr`, and `espressif__esp-dsp`. The component manager may register the short names instead.
16. This environment has no Docker. The ESP-IDF 5.5.4 image was not executed. If `espressif/idf:v5.5.4` is missing, the workflow fails with that pull error; `espressif/idf:release-v5.5` is the fallback to try, and the exact error should be kept.
17. `@cloudflare/vitest-pool-workers` 0.10 is deprecated in favor of `@cloudflare/vitest-plugin`. Tests pass on 0.10. `npm audit` reports vulnerabilities in the tool tree. They were not force-fixed.
18. The Worker's rate-limit binding is configured as 30 requests per 60 seconds. That is the Wrangler simple limiter in `wrangler.jsonc`, not a secret.
