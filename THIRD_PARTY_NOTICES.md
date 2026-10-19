# Third-party notices

Jarvis is a personal, non-commercial hobby project. Our own code is MIT (`LICENSE`). The pieces below keep their own licenses. Do not strip those notices when you move a file.

## groki-bot

- Source: https://github.com/JosssphZhou/groki-bot
- Firmware files under `firmware/components/scs_servo/`, `firmware/components/groki_motion/`, `firmware/components/servo_limits/`, `firmware/components/stackchan_board/` (the Si12T and PY32 drivers), and `firmware/third_party/groki-bot/` are Boost Software License 1.0. The license text is `firmware/third_party/groki-bot/LICENSE`. Copyright holders named upstream include Kenta IDA, Joseph Zhou, and sefuzhou770801-hub, as written on each file.
- `firmware/tools/check_static_ram.py` is BSL-1.0, Copyright 2026 Joseph Zhou.
- The groki-bot gateway was MIT. It is not included. The Grok Bot login-voice path (Keychain decryption, private backend calls, client spoofing) and the third-party `gbot` CLI are not included.

## Emotion Ball eye data

- `firmware/third_party/emotion-ball/` is the non-commercial eye-ring data and its `LICENSE` and `NOTICE.md`.
- That folder is not compiled. The default face is the MIT eye model.
- A firmware build that compiles this folder is non-commercial only. The notice also restricts the ball character's visual design. Read `LICENSE` and `NOTICE.md` before copying anything out of the folder.

## zongzhu-bot

- Source: https://github.com/Liu-zhongxian/zongzhu-bot
- `firmware/components/jarvis_eye/include/eyes/eye_model.h` and `firmware/third_party/zongzhu-bot/eye_model.h` are MIT. Copyright 2025 Shenzhen Xinzhi Future Technology Co., Ltd. and Project Contributors. The license is `firmware/third_party/zongzhu-bot/LICENSE`.
- Prebuilt zongzhu firmware and any keys from that repository are not included.

## TartanLlama expected

- `firmware/components/tl_expected/include/tl/expected.hpp` is the CC0 1.0 dedication by Sy Brand (TartanLlama), expected v1.1.0. The dedication is at the top of that header. https://github.com/TartanLlama/expected

## Espressif and M5Stack (fetched at build time, not vendored)

- `espressif/esp-sr` ~2.6.0 and `espressif/esp-dsp` are Espressif component-registry packages. esp-sr's model license restricts use to Espressif chips. Their license files arrive with the component manager download under `managed_components/`, which is gitignored.
- `m5stack/M5Unified` (and M5GFX, which it brings) is fetched the same way. Keep the license that ships inside the downloaded component.

## Cloudflare tooling

- The Worker tests use Wrangler, Miniflare, and `@cloudflare/vitest-pool-workers`. Those are development dependencies in `worker/package-lock.json` and are not copied into the firmware.
