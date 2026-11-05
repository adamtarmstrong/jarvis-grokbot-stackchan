#include <sys/time.h>
#include <ctime>
#include "runtime.hpp"

#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "jarvis/privacy.hpp"

namespace jarvis {

namespace {
Runtime g_runtime;
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;

uint16_t rgb565(Rgb c)
{
    return static_cast<uint16_t>(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
}

void draw_shape(int cx, int cy, const eyes::Shape& shape, uint16_t color, float lip)
{
    float h = shape.h;
    if (runtime().speaking.load()) h *= 0.72f + 0.45f * lip;
    const int x = cx + static_cast<int>(shape.dx);
    const int y = cy + static_cast<int>(shape.dy);
    if (shape.arc < 0.0f) {
        M5.Display.fillRoundRect(x - static_cast<int>(shape.w / 2), y, static_cast<int>(shape.w),
                                 static_cast<int>(h * 0.25f), 4, color);
        return;
    }
    M5.Display.fillEllipse(x, y, static_cast<int>(shape.w / 2), static_cast<int>(h / 2), color);
    if (shape.lid > 0.0f) {
        const int lid_h = static_cast<int>(h * shape.lid);
        M5.Display.fillRect(x - static_cast<int>(shape.w / 2) - 2, y - static_cast<int>(h / 2) - 2,
                            static_cast<int>(shape.w) + 4, lid_h, rgb565(kFaceBackground));
    }
}

void eye_task(void*)
{
    // Priority stays below the audio tasks. groki-bot found that raising the
    // display task made the speaker stutter. This task is created at priority 1.
    float t = 0.0f;
    FaceState previous = FaceState::Idle;
    while (true) {
        const FaceState state = g_runtime.face.load();
        const float lip = static_cast<float>(g_runtime.lip_milli.load()) / 1000.0f;
        if (state != previous) {
            M5.Display.fillScreen(rgb565(kFaceBackground));
            previous = state;
        } else {
            // Redraw the eye band only, not the privacy dots in the corner.
            M5.Display.fillRect(0, 40, kScreenW, 160, rgb565(kFaceBackground));
        }
        const eyes::Face face = eyes::FaceFor(mood_for(state), t);
        const uint16_t color = rgb565(kAccents[static_cast<int>(Accent::Brass)]);
        draw_shape(90, 120, face.left, color, lip);
        draw_shape(230, 120, face.right, color, lip);
        t += 0.05f;
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
} // namespace

Runtime& runtime()
{
    return g_runtime;
}

void eye_start()
{
    auto cfg = M5.config();
    cfg.clear_display = true;
    // M5.begin copies the board RTC (unset, year 2000) into the system clock and would
    // undo SNTP. Keep a synced clock, and write it back to the RTC for the next boot.
    timeval before = {};
    gettimeofday(&before, nullptr);
    M5.begin(cfg);
    if (before.tv_sec > 1767225600) { // after 2026-01-01: SNTP already ran
        settimeofday(&before, nullptr);
        if (M5.Rtc.isEnabled()) {
            const std::time_t now = before.tv_sec;
            std::tm utc = {};
            gmtime_r(&now, &utc);
            M5.Rtc.setDateTime(&utc);
        }
    }
    M5.Display.setRotation(1);
    M5.Display.fillScreen(rgb565(kFaceBackground));
    // UNTESTED on a panel. Priority 1 is deliberate; audio tasks use 5.
    xTaskCreatePinnedToCore(eye_task, "eyes", 4096, nullptr, 1, nullptr, 1);
}

} // namespace jarvis
