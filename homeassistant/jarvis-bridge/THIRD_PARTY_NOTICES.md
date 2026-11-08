# Jarvis Bridge licensing and model provenance

The bridge Python application and its tests are GPL-3.0-or-later, compatible with
Piper's GPL dependency. See LICENSE in this folder. This does not relicense the
separate firmware or Worker.

- Piper TTS (`piper-tts` 1.3.0): GPL-3.0-or-later, https://github.com/OHF-Voice/piper1-gpl
  (including its bundled eSpeak NG components and upstream notices).
- faster-whisper: MIT, https://github.com/SYSTRAN/faster-whisper
- CTranslate2: MIT, https://github.com/OpenNMT/CTranslate2
- Whisper models: MIT, https://github.com/openai/whisper
- NumPy: BSD-3-Clause; SciPy: BSD-3-Clause; aiohttp: Apache-2.0 and MIT.
  Installed packages retain their license metadata in the image.
- Official Alan voice is downloaded on first start, not included in git or the
  image. Source: https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_GB/alan/medium
  The upstream MODEL_CARD is saved beside the model. It identifies Alan Pope's
  voice, links the Mycroft apope dataset and says "License: See URL". That linked
  legacy LICENSE currently reads "Copyright 2022 Mycroft AI / All Rights Reserved".
  Do not call these weights MIT or claim unrestricted redistribution. Clarify
  upstream voice/dataset licensing before distributing bundled voice weights.
  This slice uses the requested official runtime-download path, with no weights
  in the repository. No voice is sold or charged for.
- Privately installed models keep their own rights and licenses. The add-on
  only reads files supplied by the operator in /share/piper. It neither retrieves
  nor distributes any fan-made voice.
