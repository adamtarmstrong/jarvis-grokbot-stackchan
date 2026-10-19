// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0

#include "groki_motion/motion.hpp"

#include <cmath>

namespace stackchan::groki_motion {

namespace {

float clamp_float(float value, float low, float high) noexcept
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

std::uint16_t clamp_speed(std::uint16_t speed) noexcept
{
    if (speed < 100) return 100;
    if (speed > 1000) return 1000;
    return speed;
}

std::uint32_t next_rand(std::uint32_t& x) noexcept
{
    // A small deterministic mixer expands one esp_random() value into several stable choices
    // without adding a runtime dependency here.
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

int rand_int(std::uint32_t& seed, int low, int high) noexcept
{
    if (high <= low) return low;
    const auto span = static_cast<std::uint32_t>(high - low + 1);
    return low + static_cast<int>(next_rand(seed) % span);
}

float rand_float(std::uint32_t& seed, float low, float high) noexcept
{
    const float unit = static_cast<float>(next_rand(seed)) / static_cast<float>(UINT32_MAX);
    return low + (high - low) * unit;
}

} // namespace

SpringParams spring_params_for_speed(std::uint16_t speed) noexcept
{
    speed = clamp_speed(speed);

    // Speed maps to spring parameters with the quadratic curve of M5Stack's motion layer: at speed=500
    // the stiffness is about 170, with critical damping, so target changes carry inertia without visible overshoot.
    constexpr float kMinStiffness = 10.0f;
    constexpr float kMaxStiffness = 650.0f;
    constexpr float kMass = 1.0f;

    const float normalized = static_cast<float>(speed) / 1000.0f;
    const float stiffness = kMinStiffness + normalized * normalized * (kMaxStiffness - kMinStiffness);
    const float damping = 2.0f * std::sqrt(kMass * stiffness);

    return SpringParams{
        .stiffness = stiffness,
        .damping = damping,
        .rest_speed_deg_s = speed > 800 ? 0.5f : 0.1f,
        .rest_delta_deg = speed > 800 ? 0.5f : 0.1f,
    };
}

void SpringAxis::reset(float value) noexcept
{
    initialized_ = true;
    target_deg_ = value;
    current_deg_ = value;
    velocity_deg_s_ = 0.0f;
}

void SpringAxis::retarget(float target, std::uint16_t speed) noexcept
{
    if (!initialized_) {
        reset(target);
    }
    params_ = spring_params_for_speed(speed);
    target_deg_ = target;
}

bool SpringAxis::moving() const noexcept
{
    return std::fabs(velocity_deg_s_) > params_.rest_speed_deg_s ||
           std::fabs(target_deg_ - current_deg_) > params_.rest_delta_deg;
}

bool SpringAxis::step(float dt_s, float min_deg, float max_deg) noexcept
{
    if (!initialized_) {
        reset(clamp_float(target_deg_, min_deg, max_deg));
        return false;
    }

    target_deg_ = clamp_float(target_deg_, min_deg, max_deg);
    current_deg_ = clamp_float(current_deg_, min_deg, max_deg);
    if (!moving()) {
        current_deg_ = target_deg_;
        velocity_deg_s_ = 0.0f;
        return false;
    }

    if (dt_s <= 0.0f) dt_s = 0.02f;
    if (dt_s > 0.05f) dt_s = 0.05f;

    const float acceleration = params_.stiffness * (target_deg_ - current_deg_) -
                               params_.damping * velocity_deg_s_;
    velocity_deg_s_ += acceleration * dt_s;
    current_deg_ += velocity_deg_s_ * dt_s;

    const float clamped = clamp_float(current_deg_, min_deg, max_deg);
    if (clamped != current_deg_) {
        current_deg_ = clamped;
        if ((current_deg_ <= min_deg && velocity_deg_s_ < 0.0f) ||
            (current_deg_ >= max_deg && velocity_deg_s_ > 0.0f)) {
            velocity_deg_s_ = 0.0f;
        }
    }

    if (!moving()) {
        current_deg_ = target_deg_;
        velocity_deg_s_ = 0.0f;
    }
    return true;
}

Pose head_pet_pose(float base_yaw_deg, float base_pitch_deg, Limits limits,
                   std::uint32_t entropy) noexcept
{
    auto seed = entropy == 0 ? 0x9E3779B9u : entropy;
    const int action = rand_int(seed, 0, 2);
    const auto speed = static_cast<std::uint16_t>(rand_int(seed, 300, 500));

    float yaw = base_yaw_deg;
    float pitch = base_pitch_deg;
    switch (action) {
    case 0: // look up slightly, with a little sideways offset
        pitch += static_cast<float>(rand_int(seed, 15, 25));
        yaw += static_cast<float>(rand_int(seed, -5, 5));
        break;
    case 1: // slight head tilt
        pitch -= static_cast<float>(rand_int(seed, 0, 5));
        yaw += rand_int(seed, 0, 1) == 0 ? -15.0f : 15.0f;
        break;
    default: // bigger happy look-up, still within the base's limits
        pitch += static_cast<float>(rand_int(seed, 25, 40));
        break;
    }

    return Pose{
        .yaw_deg = clamp_float(yaw, limits.yaw_min_deg, limits.yaw_max_deg),
        .pitch_deg = clamp_float(pitch, limits.pitch_min_deg, limits.pitch_max_deg),
        .speed = speed,
    };
}

Pose idle_pose(float current_yaw_deg, float current_pitch_deg, Limits limits,
               std::uint32_t entropy) noexcept
{
    auto seed = entropy == 0 ? 0x85EBCA6Bu : entropy;
    const int action = rand_int(seed, 0, 99);

    float yaw = current_yaw_deg;
    float pitch = current_pitch_deg;
    std::uint16_t speed = 180;

    if (action < 50) {
        // Look around: stay in the middle part of the configured range.
        yaw = rand_float(seed,
                         limits.yaw_min_deg * 0.4f,
                         limits.yaw_max_deg * 0.4f);
        pitch = rand_float(seed,
                           limits.pitch_min_deg * 0.4f,
                           limits.pitch_max_deg * 0.4f);
        speed = static_cast<std::uint16_t>(rand_int(seed, 150, 300));
    } else if (action < 80) {
        // Small glance around the current pose.
        yaw += static_cast<float>(rand_int(seed, -12, 12));
        pitch += static_cast<float>(rand_int(seed, -6, 6));
        speed = static_cast<std::uint16_t>(rand_int(seed, 120, 250));
    } else if (action < 90) {
        // Quick glance, still smaller than the full mechanical range.
        yaw = rand_float(seed,
                         limits.yaw_min_deg * 0.55f,
                         limits.yaw_max_deg * 0.55f);
        pitch = rand_float(seed,
                           limits.pitch_min_deg * 0.5f,
                           limits.pitch_max_deg * 0.5f);
        speed = static_cast<std::uint16_t>(rand_int(seed, 250, 400));
    } else {
        // Back to the centre left/right, keeping a small pitch change.
        yaw = 0.0f;
        pitch = rand_float(seed,
                           limits.pitch_min_deg * 0.4f,
                           limits.pitch_max_deg * 0.4f);
        speed = static_cast<std::uint16_t>(rand_int(seed, 120, 300));
    }

    return Pose{
        .yaw_deg = clamp_float(yaw, limits.yaw_min_deg, limits.yaw_max_deg),
        .pitch_deg = clamp_float(pitch, limits.pitch_min_deg, limits.pitch_max_deg),
        .speed = speed,
    };
}

std::array<ServoCommand, kNadenadeWobbleStepCount> nadenade_wobble_steps() noexcept
{
    std::array<ServoCommand, kNadenadeWobbleStepCount> steps{};
    for (int i = 0; i < kNadenadeWobbleRounds; ++i) {
        const std::size_t base = static_cast<std::size_t>(i) * 2;
        steps[base] = ServoCommand{-kNadenadeWobbleDeg, kNadenadeWobbleSpeed};
        steps[base + 1] = ServoCommand{kNadenadeWobbleDeg, kNadenadeWobbleSpeed};
    }
    return steps;
}

} // namespace stackchan::groki_motion
