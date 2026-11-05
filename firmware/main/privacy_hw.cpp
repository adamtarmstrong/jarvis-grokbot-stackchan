#include "runtime.hpp"
#include "gates.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <M5Unified.h>

#include <atomic>
#include <esp_log.h>

#include "board/io_expander_py32.hpp"

namespace jarvis {

namespace {
MicGate* g_mic = nullptr;
CameraGate* g_camera = nullptr;
// Latest dots, written by the privacy task and painted by the eye task.
std::atomic<uint8_t> g_dot_bits{0};
std::atomic<bool> g_servo_power{false};
std::atomic<bool> g_servo_power_checked{false};

MicGate& mic_slot()
{
    static MicGate gate;
    return gate;
}
CameraGate& camera_slot()
{
    static CameraGate gate;
    return gate;
}

void paint_dots(const PrivacyDots& dots)
{
    const uint16_t bg = M5.Display.color565(kFaceBackground.r, kFaceBackground.g, kFaceBackground.b);
    M5.Display.fillRect(299, 3, 20, 11, bg);
    const uint16_t ring = M5.Display.color565(kDotRing.r, kDotRing.g, kDotRing.b);
    if (dots.mic) {
        M5.Display.fillCircle(304, 8, 4, ring);
        M5.Display.fillCircle(304, 8, 3, M5.Display.color565(kMicDotScreen.r, kMicDotScreen.g, kMicDotScreen.b));
    }
    if (dots.camera) {
        M5.Display.fillCircle(313, 8, 4, ring);
        M5.Display.fillCircle(313, 8, 3, M5.Display.color565(kCameraDot.r, kCameraDot.g, kCameraDot.b));
    }
}

void mirror_leds(stackchan::board::Py32Expander* expander, const PrivacyDots& dots)
{
    if (expander == nullptr) return;
    std::uint8_t raw[12 * 2] = {};
    auto put = [&](int index, Rgb color) {
        const auto packed = stackchan::board::Py32Expander::rgb565(color.r, color.g, color.b);
        raw[index * 2] = static_cast<std::uint8_t>(packed & 0xFF);
        raw[index * 2 + 1] = static_cast<std::uint8_t>(packed >> 8);
    };
    if (dots.mic) put(0, kMicDot);
    if (dots.camera) put(1, kCameraDot);
    if (!expander->write_led_colors(raw, 12)) return;
    (void)expander->refresh_leds();
}

void privacy_task(void*)
{
    stackchan::board::Py32Expander* leds = nullptr;
    if (auto probed = stackchan::board::Py32Expander::probe()) leds = new stackchan::board::Py32Expander(std::move(*probed));
    if (leds != nullptr) {
        (void)leds->set_led_count(12);
        // Servo power on the Stack-chan base is gated by PY32 pin 0. Nothing turned it on before.
        const auto pin = stackchan::board::Py32Expander::kPinServoPowerEnable;
        const bool dir_ok = leds->set_direction(pin, true).has_value();
        const bool on_ok = leds->digital_write(pin, true).has_value();
        ESP_LOGI("privacy", "servo power enable: direction %s, write %s", dir_ok ? "ok" : "failed", on_ok ? "ok" : "failed");
        g_servo_power.store(dir_ok && on_ok);
    } else {
        ESP_LOGE("privacy", "PY32 expander probe failed; servo power stays off");
    }
    g_servo_power_checked.store(true);
    while (true) {
        const PrivacyDots dots = dots_from_drivers(driver_flags(*g_mic, *g_camera));
        g_dot_bits.store(static_cast<uint8_t>((dots.mic ? 1 : 0) | (dots.camera ? 2 : 0)));
        mirror_leds(leds, dots);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
} // namespace

bool servo_power_wait(int ms)
{
    for (int i = 0; i < ms / 50 && !g_servo_power_checked.load(); ++i) vTaskDelay(pdMS_TO_TICKS(50));
    return g_servo_power.load();
}

void privacy_paint_dots()
{
    // Repaint only when the dots change. Clearing them every frame made them flicker.
    static int painted = -1;
    const uint8_t bits = g_dot_bits.load();
    if (painted == bits) return;
    painted = bits;
    PrivacyDots dots{};
    dots.mic = (bits & 1) != 0;
    dots.camera = (bits & 2) != 0;
    paint_dots(dots);
}

MicGate& mic_gate()
{
    return mic_slot();
}
CameraGate& camera_gate()
{
    return camera_slot();
}

void privacy_start()
{
    g_mic = &mic_gate();
    g_camera = &camera_gate();
    // This task only drives the LEDs. The eye task paints the dots so one task owns the display.
    xTaskCreatePinnedToCore(privacy_task, "privacy", 4096, nullptr, 1, nullptr, 1);
}

} // namespace jarvis
