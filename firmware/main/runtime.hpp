#pragma once

#include <atomic>
#include <cstdint>

#include "eyes/eye_model.h"

namespace jarvis {

enum class FaceState : std::uint8_t { Idle, Listening, Thinking, Speaking, Sleeping };

inline eyes::Mood mood_for(FaceState state)
{
    switch (state) {
    case FaceState::Listening:
        return eyes::Mood::Listening;
    case FaceState::Thinking:
        return eyes::Mood::Thinking;
    case FaceState::Speaking:
        return eyes::Mood::Speaking;
    case FaceState::Sleeping:
        return eyes::Mood::Sleepy;
    case FaceState::Idle:
        return eyes::Mood::Idle;
    }
    return eyes::Mood::Idle;
}

// Face and privacy tasks read these. Audio code writes them.
struct Runtime {
    std::atomic<FaceState> face{FaceState::Idle};
    std::atomic<int> lip_milli{0}; // 0..1000, used while speaking
    std::atomic<bool> listening{false};
    std::atomic<bool> speaking{false};
    std::atomic<bool> ptt{false};
};

Runtime& runtime();

void eye_start();
void privacy_start();
// Draws the privacy dots. Call only from the eye task, which owns M5.Display.
void privacy_paint_dots();
void head_start();
void head_submit(float yaw_deg, float pitch_deg);

// 16 kHz mono PCM. The mouth only moves while the speaking flag is set.
void lip_feed(const std::int16_t* pcm, std::size_t n);

} // namespace jarvis
