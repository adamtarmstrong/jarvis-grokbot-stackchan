#include <sys/time.h>
#include <ctime>
#include "runtime.hpp"

#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>

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

void draw_shape(lgfx::LovyanGFX& gfx, int cx, int cy, const eyes::Shape& shape, uint16_t color, float lip)
{
    float h = shape.h;
    if (runtime().speaking.load()) h *= 0.72f + 0.45f * lip;
    const int x = cx + static_cast<int>(shape.dx);
    const int y = cy + static_cast<int>(shape.dy);
    if (shape.arc < 0.0f) {
        gfx.fillRoundRect(x - static_cast<int>(shape.w / 2), y, static_cast<int>(shape.w),
                                 static_cast<int>(h * 0.25f), 4, color);
        return;
    }
    // Tall oval eyes like Grok Bot's. Moods change w, h, offsets and lids.
    gfx.fillEllipse(x, y, static_cast<int>(shape.w / 2), static_cast<int>(h / 2), color);
    if (shape.lid > 0.0f) {
        const int lid_h = static_cast<int>(h * shape.lid);
        gfx.fillRect(x - static_cast<int>(shape.w / 2) - 2, y - static_cast<int>(h / 2) - 2,
                            static_cast<int>(shape.w) + 4, lid_h, rgb565(kFaceBackground));
    }
}

void eye_task(void*)
{
    // Priority stays below the audio tasks. groki-bot found that raising the
    // display task made the speaker stutter. This task is created at priority 1.
    // The eye band is drawn off screen in PSRAM and pushed in one transfer, so the
    // panel never shows a cleared frame. Clearing the panel directly made the eyes flicker.
    constexpr int kBandY = 40;
    constexpr int kBandH = 160;
    static M5Canvas band(&M5.Display);
    band.setPsram(true);
    band.setColorDepth(16);
    const bool have_band = band.createSprite(kScreenW, kBandH) != nullptr;
    if (!have_band) ESP_LOGW("eyes", "no PSRAM for the eye sprite; drawing directly");
    const uint16_t bg = rgb565(kFaceBackground);
    const uint16_t color = rgb565(kEyeColor);
    float t = 0.0f;
    while (true) {
        const FaceState state = g_runtime.face.load();
        const float lip = static_cast<float>(g_runtime.lip_milli.load()) / 1000.0f;
        const eyes::Face face = eyes::FaceFor(mood_for(state), t);
        if (have_band) {
            band.fillSprite(bg);
            draw_shape(band, 90, 120 - kBandY, face.left, color, lip);
            draw_shape(band, 230, 120 - kBandY, face.right, color, lip);
        }
        // One locked frame. This is the only task that draws, so SPI writes never interleave.
        M5.Display.startWrite();
        if (have_band) {
            band.pushSprite(0, kBandY);
        } else {
            M5.Display.fillRect(0, kBandY, kScreenW, kBandH, bg);
            draw_shape(M5.Display, 90, 120, face.left, color, lip);
            draw_shape(M5.Display, 230, 120, face.right, color, lip);
        }
        privacy_paint_dots();
        {
            constexpr int kBarH = 2;
            const std::int64_t deadline = g_runtime.session_deadline_us.load();
            M5.Display.fillRect(0, 0, kScreenW, kBarH, bg);
            if (deadline > 0) {
                const std::int64_t remain = deadline - esp_timer_get_time();
                float used = 1.0f - static_cast<float>(remain) / static_cast<float>(kMaxSessionUs);
                if (used < 0.0f) used = 0.0f;
                if (used > 1.0f) used = 1.0f;
                int w = static_cast<int>(used * static_cast<float>(kScreenW) + 0.5f);
                if (used > 0.0f && w < 1) w = 1;
                const uint16_t bar = used >= 0.75f ? rgb565(Rgb{255, 196, 72}) : rgb565(Rgb{255, 255, 255});
                if (w > 0) M5.Display.fillRect(0, 0, w, kBarH, bar);
            }
        }
        M5.Display.endWrite();
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
    // Speaker self-test before the wake task takes the shared I2S pins for the mic.
    {
        const bool began = M5.Speaker.begin();
        M5.Speaker.setVolume(180);
        const bool toned = began && M5.Speaker.tone(1000, 150);
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI("eye", "boot chime: speaker begin=%d enabled=%d tone=%d", (int)began, (int)M5.Speaker.isEnabled(),
                 (int)toned);
        M5.Speaker.end();
    }
    M5.Display.setRotation(1);
    M5.Display.setBrightness(160);
    M5.Display.fillScreen(rgb565(kFaceBackground));

    // UNTESTED on a panel. Priority 1 is deliberate; audio tasks use 5.
    xTaskCreatePinnedToCore(eye_task, "eyes", 4096, nullptr, 1, nullptr, 1);
}

} // namespace jarvis
