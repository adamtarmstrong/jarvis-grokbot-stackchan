// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>

namespace stackchan::groki_motion {

// LCD + IMU gesture events. demo_loop feeds in one frame of M5.Touch / M5.Imu samples
// and writes the resulting intent to SharedState; the gesture logic itself never touches hardware.
// The Intent → Expression mapping lives in the assembly layer (main/face_intent_map.hpp).

enum class Intent : std::uint8_t {
    None,
    Tap,
    DoubleTap,
    Hold,
    FlickLeft,
    FlickRight,
    FlickUp,
    FlickDown,
    Stroke,
    StrokeRestore,
    DizzyStart,
    DizzyEnd,
};

struct TouchSample {
    std::int16_t x = 0;
    std::int16_t y = 0;
    bool pressed = false;
    bool was_pressed = false;
    bool was_clicked = false;
    std::uint8_t click_count = 0;
    bool was_hold = false;
    bool is_moving = false;
    bool was_flicked = false;
    std::int16_t distance_x = 0;
    std::int16_t distance_y = 0;
    std::uint32_t now_ms = 0;
};

struct ImuSample {
    bool valid = false;
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    std::uint32_t now_ms = 0;
};

struct Policy {
    bool expressions_enabled = true;
    bool overlay_owns_panel = false;
};

struct FaceInputTick {
    Intent intent = Intent::None;
    std::int8_t preview_step = 0;
    bool gaze_active = false;
    float gaze_h = 0.0f;
    float gaze_v = 0.0f;
};

constexpr std::int16_t kAxisLockPx = 12;
constexpr std::int16_t kStrokeMinPathPx = 48;
constexpr std::uint8_t kStrokeMinReversals = 2;
constexpr std::uint32_t kStrokeMinDurationMs = 280;
constexpr float kStrokeMaxSpeedPxPerMs = 0.40f;
constexpr std::uint32_t kStrokeRestoreMs = 3000;
constexpr std::uint32_t kDoubleTapWaitMs = 400;
constexpr float kGazeGain = 5.0f;

constexpr std::uint8_t kShakeSwings = 4;
constexpr float kShakeJerk = 0.50f;
constexpr float kShakeAxisDelta = 0.32f;
constexpr std::uint32_t kShakeMinGapMs = 90;
constexpr std::uint32_t kShakeMaxGapMs = 480;
constexpr std::uint32_t kShakeSequenceTimeoutMs = 1500;
constexpr std::uint32_t kShakeSettleMs = 650;
constexpr float kTiltFullScaleG = 0.28f;

class FaceInput {
public:
    void set_screen_center(float cx, float cy) noexcept;

    FaceInputTick tick(const TouchSample& touch, const ImuSample& imu, const Policy& policy) noexcept;

private:
    void reset_gesture() noexcept;
    void reset_shake() noexcept;
    void apply_touch_gaze(const TouchSample& touch, FaceInputTick& out) const noexcept;
    void apply_tilt_gaze(FaceInputTick& out) const noexcept;
    Intent classify_flick(std::int16_t dx, std::int16_t dy) const noexcept;
    static void set_preview_step(Intent flick, FaceInputTick& out) noexcept;
    void track_stroke(const TouchSample& touch) noexcept;
    bool stroke_qualified(std::uint32_t now_ms) const noexcept;
    bool touch_is_moving(const TouchSample& touch) const noexcept;
    Intent feed_touch(const TouchSample& touch, FaceInputTick& out) noexcept;
    Intent feed_imu(const ImuSample& imu, std::uint32_t now_ms) noexcept;

    float cx_{233.0f};
    float cy_{233.0f};

    bool committed_{false};
    bool stroke_fired_{false};
    bool touch_gaze_this_tick_{false};

    std::int16_t stroke_last_x_{0};
    std::int8_t stroke_dir_{0};
    std::uint8_t stroke_reversals_{0};
    std::int32_t stroke_path_px_{0};
    std::uint32_t stroke_start_ms_{0};
    std::uint32_t stroke_restore_since_ms_{0};
    std::uint32_t pending_tap_since_ms_{0};

    bool imu_ready_{false};
    float neutral_x_{0.0f};
    float neutral_y_{0.0f};
    float filtered_x_{0.0f};
    float filtered_y_{0.0f};
    float prev_ax_{0.0f};
    float prev_ay_{0.0f};
    float prev_az_{0.0f};
    float shake_energy_{0.0f};
    bool dizzy_active_{false};
    std::uint8_t shake_swings_{0};
    std::int8_t shake_dir_{0};
    std::uint32_t shake_seq_start_ms_{0};
    std::uint32_t last_swing_ms_{0};
    std::uint32_t dizzy_quiet_since_ms_{0};
};

} // namespace stackchan::groki_motion
