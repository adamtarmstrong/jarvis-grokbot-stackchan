// SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace stackchan::groki_motion {

struct Limits {
    float yaw_min_deg;
    float yaw_max_deg;
    float pitch_min_deg;
    float pitch_max_deg;
};

struct Pose {
    float yaw_deg;
    float pitch_deg;
    std::uint16_t speed;
};

struct SpringParams {
    float stiffness;
    float damping;
    float rest_speed_deg_s;
    float rest_delta_deg;
};

// Spring parameters tuned for the SCS0009 head: higher speeds give a stiffer,
// faster spring.
SpringParams spring_params_for_speed(std::uint16_t speed) noexcept;

class SpringAxis {
public:
    void reset(float value) noexcept;
    void retarget(float target, std::uint16_t speed) noexcept;
    bool step(float dt_s, float min_deg, float max_deg) noexcept;

    bool initialized() const noexcept { return initialized_; }
    bool moving() const noexcept;
    float current() const noexcept { return current_deg_; }
    float target() const noexcept { return target_deg_; }

private:
    bool initialized_{false};
    float target_deg_{0.0f};
    float current_deg_{0.0f};
    float velocity_deg_s_{0.0f};
    SpringParams params_{spring_params_for_speed(200)};
};

// Pose generators for being petted and for idle looking around. entropy
// seeds the random choice, so the same value gives the same pose.
Pose head_pet_pose(float base_yaw_deg, float base_pitch_deg, Limits limits,
                   std::uint32_t entropy) noexcept;
Pose idle_pose(float current_yaw_deg, float current_pitch_deg, Limits limits,
               std::uint32_t entropy) noexcept;

// Four rounds of left/right wobble after a stroke. speed_override is consumed
// once per target change, so the caller writes the speed again before every
// step.
constexpr std::uint16_t kNadenadeWobbleSpeed = 800;
constexpr float kNadenadeWobbleDeg = 8.0f;
constexpr int kNadenadeWobbleRounds = 4;
constexpr std::size_t kNadenadeWobbleStepCount =
    static_cast<std::size_t>(kNadenadeWobbleRounds) * 2;

struct ServoCommand {
    float yaw_deg;
    std::uint16_t speed;
};

std::array<ServoCommand, kNadenadeWobbleStepCount> nadenade_wobble_steps() noexcept;

} // namespace stackchan::groki_motion
