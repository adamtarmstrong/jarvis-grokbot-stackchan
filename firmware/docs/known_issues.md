# Known issues carried from groki-bot

The upstream notes are kept, in the original Japanese, at `firmware/third_party/groki-bot/known_issues.md` (BSL-1.0). This page is the short English list for people building Jarvis. None of it has been re-tested on a board in this repository.

## M5GFX I2C mutex

A long run (about 10–40 minutes) can reboot with `xTaskPriorityDisinherit` while one task uses the panel or the PY32 LEDs and another task is busy with audio. groki-bot's mitigation was to cut a read-modify-write in `refresh_leds`. Jarvis also pins the eye task and the privacy-LED task to the same core, at priority 1, below the microphone task. That is a mitigation, not a fix. It is untested.

## AXP2101 with the Stack-chan base attached

The power-management chip can come up in a bad state when the base is attached. This firmware does not add a new power-up sequence. If the board brownouts when servos move, start from the upstream note before changing the driver.

## App partition is nearly full on a feature-complete groki-bot build

`partitions.csv` gives each OTA slot 4 MiB. The speech-model partition is 6 MiB. A build that also pulls M5Unified, esp-sr, and the websocket client can still land close to the slot limit. The CI build is the place to see the actual size. This environment did not run that build (no Docker).

## Static RAM floor

`firmware/tools/check_static_ram.py` fails a CoreS3 map that leaves less than **116 KiB** of D/IRAM. The plan text said 96 KiB. Upstream raised the floor after measuring an IDF 5.5.4 / esp-sr 2.6.0 build that had about 124.5 KiB left, with the broken 2.5.4-era builds around 18 KiB. The script only sees static size via `python -m esp_idf_size` field `diram_remain`. It does not measure Wi-Fi plus a live voice session.

## esp-sr 2.5.4

That release boot-looped CoreS3 in groki-bot. `main/idf_component.yml` pins `espressif/esp-sr` to `~2.6.0`. The WakeNet preset is `wn9_jarvis_tts` (`CONFIG_SR_WN_WN9_JARVIS_TTS`). If a resolved 2.6.x drops that Kconfig symbol, `idf.py build` will warn and the model partition will not contain Jarvis. `wake_start` logs that and does not start detection.

## Camera and WakeNet

The GC0308 and WakeNet together exhaust internal DMA. The camera driver is not compiled, and `CameraGate::open` always fails, so the camera privacy dot cannot turn on.

## Servo range

The SCS0009 driver uses yaw ID 1 zero 460 and pitch ID 2 zero 620, UART1 TX GPIO6 RX GPIO7 at 1 Mbps. Motion limits default to yaw ±40° and pitch −10° to +25°, matching groki-bot. A mechanical tilt of about 5–85° is a physical limit of the head, not a second software clamp. Calibrate the zero on the real base before trusting the numbers.
