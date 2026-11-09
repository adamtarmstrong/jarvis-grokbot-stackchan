// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jarvis contributors
//
// WakeNet "Jarvis" (wn9_jarvis_tts), esp-sr pinned to ~2.6.0.
// UNTESTED on a CoreS3 microphone. The feed/detect split follows groki-bot's
// asr_probe.cpp (BSL-1.0); this file is a new caller, not a copy of that task.

#include "link.hpp"
#include "runtime.hpp"
#include "gates.hpp"

#include <atomic>
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
std::atomic<bool> g_wake_request{false};

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
        // voice_start_turn reads NVS (flash). That cannot run on a PSRAM task stack
        // (cache-disable assert). Detect only sets the flag; we start the turn here.
        if (g_wake_request.exchange(false)) {
            if (!(runtime().listening.load() || runtime().speaking.load() || runtime().ptt.load())) {
                voice_start_turn();
            } else {
                ESP_LOGI(kTag, "wake word ignored; turn already open");
            }
        }
    }
}

void detect_task(void*)
{
    std::int64_t last_ms = 0;
    std::int64_t last_vol_ms = 0;
    for (;;) {
        afe_fetch_result_t* result = g_afe->fetch(g_afe_data);
        if (result == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const std::int64_t now_ms = esp_timer_get_time() / 1000;
        // Integer-only diagnostics (float printf is stack-heavy on the detect task).
        if (now_ms - last_vol_ms > 2000) {
            last_vol_ms = now_ms;
            const int vol_x10 = static_cast<int>(result->data_volume * 10.0f);
            const int ring_x100 = static_cast<int>(result->ringbuff_free_pct * 100.0f);
            if (vol_x10 > -450 || ring_x100 > 50) {
                ESP_LOGI(kTag, "afe vol=%d.%d dBFS ring_free=%d%% vad=%d", vol_x10 / 10,
                         (vol_x10 < 0 ? -vol_x10 : vol_x10) % 10, ring_x100, static_cast<int>(result->vad_state));
            }
        }
        if (result->wakeup_state != WAKENET_DETECTED) continue;
        if (now_ms - last_ms < 1500) continue;
        last_ms = now_ms;
        // Do not call voice_start_turn here: it reads NVS/flash. With a PSRAM stack that
        // trips esp_task_stack_is_sane_cache_disabled. Signal the feed task instead.
        ESP_LOGI(kTag, "wake word");
        g_wake_request.store(true);
    }
}

void start_tasks()
{
    // Stacks must be in internal RAM: voice_start_turn (from feed) and any flash/NVS
    // path disable the cache, which asserts if the task stack lives in PSRAM.
    constexpr int kFeedStack = 8192;
    constexpr int kDetectStack = 8192;
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
    // StackChan board mic magnification defaults to 2; enable WakeNet-driven AGC and a
    // slightly more sensitive detector so desk-distance "Hey Jarvis" still triggers.
    cfg->agc_init = true;
    cfg->agc_mode = AFE_AGC_MODE_WAKENET;
    cfg->wakenet_mode = DET_MODE_95;
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
