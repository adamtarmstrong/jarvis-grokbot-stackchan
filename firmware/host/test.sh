#!/bin/sh
# Host checks that do not need ESP-IDF or a board.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
g++ -std=c++20 \
  -I "$ROOT/components/jarvis_eye/include" \
  -I "$ROOT/components/jarvis_logic/include" \
  -I "$ROOT/components/groki_motion/include" \
  "$ROOT/host/test_logic.cpp" \
  "$ROOT/components/groki_motion/motion.cpp" \
  "$ROOT/components/groki_motion/face_input.cpp" \
  -o /tmp/jarvis-host-logic
/tmp/jarvis-host-logic
python3 "$ROOT/host/test_hmac.py"
