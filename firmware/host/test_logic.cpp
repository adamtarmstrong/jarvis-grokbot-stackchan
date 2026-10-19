// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jarvis contributors
// Host checks for the portable eye, motion, and privacy code. No ESP-IDF.

#include "eyes/eye_model.h"
#include "groki_motion/motion.hpp"
#include "jarvis/privacy.hpp"
#include "jarvis/sign.hpp"

#include <cmath>
#include <iostream>
#include <string>

int main()
{
    const std::string hash = "cbbbdcd27692344de5dbab3abcaba413fb0f45307267de7081401576df1cb176";
    const std::string message =
        jarvis::canonical_string("post", "/testprefix/ask", "device-1", "1760000000", "nonce-1", hash);
    const std::string expected = "JARVIS1\nPOST\n/testprefix/ask\ndevice-1\n1760000000\nnonce-1\n" + hash;
    if (message != expected) {
        std::cerr << "canonical string drifted\n";
        return 1;
    }

    const jarvis::PrivacyDots mic_only = jarvis::dots_from_drivers({true, false});
    const jarvis::PrivacyDots camera = jarvis::dots_from_drivers({false, true});
    const jarvis::PrivacyDots neither = jarvis::dots_from_drivers({});
    if (!mic_only.mic || mic_only.camera || camera.mic || !camera.camera || neither.mic || neither.camera) {
        std::cerr << "privacy dots do not follow the driver flags\n";
        return 2;
    }
    if (jarvis::kAccents[0].r != 224 || jarvis::kAccents[0].g != 161 || jarvis::kAccents[0].b != 6) {
        std::cerr << "Jarvis accent is not brass\n";
        return 3;
    }

    stackchan::groki_motion::SpringAxis axis;
    axis.reset(0);
    axis.retarget(80, 400);
    for (int i = 0; i < 500; ++i) axis.step(0.02f, -40.0f, 40.0f);
    if (axis.current() > 40.01f || axis.current() < -40.01f) {
        std::cerr << "spring left the yaw limit\n";
        return 4;
    }

    for (int mood = 0; mood < static_cast<int>(eyes::Mood::Count); ++mood) {
        const eyes::Face face = eyes::FaceFor(static_cast<eyes::Mood>(mood), 1.5f);
        if (!std::isfinite(face.left.w) || !std::isfinite(face.left.h) || !std::isfinite(face.right.w) ||
            !std::isfinite(face.right.h)) {
            std::cerr << "eye model produced a non-finite shape\n";
            return 5;
        }
    }
    std::cout << "host logic ok\n";
    return 0;
}
