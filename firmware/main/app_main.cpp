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
#include <nvs_flash.h>

#include "board/si12t_touch.hpp"

namespace jarvis {
namespace {
constexpr const char* kTag = "jarvis";

bool boot_setup_requested()
{
    // Existing robots can reopen setup without erasing NVS or using a command line.
    // Hold the display during startup; consecutive samples reject a stray tap.
    unsigned held = 0;
    for (unsigned sample = 0; sample < 60; ++sample) {
        M5.update();
        held = M5.Touch.getCount() > 0 ? held + 1 : 0;
        if (held >= 40) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
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
    if (!clock_sync()) ESP_LOGW(kTag, "clock not synced; the Worker will reject signatures until it is");

    eye_start();
    if (boot_setup_requested()) {
        portal_start();
        return;
    }
    privacy_start();
    head_start();
    DeviceConfig cfg;
    if (config_load(cfg) && cfg.ha_url.empty()) worker_ws_start(cfg);
    wake_start();
    xTaskCreatePinnedToCore(ptt_task, "ptt", 4096, nullptr, 4, nullptr, 1);
    camera_gate().close();
}
