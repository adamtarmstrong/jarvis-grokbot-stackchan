#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Joseph Zhou
# SPDX-License-Identifier: BSL-1.0
"""Fail a firmware build whose static data leaves too little internal RAM.

The ESP32-S3 heap only gets the internal D/IRAM that .data, .bss and IRAM
code leave free. If a dependency update grows those sections, the firmware
still builds but runs out of internal RAM at boot. v0.2.0-v0.2.2 shipped
like that on CoreS3: about 18 KiB of D/IRAM was left and every boot aborted
in esp_wifi_init. This check stops such a build before it is released.

Usage:
    python3 tools/check_static_ram.py --board cores3 build-cores3/stackchan_idf.map

Run it inside an exported ESP-IDF environment (it calls `python -m esp_idf_size`).
Boards without a floor below only get the number printed.
"""

import argparse
import json
import subprocess
import sys

# Minimum static D/IRAM left for the heap, in bytes. cores3 had 127,473
# bytes (about 124.5 KiB) left when this floor was set (IDF 5.5.4, esp-sr
# 2.6.0) and boots normally; the broken releases had about 18 KiB. The floor
# leaves about 8 KiB of slack below that build.
# This check only guarantees that the firmware boots. Internal RAM use with
# Wi-Fi connected and a voice session running has not been measured yet;
# set the floor again from those measurements once they exist.
MIN_DIRAM_REMAIN = {
    "cores3": 116 * 1024,
}


def diram_remain(map_file: str) -> int:
    out = subprocess.run(
        [sys.executable, "-m", "esp_idf_size", "--format", "json", map_file],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout
    data = json.loads(out)
    if "diram_remain" not in data:
        raise SystemExit(f"esp_idf_size output has no 'diram_remain' field; keys: {sorted(data)}")
    return int(data["diram_remain"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--board", required=True)
    parser.add_argument("map_file")
    args = parser.parse_args()

    remain = diram_remain(args.map_file)
    floor = MIN_DIRAM_REMAIN.get(args.board)
    print(f"{args.board}: static D/IRAM left for the heap = {remain} bytes ({remain / 1024:.1f} KiB)")
    if floor is None:
        print(f"{args.board}: no floor set, not checked")
        return 0
    if remain < floor:
        print(
            f"::error::{args.board}: only {remain} bytes of D/IRAM left after static data "
            f"(floor {floor}). The firmware would run out of internal RAM at boot. "
            f"Compare `python -m esp_idf_size --archives` with the last good build to find what grew."
        )
        return 1
    print(f"{args.board}: OK (floor {floor} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
