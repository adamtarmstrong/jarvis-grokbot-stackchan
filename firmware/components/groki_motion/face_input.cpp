// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0

#include "groki_motion/face_input.hpp"

#include <cmath>

namespace stackchan::groki_motion {

namespace {

float clampf(float v, float lo, float hi) noexcept {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

std::int16_t abs16(std::int16_t v) noexcept {
    return v < 0 ? static_cast<std::int16_t>(-v) : v;
}

} // namespace

void FaceInput::set_screen_center(float cx, float cy) noexcept {
    cx_ = cx;
    cy_ = cy;
}

void FaceInput::reset_gesture() noexcept {
    committed_ = false;
    stroke_fired_ = false;
    stroke_last_x_ = 0;
    stroke_dir_ = 0;
    stroke_reversals_ = 0;
    stroke_path_px_ = 0;
    stroke_start_ms_ = 0;
}

void FaceInput::reset_shake() noexcept {
    shake_swings_ = 0;
    shake_dir_ = 0;
    shake_seq_start_ms_ = 0;
    last_swing_ms_ = 0;
}

void FaceInput::apply_touch_gaze(const TouchSample& touch, FaceInputTick& out) const noexcept {
    const float dx = static_cast<float>(touch.x) - cx_;
    const float dy = static_cast<float>(touch.y) - cy_;
    const float r = std::sqrt(dx * dx + dy * dy);
    if (r < 1.0f) {
        out.gaze_active = false;
        out.gaze_h = 0.0f;
        out.gaze_v = 0.0f;
        return;
    }
    const float inv = 1.0f / r;
    out.gaze_active = true;
    out.gaze_h = dx * inv * kGazeGain;
    out.gaze_v = dy * inv * kGazeGain;
}

void FaceInput::apply_tilt_gaze(FaceInputTick& out) const noexcept {
    if (!imu_ready_) {
        return;
    }
    const float tx = clampf(-(filtered_x_ - neutral_x_) / kTiltFullScaleG, -1.0f, 1.0f);
    const float ty = clampf(-(filtered_y_ - neutral_y_) / kTiltFullScaleG, -1.0f, 1.0f);
    if (std::fabs(tx) < 0.05f && std::fabs(ty) < 0.05f) {
        return;
    }
    out.gaze_active = true;
    out.gaze_h = tx * kGazeGain;
    out.gaze_v = ty * kGazeGain;
}

Intent FaceInput::classify_flick(std::int16_t dx, std::int16_t dy) const noexcept {
    const auto adx = abs16(dx);
    const auto ady = abs16(dy);
    if (adx < kAxisLockPx && ady < kAxisLockPx) {
        return Intent::None;
    }
    if (adx >= ady) {
        return dx < 0 ? Intent::FlickLeft : Intent::FlickRight;
    }
    return dy < 0 ? Intent::FlickUp : Intent::FlickDown;
}

void FaceInput::set_preview_step(Intent flick, FaceInputTick& out) noexcept {
    if (flick == Intent::FlickLeft) {
        out.preview_step = 1;
    } else if (flick == Intent::FlickRight) {
        out.preview_step = -1;
    }
}

bool FaceInput::touch_is_moving(const TouchSample& touch) const noexcept {
    return touch.is_moving || abs16(touch.distance_x) >= kAxisLockPx || abs16(touch.distance_y) >= kAxisLockPx;
}

void FaceInput::track_stroke(const TouchSample& touch) noexcept {
    if (stroke_start_ms_ == 0) {
        stroke_start_ms_ = touch.now_ms;
        stroke_last_x_ = touch.x;
        return;
    }
    const std::int16_t step = static_cast<std::int16_t>(touch.x - stroke_last_x_);
    const auto astep = abs16(step);
    if (astep < 6) {
        return;
    }
    const std::int8_t dir = step > 0 ? 1 : -1;
    if (stroke_dir_ != 0 && dir != stroke_dir_) {
        ++stroke_reversals_;
    }
    stroke_dir_ = dir;
    stroke_path_px_ += astep;
    stroke_last_x_ = touch.x;
}

bool FaceInput::stroke_qualified(std::uint32_t now_ms) const noexcept {
    if (stroke_reversals_ < kStrokeMinReversals) {
        return false;
    }
    if (stroke_path_px_ < kStrokeMinPathPx) {
        return false;
    }
    if (stroke_start_ms_ == 0) {
        return false;
    }
    const std::uint32_t dur = now_ms - stroke_start_ms_;
    if (dur < kStrokeMinDurationMs) {
        return false;
    }
    const float speed = static_cast<float>(stroke_path_px_) / static_cast<float>(dur);
    return speed <= kStrokeMaxSpeedPxPerMs;
}

Intent FaceInput::feed_touch(const TouchSample& touch, FaceInputTick& out) noexcept {
    if (touch.was_pressed) {
        reset_gesture();
        stroke_start_ms_ = touch.now_ms;
        stroke_last_x_ = touch.x;
        stroke_restore_since_ms_ = 0;
    }

    if (touch.pressed) {
        if (pending_tap_since_ms_ != 0 && touch_is_moving(touch)) {
            pending_tap_since_ms_ = 0;
        }
        track_stroke(touch);

        if (!stroke_fired_ && stroke_qualified(touch.now_ms)) {
            stroke_fired_ = true;
            committed_ = true;
            pending_tap_since_ms_ = 0;
            stroke_restore_since_ms_ = 0;
            apply_touch_gaze(touch, out);
            touch_gaze_this_tick_ = true;
            return Intent::Stroke;
        }

        if (!committed_ && touch.was_hold && !touch_is_moving(touch) && stroke_reversals_ == 0) {
            committed_ = true;
            pending_tap_since_ms_ = 0;
            return Intent::Hold;
        }

        if (stroke_fired_ || touch_is_moving(touch)) {
            apply_touch_gaze(touch, out);
            touch_gaze_this_tick_ = true;
        }
        return Intent::None;
    }

    Intent released = Intent::None;
    if (!committed_ && touch.was_flicked) {
        committed_ = true;
        pending_tap_since_ms_ = 0;
        released = classify_flick(touch.distance_x, touch.distance_y);
        set_preview_step(released, out);
    } else if (!committed_ && touch.was_clicked) {
        committed_ = true;
        if (touch.click_count >= 2) {
            pending_tap_since_ms_ = 0;
            released = Intent::DoubleTap;
        } else {
            pending_tap_since_ms_ = touch.now_ms != 0 ? touch.now_ms : 1;
        }
    }

    if (stroke_fired_) {
        stroke_restore_since_ms_ = touch.now_ms != 0 ? touch.now_ms : 1;
    }
    reset_gesture();
    return released;
}

Intent FaceInput::feed_imu(const ImuSample& imu, std::uint32_t now_ms) noexcept {
    if (!imu.valid) {
        return Intent::None;
    }

    constexpr float kFilter = 0.32f;
    if (!imu_ready_) {
        filtered_x_ = imu.ax;
        filtered_y_ = imu.ay;
        neutral_x_ = imu.ax;
        neutral_y_ = imu.ay;
        prev_ax_ = imu.ax;
        prev_ay_ = imu.ay;
        prev_az_ = imu.az;
        imu_ready_ = true;
        return Intent::None;
    }

    filtered_x_ += (imu.ax - filtered_x_) * kFilter;
    filtered_y_ += (imu.ay - filtered_y_) * kFilter;
    // Slowly fold the holding angle into the neutral point, so holding the device tilted does not pin the eyes.
    neutral_x_ += (filtered_x_ - neutral_x_) * 0.0004f;
    neutral_y_ += (filtered_y_ - neutral_y_) * 0.0004f;

    const float dx = imu.ax - prev_ax_;
    const float dy = imu.ay - prev_ay_;
    const float dz = imu.az - prev_az_;
    const float jerk = std::fabs(dx) + std::fabs(dy) + std::fabs(dz);
    prev_ax_ = imu.ax;
    prev_ay_ = imu.ay;
    prev_az_ = imu.az;
    shake_energy_ = shake_energy_ * 0.72f + jerk * 0.28f;

    if (dizzy_active_) {
        if (shake_energy_ > 0.10f) {
            dizzy_quiet_since_ms_ = 0;
            return Intent::None;
        }
        if (dizzy_quiet_since_ms_ == 0) {
            dizzy_quiet_since_ms_ = now_ms;
            return Intent::None;
        }
        if (now_ms - dizzy_quiet_since_ms_ >= kShakeSettleMs) {
            dizzy_active_ = false;
            dizzy_quiet_since_ms_ = 0;
            reset_shake();
            return Intent::DizzyEnd;
        }
        return Intent::None;
    }

    if (shake_swings_ > 0 &&
        (now_ms - shake_seq_start_ms_ > kShakeSequenceTimeoutMs || now_ms - last_swing_ms_ > kShakeMaxGapMs)) {
        reset_shake();
    }

    const std::int8_t dir = dx >= 0.0f ? 1 : -1;
    const bool strong = jerk >= kShakeJerk && std::fabs(dx) >= kShakeAxisDelta;
    if (shake_swings_ == 0) {
        if (strong) {
            shake_swings_ = 1;
            shake_dir_ = dir;
            shake_seq_start_ms_ = now_ms;
            last_swing_ms_ = now_ms;
        }
    } else if (strong && dir != shake_dir_) {
        const std::uint32_t gap = now_ms - last_swing_ms_;
        if (gap >= kShakeMinGapMs && gap <= kShakeMaxGapMs) {
            ++shake_swings_;
            shake_dir_ = dir;
            last_swing_ms_ = now_ms;
        }
    }

    if (shake_swings_ >= kShakeSwings) {
        dizzy_active_ = true;
        dizzy_quiet_since_ms_ = 0;
        reset_shake();
        return Intent::DizzyStart;
    }
    return Intent::None;
}

FaceInputTick FaceInput::tick(const TouchSample& touch, const ImuSample& imu, const Policy& policy) noexcept {
    FaceInputTick out{};
    touch_gaze_this_tick_ = false;
    const std::uint32_t now_ms = touch.now_ms != 0 ? touch.now_ms : imu.now_ms;

    if (!policy.expressions_enabled || policy.overlay_owns_panel) {
        reset_gesture();
        stroke_restore_since_ms_ = 0;
        pending_tap_since_ms_ = 0;
        if (dizzy_active_) {
            dizzy_active_ = false;
            dizzy_quiet_since_ms_ = 0;
            reset_shake();
        }
        return out;
    }

    out.intent = feed_touch(touch, out);

    if (out.intent == Intent::None && pending_tap_since_ms_ != 0 &&
        now_ms - pending_tap_since_ms_ >= kDoubleTapWaitMs) {
        pending_tap_since_ms_ = 0;
        out.intent = Intent::Tap;
    }

    if (out.intent == Intent::None && stroke_restore_since_ms_ != 0 &&
        now_ms - stroke_restore_since_ms_ >= kStrokeRestoreMs) {
        stroke_restore_since_ms_ = 0;
        out.intent = Intent::StrokeRestore;
    }

    const Intent imu_intent = feed_imu(imu, now_ms);
    if (out.intent == Intent::None && imu_intent != Intent::None) {
        out.intent = imu_intent;
        if (imu_intent == Intent::DizzyStart) {
            stroke_restore_since_ms_ = 0;
        }
    }

    if (!touch_gaze_this_tick_) {
        apply_tilt_gaze(out);
    }
    return out;
}

} // namespace stackchan::groki_motion
