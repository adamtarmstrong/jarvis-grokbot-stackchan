#include "runtime.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_random.h>

#include "groki_motion/motion.hpp"
#include "scs_servo/scs_bus.hpp"
#include "scs_servo/scs_servo.hpp"
#include "servo_limits.hpp"

namespace jarvis {

namespace {
using stackchan::groki_motion::Limits;
using stackchan::groki_motion::SpringAxis;
using stackchan::scs_servo::ScsBus;
using stackchan::scs_servo::ScsServo;

struct Pending {
    bool set = false;
    float yaw = 0;
    float pitch = 0;
};

Pending g_pending;

void head_task(void*)
{
    // UART1 TX 6 / RX 7, 1 Mbps. Yaw ID 1 zero 460, pitch ID 2 zero 620.
    // UNTESTED: no servo was attached in this environment.
    ScsBus::Config cfg;
    cfg.uart = UART_NUM_1;
    cfg.tx = GPIO_NUM_6;
    cfg.rx = GPIO_NUM_7;
    cfg.baud = 1'000'000;
    const bool powered = servo_power_wait(3000);
    ESP_LOGI("head", "servo power %s", powered ? "on" : "NOT on");
    vTaskDelay(pdMS_TO_TICKS(200));  // let the servo rail settle
    auto bus = ScsBus::create(cfg);
    if (!bus) {
        ESP_LOGE("head", "servo bus did not open");
        vTaskDelete(nullptr);
        return;
    }
    ScsServo yaw(*bus, stackchan::scs_servo::kYawId);
    ScsServo pitch(*bus, stackchan::scs_servo::kPitchId);
    const bool yaw_ok = yaw.ping().has_value();
    const bool pitch_ok = pitch.ping().has_value();
    ESP_LOGI("head", "servo bus open; ping yaw %s, pitch %s", yaw_ok ? "ok" : "no reply", pitch_ok ? "ok" : "no reply");
    (void)yaw.enable_torque(true);
    (void)pitch.enable_torque(true);

    const stackchan::app::ServoLimits limits;
    const Limits motion_limits{
        static_cast<float>(limits.yaw_min_deg),
        static_cast<float>(limits.yaw_max_deg),
        static_cast<float>(limits.pitch_min_deg),
        static_cast<float>(limits.pitch_max_deg),
    };
    SpringAxis yaw_axis;
    SpringAxis pitch_axis;
    yaw_axis.reset(0);
    pitch_axis.reset(0);
    bool torque = true;

    // Idle look-around: a small random glance every 4-8 s while idle.
    TickType_t next_glance = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    uint32_t glances = 0;
    while (true) {
        if (xTaskGetTickCount() >= next_glance) {
            next_glance = xTaskGetTickCount() + pdMS_TO_TICKS(4000 + esp_random() % 4000);
            if (!runtime().listening.load() && runtime().face.load() == FaceState::Idle) {
                const float y = static_cast<float>(static_cast<int>(esp_random() % 41) - 20);
                const float p = static_cast<float>(static_cast<int>(esp_random() % 13) - 6);
                head_submit(y, p);
                if ((glances++ % 5) == 0) ESP_LOGI("head", "idle glance #%u to yaw %.0f pitch %.0f", (unsigned)glances, y, p);
            }
        }
        // Listening pauses tracking. Speaking keeps only the latest command.
        if (!runtime().listening.load() && g_pending.set) {
            yaw_axis.retarget(g_pending.yaw, 200);
            pitch_axis.retarget(g_pending.pitch, 200);
            g_pending.set = false;
        }
        const bool moving = yaw_axis.step(0.02f, motion_limits.yaw_min_deg, motion_limits.yaw_max_deg) ||
                            pitch_axis.step(0.02f, motion_limits.pitch_min_deg, motion_limits.pitch_max_deg);
        if (!moving && torque) {
            (void)yaw.enable_torque(false);
            (void)pitch.enable_torque(false);
            torque = false;
        } else if (moving && !torque) {
            (void)yaw.enable_torque(true);
            (void)pitch.enable_torque(true);
            torque = true;
        }
        if (moving) {
            (void)yaw.write_goal_position(stackchan::scs_servo::deg_to_raw(yaw_axis.current(), limits.yaw_zero), 0, 200);
            (void)pitch.write_goal_position(
                stackchan::scs_servo::deg_to_raw(pitch_axis.current(), limits.pitch_zero), 0, 200);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
} // namespace

void head_submit(float yaw_deg, float pitch_deg)
{
    // One slot. A newer command replaces an older one that has not been applied.
    g_pending.yaw = yaw_deg;
    g_pending.pitch = pitch_deg;
    g_pending.set = true;
}

void head_start()
{
    xTaskCreatePinnedToCore(head_task, "head", 4096, nullptr, 3, nullptr, 1);
}

} // namespace jarvis
