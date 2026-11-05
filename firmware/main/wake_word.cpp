// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jarvis contributors
//
// WakeNet "Jarvis" (wn9_jarvis_tts), esp-sr pinned to ~2.6.0.
// UNTESTED on a CoreS3 microphone. The feed/detect split follows groki-bot's
// asr_probe.cpp (BSL-1.0); this file is a new caller, not a copy of that task.

#include "link.hpp"
#include "runtime.hpp"
#include "gates.hpp"

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <M5Unified.h>
#include <esp_timer.h>
#include <cmath>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"
#include "model_path.h"

namespace jarvis {

namespace {
constexpr const char* kTag = "wake";

const esp_afe_sr_iface_t* g_afe = nullptr;
esp_afe_sr_data_t* g_afe_data = nullptr;

void feed_task(void*)
{
    const int chunk = g_afe->get_feed_chunksize(g_afe_data);
    const int channels = g_afe->get_channel_num(g_afe_data);
    auto* buf = static_cast<std::int16_t*>(
        heap_caps_malloc(sizeof(std::int16_t) * static_cast<std::size_t>(chunk * channels), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        ESP_LOGE(kTag, "microphone buffer alloc failed");
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        // Half duplex. The speaker and the microphone share one I2S controller.
        if (runtime().speaking.load() || M5.Speaker.isPlaying()) {
            if (mic_gate().is_open()) mic_gate().close();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!mic_gate().is_open() && !mic_gate().open()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!M5.Mic.record(buf, static_cast<std::size_t>(chunk), 16000, false)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // record() returns before the DMA fills buf. Wait for it, or AFE and xAI get a stale,
        // half-written chunk.
        while (M5.Mic.isRecording()) vTaskDelay(1);
        {
            // Idle mic level every ~5 s so a silent or dead mic shows in the log.
            static std::int64_t last_us = 0;
            static int peak = 0;
            static double sum_sq = 0;
            static std::size_t count = 0;
            for (int i = 0; i < chunk; ++i) {
                const int v = buf[i] < 0 ? -buf[i] : buf[i];
                if (v > peak) peak = v;
                sum_sq += static_cast<double>(buf[i]) * buf[i];
            }
            count += static_cast<std::size_t>(chunk);
            const std::int64_t now = esp_timer_get_time();
            if (now - last_us > 5000000) {
                ESP_LOGI(kTag, "mic level rms %d peak %d", static_cast<int>(std::sqrt(sum_sq / (count ? count : 1))), peak);
                last_us = now;
                peak = 0;
                sum_sq = 0;
                count = 0;
            }
        }
        g_afe->feed(g_afe_data, buf);
        if (runtime().ptt.load() || runtime().listening.load()) voice_feed_pcm(buf, static_cast<std::size_t>(chunk));
    }
}

void detect_task(void*)
{
    std::int64_t last_ms = 0;
    for (;;) {
        afe_fetch_result_t* result = g_afe->fetch(g_afe_data);
        if (result == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (result->wakeup_state != WAKENET_DETECTED) continue;
        const std::int64_t now = esp_timer_get_time() / 1000;
        if (now - last_ms < 1500) continue;
        last_ms = now;
        ESP_LOGI(kTag, "wake word");
        voice_start_turn();
    }
}

void start_tasks()
{
    static StaticTask_t feed_tcb;
    static StaticTask_t detect_tcb;
    constexpr int kFeedStack = 4096;
    constexpr int kDetectStack = 6144;
    auto* feed_stack = static_cast<StackType_t*>(heap_caps_malloc(kFeedStack, MALLOC_CAP_SPIRAM));
    auto* detect_stack = static_cast<StackType_t*>(heap_caps_malloc(kDetectStack, MALLOC_CAP_SPIRAM));
    if (feed_stack != nullptr && detect_stack != nullptr) {
        xTaskCreateStaticPinnedToCore(feed_task, "wake_feed", kFeedStack, nullptr, 5, feed_stack, &feed_tcb, 0);
        xTaskCreateStaticPinnedToCore(detect_task, "wake_det", kDetectStack, nullptr, 5, detect_stack, &detect_tcb, 1);
        return;
    }
    xTaskCreatePinnedToCore(feed_task, "wake_feed", kFeedStack, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(detect_task, "wake_det", kDetectStack, nullptr, 5, nullptr, 1);
}
} // namespace

void wake_start()
{
    ESP_LOGW(kTag, "WakeNet UNTESTED. Camera stays off so the microphone can keep its DMA");
    srmodel_list_t* models = esp_srmodel_init("model");
    if (models == nullptr || models->num <= 0) {
        ESP_LOGE(kTag, "no speech models in the model partition");
        return;
    }
    char* model = esp_srmodel_filter(models, ESP_WN_PREFIX, "jarvis");
    if (model == nullptr) {
        ESP_LOGE(kTag, "wn9_jarvis_tts was not in the partition. Check CONFIG_SR_WN_WN9_JARVIS_TTS");
        return;
    }
    ESP_LOGI(kTag, "wakenet model selected");
    afe_config_t* cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (cfg == nullptr) {
        ESP_LOGE(kTag, "afe_config_init failed");
        return;
    }
    cfg->aec_init = false;
    cfg->wakenet_mode = DET_MODE_90;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->pcm_config.total_ch_num = 1;
    cfg->pcm_config.mic_num = 1;
    cfg->pcm_config.ref_num = 0;
    g_afe = esp_afe_handle_from_config(cfg);
    if (g_afe == nullptr) {
        afe_config_free(cfg);
        ESP_LOGE(kTag, "no AFE handle");
        return;
    }
    g_afe_data = g_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (g_afe_data == nullptr) {
        ESP_LOGE(kTag, "AFE create failed");
        return;
    }
    start_tasks();
}

} // namespace jarvis
