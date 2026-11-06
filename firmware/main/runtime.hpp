#pragma once

#include <atomic>
#include <cstdint>

#include "eyes/eye_model.h"

namespace jarvis {

enum class FaceState : std::uint8_t { Idle, Listening, Thinking, Speaking, Sleeping, Connecting };

inline eyes::Mood mood_for(FaceState state)
{
    switch (state) {
    case FaceState::Listening:
        return eyes::Mood::Listening;
    case FaceState::Thinking:
        return eyes::Mood::Thinking;
    case FaceState::Speaking:
        return eyes::Mood::Speaking;
    case FaceState::Connecting:
        return eyes::Mood::Curious;  // waiting for the realtime session; do not speak yet
    case FaceState::Sleeping:
        return eyes::Mood::Sleepy;
    case FaceState::Idle:
        return eyes::Mood::Idle;
    }
    return eyes::Mood::Idle;
}

// Hard cap on an xAI realtime session. The voice task closes at this mark.
constexpr std::int64_t kMaxSessionUs = 60'000'000;

// Face and privacy tasks read these. Audio code writes them.
struct Runtime {
    std::atomic<FaceState> face{FaceState::Idle};
    std::atomic<int> lip_milli{0}; // 0..1000, used while speaking
    std::atomic<bool> listening{false};
    std::atomic<bool> speaking{false};
    std::atomic<bool> ptt{false};
    // 0 = no open session; otherwise esp_timer deadline for the 60 s cap.
    std::atomic<std::int64_t> session_deadline_us{0};
};

Runtime& runtime();

void eye_start();
void privacy_start();
// Draws the privacy dots. Call only from the eye task, which owns M5.Display.
void privacy_paint_dots();
void head_start();
// Waits up to ms for the PY32 servo power enable; true when servo power is on.
bool servo_power_wait(int ms);
void head_submit(float yaw_deg, float pitch_deg);

// 16 kHz mono PCM. The mouth only moves while the speaking flag is set.
void lip_feed(const std::int16_t* pcm, std::size_t n);

} // namespace jarvis
