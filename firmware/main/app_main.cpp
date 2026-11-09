// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jarvis contributors
//
// UNTESTED on a StackChan. No board was attached in the build environment.

#include "link.hpp"
#include "runtime.hpp"
#include "gates.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <cstdint>
#include <nvs_flash.h>

#include "board/si12t_touch.hpp"
#include "jarvis/privacy.hpp"

namespace jarvis {
namespace {
constexpr const char* kTag = "jarvis";

void restore_face_display()
{
    // eye_task only redraws a 320x160 band; wipe prompt pixels and text state first.
    M5.Display.setRotation(1);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextSize(1);
    const uint16_t bg = M5.Display.color565(kFaceBackground.r, kFaceBackground.g, kFaceBackground.b);
    M5.Display.setTextColor(0xFFFF, bg);
    M5.Display.fillScreen(bg);
}

bool boot_setup_requested()
{
    // Existing robots can reopen setup without erasing NVS or using a command line.
    // Use the same touch sources as PTT (panel + Si12T). Consecutive samples reject a stray tap.
    stackchan::board::Si12tTouch* touch = nullptr;
    if (auto probed = stackchan::board::Si12tTouch::probe()) {
        touch = new stackchan::board::Si12tTouch(std::move(*probed));
        touch->recalibrate();
        ESP_LOGI(kTag, "setup-hold: Si12T ready");
    } else {
        ESP_LOGW(kTag, "setup-hold: Si12T probe failed; panel touch only");
    }

    const uint16_t bg = M5.Display.color565(kFaceBackground.r, kFaceBackground.g, kFaceBackground.b);
    M5.Display.setRotation(1);
    M5.Display.fillScreen(bg);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0xFFFF, bg);
    M5.Display.drawString("Hold for setup", M5.Display.width() / 2, M5.Display.height() / 2);

    constexpr unsigned kSamples = 160;   // 8.0 s at 50 ms
    constexpr unsigned kHoldNeed = 40;   // 2.0 s continuous
    unsigned held = 0;
    bool requested = false;
    for (unsigned sample = 0; sample < kSamples; ++sample) {
        M5.update();
        const int panel = M5.Touch.getCount();
        bool down = panel > 0;
        std::uint8_t f = 0, m = 0, b = 0;
        if (touch != nullptr) {
            const auto reading = touch->read();
            f = reading.front();
            m = reading.middle();
            b = reading.back();
            if (reading.firmly_touched()) down = true;
        }
        held = down ? held + 1 : 0;
        if (sample % 20 == 0) {
            ESP_LOGI(kTag, "setup-hold sample=%u held=%u panel=%d si12t=%u/%u/%u", sample, held, panel, f, m, b);
        }
        if (held >= kHoldNeed) {
            ESP_LOGW(kTag, "setup-hold detected (held=%u); opening portal", held);
            requested = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!requested) ESP_LOGI(kTag, "setup-hold timed out");
    delete touch;
    restore_face_display();
    return requested;
}

void ptt_task(void*)
{
    stackchan::board::Si12tTouch* touch = nullptr;
    if (auto probed = stackchan::board::Si12tTouch::probe()) touch = new stackchan::board::Si12tTouch(std::move(*probed));
    bool held = false;
    for (;;) {
        M5.update();
        bool down = M5.Touch.getCount() > 0;
        if (touch != nullptr && touch->read().firmly_touched()) down = true;
        runtime().ptt.store(down);
        if (down && !held) voice_start_turn();
        if (!down && held) voice_stop_turn();
        held = down;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
} // namespace
} // namespace jarvis

extern "C" void app_main(void)
{
    using namespace jarvis;
    ESP_LOGW(kTag, "Jarvis firmware. Servo, microphone, display, wake word, portal, and OTA are UNTESTED.");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (!config_is_ready() || !wifi_join_saved()) {
        portal_start();
        return;
    }
    esp_ota_mark_app_valid_cancel_rollback();

    // Bring the display/touch up before the hold window so Si12T and the panel are live.
    // Clock sync waits until after the window; setup entry must not sit behind SNTP.
    eye_start();
    if (boot_setup_requested()) {
        portal_start();
        return;
    }
    if (!clock_sync()) ESP_LOGW(kTag, "clock not synced; the Worker will reject signatures until it is");
    eye_run();
    privacy_start();
    head_start();
    DeviceConfig cfg;
    if (config_load(cfg) && cfg.ha_url.empty()) worker_ws_start(cfg);
    wake_start();
    xTaskCreatePinnedToCore(ptt_task, "ptt", 4096, nullptr, 4, nullptr, 1);
    camera_gate().close();
}
