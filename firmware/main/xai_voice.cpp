// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jarvis contributors
//
// UNTESTED against a live xAI session. Event names follow the Voice Agent
// schema checked on 2026-10-19 (session.update, input_audio_buffer.append,
// response.output_audio.delta, response.function_call_arguments.done).
// client_secrets does not accept a voice, so the voice is set here.

#include "link.hpp"
#include "runtime.hpp"
#include "b64.hpp"
#include "pcm_ring.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <M5Unified.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_websocket_client.h>
#include <lwip/netdb.h>

#include <cmath>

namespace jarvis {

namespace {
constexpr const char* kTag = "voice";

struct SpeakItem {
    std::string text;
    std::string request_id;
};

std::mutex g_mu;
std::deque<SpeakItem> g_speak;
std::atomic<bool> g_want{false};
std::atomic<bool> g_interrupt{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_saw_connected{false};
std::atomic<bool> g_closing{false};
std::atomic<bool> g_turn_done{false};
std::atomic<bool> g_started{false};
esp_websocket_client_handle_t g_xai = nullptr;
std::string g_secret_header;
std::string g_realtime_url;
std::string g_voice = "leo";
std::int64_t g_expires_at = 0;
std::atomic<bool> g_need_update{false};
std::atomic<bool> g_awaiting_update{false};
constexpr std::size_t kRxCap = 262144; // assemble large audio-delta JSON in PSRAM
char* g_rx_buf = nullptr;
std::size_t g_rx_len = 0;
bool g_rx_drop = false;

std::string g_active_reply;
std::atomic<bool> g_spoke{false};
int g_open_failures = 0;
std::atomic<bool> g_ready_beep{false};
std::atomic<bool> g_ack_beep{false};
std::atomic<std::int64_t> g_listen_deadline_us{0};
std::atomic<std::int64_t> g_next_mic_send_us{0};
constexpr std::int64_t kListenWindowUs = 8'000'000;
std::mutex g_mic_send_mu;
constexpr std::size_t kMicSendCap = 80'000; // 5 s at 16 kHz, kept in PSRAM so TLS can use internal RAM
std::int16_t* g_mic_ring = nullptr;
std::size_t g_mic_r = 0;
std::size_t g_mic_w = 0;
std::size_t g_mic_n = 0;

bool mic_ring_ensure()
{
    if (g_mic_ring != nullptr) return true;
    g_mic_ring = static_cast<std::int16_t*>(
        heap_caps_malloc(kMicSendCap * sizeof(std::int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_mic_ring == nullptr) {
        ESP_LOGE(kTag, "mic ring PSRAM alloc failed");
        return false;
    }
    g_mic_r = g_mic_w = g_mic_n = 0;
    ESP_LOGI(kTag, "mic ring ready in PSRAM (%u samples)", (unsigned)kMicSendCap);
    return true;
}

void mic_ring_clear()
{
    g_mic_r = g_mic_w = g_mic_n = 0;
}

void mic_ring_push(std::int16_t sample)
{
    if (!mic_ring_ensure()) return;
    if (g_mic_n == kMicSendCap) {
        g_mic_r += 1;
        if (g_mic_r == kMicSendCap) g_mic_r = 0;
        g_mic_n -= 1;
    }
    g_mic_ring[g_mic_w] = sample;
    g_mic_w += 1;
    if (g_mic_w == kMicSendCap) g_mic_w = 0;
    g_mic_n += 1;
}

std::size_t mic_ring_pop(std::int16_t* dest, std::size_t max)
{
    const std::size_t n = g_mic_n < max ? g_mic_n : max;
    for (std::size_t i = 0; i < n; ++i) {
        dest[i] = g_mic_ring[g_mic_r];
        g_mic_r += 1;
        if (g_mic_r == kMicSendCap) g_mic_r = 0;
    }
    g_mic_n -= n;
    return n;
}

void mic_ring_keep_latest(std::size_t max_samples)
{
    if (g_mic_n <= max_samples) return;
    const std::size_t drop = g_mic_n - max_samples;
    g_mic_r += drop;
    if (g_mic_r >= kMicSendCap) g_mic_r -= kMicSendCap;
    g_mic_n = max_samples;
}

// Speaker and microphone share one I2S controller. Raise speaking so the wake task closes
// the mic, then start the speaker. Returns false if the speaker would not start.
bool speaker_ready()
{
    runtime().speaking.store(true);
    // If the speaker already owns I2S, do not wait on the mic — that wait was
    // happening before this check and stretched speech to about 20 ms per second.
    if (M5.Speaker.isEnabled()) return true;
    int waited = 0;
    for (; waited < 100 && M5.Mic.isEnabled(); ++waited) vTaskDelay(pdMS_TO_TICKS(10));
    if (M5.Speaker.isEnabled()) return true;
    const bool mic_still_on = M5.Mic.isEnabled();
    const bool began = M5.Speaker.begin();
    ESP_LOGI(kTag, "speaker begin=%d enabled=%d mic_on=%d waited_ms=%d", (int)began, (int)M5.Speaker.isEnabled(),
             (int)mic_still_on, waited * 10);
    if (!began) return false;
    M5.Speaker.setVolume(180);
    return true;
}

// Resolve the Worker host before minting. Wi-Fi can be up while DNS still fails.
bool wait_for_dns(const std::string& base)
{
    std::string host = base;
    if (host.rfind("https://", 0) == 0) host.erase(0, 8);
    const auto cut = host.find_first_of("/:");
    if (cut != std::string::npos) host.resize(cut);
    if (host.empty()) return false;
    for (int attempt = 1; attempt <= 6; ++attempt) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        const int rc = getaddrinfo(host.c_str(), "443", &hints, &res);
        if (res != nullptr) freeaddrinfo(res);
        if (rc == 0) {
            if (attempt > 1) ESP_LOGI(kTag, "worker DNS ok on try %d", attempt);
            return true;
        }
        ESP_LOGW(kTag, "worker DNS try %d failed (%d)", attempt, rc);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    return false;
}

struct ToolCall {
    bool pending = false;
    std::string call_id;
    std::string name;
    std::string arguments;
};
ToolCall g_tool;

std::mutex g_pcm_mu;
PcmRing g_pcm;
constexpr std::size_t kPcmCap = 16000 * 60;
constexpr std::size_t kPlayChunk = 3200;
constexpr std::size_t kPlayBuffers = 3;
std::int16_t* g_audio_storage = nullptr;
std::size_t g_play_index = 0;
std::atomic<bool> g_audio_failed{false};

bool rx_ensure()
{
    if (g_rx_buf != nullptr) return true;
    g_rx_buf = static_cast<char*>(heap_caps_malloc(kRxCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_rx_buf == nullptr) {
        ESP_LOGE(kTag, "rx PSRAM alloc failed");
        return false;
    }
    g_rx_len = 0;
    g_rx_drop = false;
    ESP_LOGI(kTag, "rx assembly buffer ready in PSRAM (%u bytes)", (unsigned)kRxCap);
    return true;
}

bool audio_storage_ensure()
{
    if (!rx_ensure()) return false;
    std::lock_guard<std::mutex> lock(g_pcm_mu);
    if (g_audio_storage != nullptr) return true;
    g_audio_storage = static_cast<std::int16_t*>(heap_caps_malloc(
        (kPcmCap + kPlayBuffers * kPlayChunk) * sizeof(std::int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_audio_storage == nullptr) {
        ESP_LOGE(kTag, "playback PSRAM alloc failed");
        return false;
    }
    g_pcm.bind(g_audio_storage, kPcmCap);
    return true;
}

void send_xai(const char* json);

void flush_mic_to_xai()
{
    if (g_xai == nullptr || !g_connected.load()) return;
    if (runtime().speaking.load()) return;
    if (!runtime().ptt.load() && !runtime().listening.load()) return;
    // Wait until session.updated so format and server VAD apply to this audio.
    if (g_need_update || g_awaiting_update.load()) return;
    const auto now = esp_timer_get_time();
    if (now < g_next_mic_send_us.load()) return;

    constexpr std::size_t kChunk = 800;
    std::int16_t chunk[kChunk];
    std::size_t n = 0;
    std::size_t remaining = 0;
    {
        std::lock_guard<std::mutex> lock(g_mic_send_mu);
        n = mic_ring_pop(chunk, kChunk);
        remaining = g_mic_n;
    }
    if (n == 0) return;

    const auto encoded = b64_encode(reinterpret_cast<const std::uint8_t*>(chunk), n * sizeof(std::int16_t));
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
    cJSON_AddStringToObject(root, "audio", encoded.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) return;
    send_xai(printed);
    cJSON_free(printed);
    g_next_mic_send_us.store(esp_timer_get_time() + static_cast<std::int64_t>(n) * 1000000 / 16000);

    static unsigned appends = 0;
    static int peak = 0;
    static double sum_sq = 0;
    static std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int v = chunk[i] < 0 ? -chunk[i] : chunk[i];
        if (v > peak) peak = v;
        sum_sq += static_cast<double>(chunk[i]) * chunk[i];
    }
    count += n;
    if ((appends++ % 50) == 0) {
        ESP_LOGI(kTag, "mic append #%u (%u samples, pcm16 16 kHz mono LE b64) rms %d peak %d, ring %u", appends,
                 (unsigned)n, static_cast<int>(std::sqrt(sum_sq / (count ? count : 1))), peak, (unsigned)remaining);
        peak = 0;
        sum_sq = 0;
        count = 0;
    }
}

void send_xai(const char* json)
{
    if (g_xai == nullptr || json == nullptr) return;
    if (!esp_websocket_client_is_connected(g_xai)) return;
    const int nbytes = static_cast<int>(std::strlen(json));
    const auto t0 = esp_timer_get_time();
    // 3 s was shorter than a single TCP retransmit against the 5760-byte send
    // window. The IDF client closes the socket if poll_write times out.
    const int sent = esp_websocket_client_send_text(g_xai, json, nbytes, pdMS_TO_TICKS(15000));
    const int elapsed_ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);
    if (sent <= 0) {
        ESP_LOGW(kTag, "realtime send failed (%d bytes, sent %d, %d ms)", nbytes, sent, elapsed_ms);
    } else if (elapsed_ms >= 200) {
        ESP_LOGI(kTag, "realtime send slow (%d bytes in %d ms)", nbytes, elapsed_ms);
    }
}

void queue_pcm(const std::uint8_t* data, std::size_t len)
{
    if (len < 2) return;
    std::lock_guard<std::mutex> lock(g_pcm_mu);
    const std::size_t samples = len / 2;
    for (std::size_t i = 0; i < samples; ++i) {
        const auto lo = data[i * 2];
        const auto hi = data[i * 2 + 1];
        if (!g_pcm.push(static_cast<std::int16_t>(lo | (hi << 8)))) {
            if (!g_audio_failed.exchange(true)) ESP_LOGE(kTag, "playback buffer full; aborting turn");
            break;
        }
    }
}

void note_audio_delta(const cJSON* root)
{
    const cJSON* delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
    if (!cJSON_IsString(delta)) return;
    const auto pcm = b64_decode(delta->valuestring);
    queue_pcm(pcm.data(), pcm.size());
    g_spoke = true;
    runtime().speaking.store(true);
    runtime().face.store(FaceState::Speaking);
    static unsigned audio_deltas = 0;
    if ((audio_deltas++ % 25) == 0) {
        std::lock_guard<std::mutex> lock(g_pcm_mu);
        ESP_LOGI(kTag, "audio delta #%u +%u bytes, queued %u samples", audio_deltas, (unsigned)pcm.size(),
                 (unsigned)g_pcm.size());
    }
}

void xai_event(void*, esp_event_base_t, std::int32_t id, void* data)
{
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        if (g_closing.load()) {
            ESP_LOGW(kTag, "realtime connected after close; ignoring");
            return;
        }
        ESP_LOGI(kTag, "realtime connected");
        g_connected.store(true);
        g_saw_connected.store(true);
        g_need_update = true;
        runtime().session_deadline_us.store(esp_timer_get_time() + kMaxSessionUs);
        return;
    }
    if (id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_DISCONNECTED) {
        const auto* e = static_cast<esp_websocket_event_data_t*>(data);
        ESP_LOGW(kTag, "realtime %s status=%d", id == WEBSOCKET_EVENT_ERROR ? "error" : "disconnected",
                 e != nullptr ? e->error_handle.esp_ws_handshake_status_code : -1);
    }
    if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        g_connected.store(false);
        g_awaiting_update.store(false);
        g_listen_deadline_us.store(0);
        runtime().session_deadline_us.store(0);
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA || data == nullptr) return;
    const auto* evt = static_cast<esp_websocket_event_data_t*>(data);
    if (evt->op_code != 0x1 || evt->data_ptr == nullptr || evt->data_len <= 0) return;
    if (evt->payload_offset == 0) {
        g_rx_len = 0;
        g_rx_drop = false;
    }
    if (g_rx_drop) return;
    if (!rx_ensure()) return;
    const auto chunk_n = static_cast<std::size_t>(evt->data_len);
    if (g_rx_len + chunk_n > kRxCap) {
        ESP_LOGW(kTag, "dropping oversized ws event (%u + %u > %u, payload %u)", (unsigned)g_rx_len,
                 (unsigned)chunk_n, (unsigned)kRxCap, (unsigned)evt->payload_len);
        g_rx_len = 0;
        g_rx_drop = true;
        return;
    }
    std::memcpy(g_rx_buf + g_rx_len, evt->data_ptr, chunk_n);
    g_rx_len += chunk_n;
    if (evt->payload_offset + evt->data_len < evt->payload_len) return;

    cJSON* root = cJSON_ParseWithLength(g_rx_buf, g_rx_len);
    g_rx_len = 0;
    if (root == nullptr) return;
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    const char* name = type->valuestring;
    static unsigned deltas = 0;
    if (std::strstr(name, "delta") == nullptr) ESP_LOGI(kTag, "event %s", name);
    else if ((deltas++ % 25) == 0) ESP_LOGI(kTag, "%s #%u", name, deltas);
    // Log the event name only. Deltas and transcripts stay off the console.
    if (std::strcmp(name, "response.output_audio.delta") == 0 || std::strcmp(name, "response.audio.delta") == 0) {
        note_audio_delta(root);
    } else if (std::strcmp(name, "session.created") == 0 || std::strcmp(name, "conversation.created") == 0) {
        ESP_LOGI(kTag, "%s", name);
    } else if (std::strcmp(name, "session.updated") == 0) {
        ESP_LOGI(kTag, "session.updated");
        if (g_awaiting_update.exchange(false)) g_ready_beep.store(true);
    } else if (std::strcmp(name, "input_audio_buffer.speech_started") == 0) {
        g_listen_deadline_us.store(0);
        runtime().face.store(FaceState::Listening);
    } else if (std::strcmp(name, "input_audio_buffer.speech_stopped") == 0) {
        runtime().face.store(FaceState::Thinking);
    } else if (std::strcmp(name, "response.created") == 0) {
        g_turn_done.store(false);
        runtime().face.store(FaceState::Thinking);
    } else if (std::strcmp(name, "response.function_call_arguments.done") == 0) {
        const cJSON* call_id = cJSON_GetObjectItemCaseSensitive(root, "call_id");
        const cJSON* fn = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON* args = cJSON_GetObjectItemCaseSensitive(root, "arguments");
        if (cJSON_IsString(call_id) && cJSON_IsString(fn) && cJSON_IsString(args)) {
            std::lock_guard<std::mutex> lock(g_mu);
            g_tool.pending = true;
            g_tool.call_id = call_id->valuestring;
            g_tool.name = fn->valuestring;
            g_tool.arguments = args->valuestring;
        }
    } else if (std::strcmp(name, "response.done") == 0 || std::strcmp(name, "response.output_audio.done") == 0) {
        if (std::strcmp(name, "response.done") == 0) g_turn_done.store(true);
        ESP_LOGI(kTag, "%s", name);
    } else if (std::strcmp(name, "error") == 0) {
        ESP_LOGW(kTag, "realtime error event");
    }
    cJSON_Delete(root);
}

bool mint_secret()
{
    DeviceConfig cfg;
    if (!config_load(cfg)) {
        ESP_LOGW(kTag, "no device config");
        return false;
    }
    std::string body;
    if (!wait_for_dns(cfg.worker_base)) return false;
    if (!worker_post(cfg, "/token", "{}", body)) return false;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;
    const cJSON* secret = cJSON_GetObjectItemCaseSensitive(root, "client_secret");
    const cJSON* url = cJSON_GetObjectItemCaseSensitive(root, "realtime_url");
    const cJSON* voice = cJSON_GetObjectItemCaseSensitive(root, "voice");
    const cJSON* exp = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    const bool ok = cJSON_IsString(secret) && secret->valuestring[0] != '\0';
    if (ok) {
        g_secret_header = std::string("Authorization: Bearer ") + secret->valuestring + "\r\n";
        g_realtime_url = cJSON_IsString(url) ? url->valuestring : "wss://api.x.ai/v1/realtime?model=grok-voice-3.0";
        if (cJSON_IsString(voice) && voice->valuestring[0] != '\0') g_voice = voice->valuestring;
        g_expires_at = cJSON_IsNumber(exp) ? static_cast<std::int64_t>(exp->valuedouble) : 0;
    }
    cJSON_Delete(root);
    ESP_LOGI(kTag, "token mint %s", ok ? "ok" : "rejected");
    return ok;
}

void send_session_update()
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON* session = cJSON_AddObjectToObject(root, "session");
    cJSON_AddStringToObject(session, "instructions",
                            "You are Jarvis, a desk robot. Speak in one to three short sentences. "
                            "No markdown and no URLs. For anything that needs the owner's other bots "
                            "or a longer lookup, call ask_bot and then wait.");
    cJSON_AddStringToObject(session, "voice", g_voice.c_str());
    cJSON* vad = cJSON_AddObjectToObject(session, "turn_detection");
    cJSON_AddStringToObject(vad, "type", "server_vad");
    cJSON* audio = cJSON_AddObjectToObject(session, "audio");
    cJSON* in = cJSON_AddObjectToObject(audio, "input");
    cJSON* in_fmt = cJSON_AddObjectToObject(in, "format");
    cJSON_AddStringToObject(in_fmt, "type", "audio/pcm");
    cJSON_AddNumberToObject(in_fmt, "rate", 16000);
    cJSON* out = cJSON_AddObjectToObject(audio, "output");
    cJSON* out_fmt = cJSON_AddObjectToObject(out, "format");
    cJSON_AddStringToObject(out_fmt, "type", "audio/pcm");
    cJSON_AddNumberToObject(out_fmt, "rate", 16000);
    cJSON* tools = cJSON_AddArrayToObject(session, "tools");
    cJSON* web = cJSON_CreateObject();
    cJSON_AddStringToObject(web, "type", "web_search");
    cJSON_AddItemToArray(tools, web);
    cJSON* x = cJSON_CreateObject();
    cJSON_AddStringToObject(x, "type", "x_search");
    cJSON_AddItemToArray(tools, x);
    cJSON* fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "type", "function");
    cJSON_AddStringToObject(fn, "name", "ask_bot");
    cJSON_AddStringToObject(fn, "description", "Hand a question to the owner's Jarvis bot.");
    cJSON* params = cJSON_AddObjectToObject(fn, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON* props = cJSON_AddObjectToObject(params, "properties");
    cJSON* task = cJSON_AddObjectToObject(props, "task");
    cJSON_AddStringToObject(task, "type", "string");
    cJSON* required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("task"));
    cJSON_AddItemToArray(tools, fn);
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed != nullptr) {
        send_xai(printed);
        cJSON_free(printed);
    }
}

bool open_realtime()
{
    if (!audio_storage_ensure()) return false;
    if (g_secret_header.empty() && !mint_secret()) return false;
    esp_websocket_client_config_t ws = {};
    ws.uri = g_realtime_url.c_str();
    ws.headers = g_secret_header.c_str();
    ws.buffer_size = 16384;
    ws.crt_bundle_attach = esp_crt_bundle_attach;
    ws.network_timeout_ms = 10000;
    // The ephemeral secret works once. The library's own reconnect replays it and gets 401,
    // and each stale client keeps a TLS session, which ran the heap out (mbedtls -0x7F00).
    ws.disable_auto_reconnect = true;
    g_xai = esp_websocket_client_init(&ws);
    if (g_xai == nullptr) return false;
    esp_websocket_register_events(g_xai, WEBSOCKET_EVENT_ANY, xai_event, nullptr);
    g_saw_connected.store(false);
    g_closing.store(false);
    if (esp_websocket_client_start(g_xai) != ESP_OK) {
        g_closing.store(true);
        esp_websocket_client_destroy(g_xai);
        g_xai = nullptr;
        return false;
    }
    ESP_LOGI(kTag, "realtime connecting");
    return true;
}

void close_realtime()
{
    g_closing.store(true);
    g_connected.store(false);
    g_saw_connected.store(false);
    runtime().session_deadline_us.store(0);
    if (g_xai != nullptr) {
        esp_websocket_client_stop(g_xai);
        esp_websocket_client_destroy(g_xai);
        g_xai = nullptr;
    }
    g_connected.store(false);
    g_secret_header.clear();
    // Stop the playback task before its borrowed buffers can be reused by another turn.
    runtime().speaking.store(true);
    M5.Speaker.end();
    {
        std::lock_guard<std::mutex> lock(g_pcm_mu);
        g_pcm.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_mic_send_mu);
        mic_ring_clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_tool = ToolCall{};
    }
    g_audio_failed.store(false);
    g_turn_done.store(false);
    g_spoke.store(false);
    g_ready_beep.store(false);
    g_awaiting_update.store(false);
    g_need_update = false;
    g_listen_deadline_us.store(0);
    g_play_index = 0;
    cJSON* end = cJSON_CreateObject();
    cJSON_AddStringToObject(end, "type", "session_end");
    char* printed = cJSON_PrintUnformatted(end);
    cJSON_Delete(end);
    if (printed != nullptr) {
        worker_send_text(printed);
        cJSON_free(printed);
    }
    runtime().speaking.store(false);
    runtime().face.store(FaceState::Idle);
    ESP_LOGI(kTag, "session closed; internal free %u largest %u; voice stack watermark %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

void mark_played_if_spoken()
{
    if (g_active_reply.empty() || !g_spoke) return;
    cJSON* ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "type", "state");
    cJSON_AddStringToObject(ack, "request_id", g_active_reply.c_str());
    cJSON_AddStringToObject(ack, "state", "played");
    char* printed = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (printed != nullptr) {
        worker_send_text(printed);
        cJSON_free(printed);
    }
    g_active_reply.clear();
    g_spoke = false;
}

void inject_text(const SpeakItem& item)
{
    g_turn_done.store(false);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON* msg = cJSON_AddObjectToObject(root, "item");
    cJSON_AddStringToObject(msg, "type", "message");
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON* content = cJSON_AddArrayToObject(msg, "content");
    cJSON* part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "type", "input_text");
    const std::string prompt = "Say this to the person in one to three sentences, with no markdown and no URLs: " + item.text;
    cJSON_AddStringToObject(part, "text", prompt.c_str());
    cJSON_AddItemToArray(content, part);
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed != nullptr) {
        send_xai(printed);
        cJSON_free(printed);
    }
    send_xai("{\"type\":\"response.create\"}");
    g_active_reply = item.request_id;
    g_spoke = false;
    runtime().face.store(FaceState::Thinking);
}

void answer_tool()
{
    ToolCall call;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_tool.pending || !g_turn_done.load()) return;
        call = g_tool;
        g_tool.pending = false;
    }
    // The worker websocket accepts session/state events, not ask requests. Do not
    // start another TLS client while realtime owns the scarce internal TLS heap.
    ESP_LOGW(kTag, "ask_bot unavailable during realtime; no HTTPS request opened");
    g_turn_done.store(false);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "type", "conversation.item.create");
    cJSON* item = cJSON_AddObjectToObject(out, "item");
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call.call_id.c_str());
    cJSON_AddStringToObject(item, "output", "{\"status\":\"failed\",\"reason\":\"bot handoff unavailable during voice; do not retry\"}");
    char* out_json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (out_json != nullptr) {
        send_xai(out_json);
        cJSON_free(out_json);
    }
    send_xai("{\"type\":\"response.create\"}");
    runtime().face.store(FaceState::Thinking);
}

bool pcm_pending()
{
    std::lock_guard<std::mutex> lock(g_pcm_mu);
    return !g_pcm.empty();
}

bool tool_pending()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_tool.pending;
}

void play_some()
{
    // playRaw borrows memory. Keep three buffers in rotation and at most two
    // requests on one channel, rather than overwriting one live buffer or mixing
    // each new chunk onto an automatically selected channel.
    if (M5.Speaker.isPlaying(0) >= 2) return;
    if (!pcm_pending()) {
        if (!M5.Speaker.isPlaying() && (!g_spoke.load() || g_turn_done.load())) runtime().speaking.store(false);
        return;
    }
    if (!speaker_ready()) {
        g_audio_failed.store(true);
        return;
    }
    std::int16_t* chunk = g_audio_storage + kPcmCap + g_play_index * kPlayChunk;
    std::size_t n = 0;
    std::size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(g_pcm_mu);
        n = g_pcm.pop(chunk, kPlayChunk);
        queued = g_pcm.size();
    }
    if (n == 0) return;
    runtime().face.store(FaceState::Speaking);
    static std::int64_t last_log = 0;
    if (esp_timer_get_time() - last_log > 1000000) {
        last_log = esp_timer_get_time();
        ESP_LOGI(kTag, "speaker play %u samples, queued %u, ok=%d", (unsigned)n, (unsigned)queued,
                 (int)M5.Speaker.isEnabled());
    }
    if (!M5.Speaker.playRaw(chunk, n, 16000, false, 1, 0, false)) {
        ESP_LOGE(kTag, "speaker rejected PCM; ending turn");
        g_audio_failed.store(true);
        return;
    }
    g_play_index = (g_play_index + 1) % kPlayBuffers;
    lip_feed(chunk, n);
}

void voice_task(void*)
{
    ESP_LOGW(kTag, "xAI voice task UNTESTED on hardware and against a live session");
    for (;;) {
        SpeakItem next;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (!g_speak.empty()) {
                next = std::move(g_speak.front());
                g_speak.pop_front();
                have = true;
            }
        }
        if (!g_want.load() && !have && !tool_pending() && !g_connected.load() && g_xai == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (g_ack_beep.exchange(false)) {
            // Immediate "I heard you" tone so the wait for /token is not silent.
            if (speaker_ready()) {
                const bool toned = M5.Speaker.tone(1046, 110);
                ESP_LOGI(kTag, "heard-you chime tone=%d", (int)toned);
                for (int i = 0; i < 20 && M5.Speaker.isPlaying(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
            }
            runtime().speaking.store(false);
        }
        if (g_xai == nullptr || !g_connected.load()) {
            if (g_xai != nullptr) {
                // A dropped client, or the handshake never finished. End this turn.
                // Minting again spends another reservation and can play another ready chime.
                if (g_saw_connected.load()) {
                    ESP_LOGW(kTag, "realtime dropped; ending turn instead of opening another session; heap %u",
                             (unsigned)esp_get_free_heap_size());
                } else {
                    ESP_LOGW(kTag, "realtime connect timed out; ending turn; heap %u",
                             (unsigned)esp_get_free_heap_size());
                }
                runtime().listening.store(false);
                if (!runtime().ptt.load()) g_want.store(false);
                close_realtime();
                continue;
            }
            if (!g_want.load() && !have && !runtime().ptt.load()) {
                continue;
            }
            ESP_LOGI(kTag, "opening realtime session");
            if (!open_realtime()) {
                // A successful mint has already spent an hourly grant. Do not
                // silently reserve another token if client creation/start fails.
                if (!g_secret_header.empty()) {
                    close_realtime();
                    runtime().listening.store(false);
                    g_want.store(false);
                    g_open_failures = 0;
                    continue;
                }
                // The Worker answers 429 while its session, hourly, or daily cap is hit.
                // Give up after three tries so the face does not stay in Listening forever.
                if (++g_open_failures >= 3) {
                    ESP_LOGW(kTag, "voice session unavailable after %d tries; giving up this turn", g_open_failures);
                    g_open_failures = 0;
                    {
                        std::lock_guard<std::mutex> lock(g_mu);
                        g_speak.clear();
                    }
                    g_want.store(false);
                    runtime().listening.store(false);
                    runtime().face.store(FaceState::Idle);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }
                if (have) {
                    std::lock_guard<std::mutex> lock(g_mu);
                    g_speak.push_front(std::move(next));
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            g_open_failures = 0;
            // TLS over a weak AP can take longer than 5 s; giving up early tears down a
            // handshake that then completes and used to open a second session.
            for (int i = 0; i < 200 && !g_connected.load(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (g_connected.load() && g_need_update.exchange(false)) {
            // Send once per connection. Re-sending every loop produced several session.updated.
            g_awaiting_update.store(true);
            send_session_update();
        }
        answer_tool();
        if (have && g_connected.load() && !g_need_update && !g_awaiting_update.load()) inject_text(next);
        else if (have) {
            std::lock_guard<std::mutex> lock(g_mu);
            g_speak.push_front(std::move(next));
        }
        if (g_ready_beep.exchange(false)) {
            // Ready to hear: chime, then hand the I2S back to the mic and open the listen window.
            if (speaker_ready()) {
                const bool toned = M5.Speaker.tone(1320, 120);
                ESP_LOGI(kTag, "ready chime tone=%d playing=%d", (int)toned, (int)M5.Speaker.isPlaying());
                vTaskDelay(pdMS_TO_TICKS(140));
                M5.Speaker.tone(1760, 120);
                for (int i = 0; i < 40 && M5.Speaker.isPlaying(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
            }
            runtime().speaking.store(false);
            runtime().face.store(FaceState::Listening);
            {
                std::lock_guard<std::mutex> lock(g_mic_send_mu);
                const std::size_t before = g_mic_n;
                constexpr std::size_t kPreRoll = 4800; // 300 ms at 16 kHz
                mic_ring_keep_latest(kPreRoll);
                ESP_LOGI(kTag, "listen pre-roll kept %u of %u samples", (unsigned)g_mic_n, (unsigned)before);
            }
            g_next_mic_send_us.store(0);
            if (runtime().listening.load()) g_listen_deadline_us.store(esp_timer_get_time() + kListenWindowUs);
            ESP_LOGI(kTag, "ready chime played; listening for 8 s");
        }
        {
            const std::int64_t deadline = g_listen_deadline_us.load();
            if (deadline != 0 && esp_timer_get_time() > deadline && !runtime().ptt.load()) {
                g_listen_deadline_us.store(0);
                ESP_LOGI(kTag, "no speech within 8 s of the chime; ending the turn");
                voice_stop_turn();
            }
        }
        {
            const std::int64_t session_deadline = runtime().session_deadline_us.load();
            if (session_deadline != 0 && esp_timer_get_time() > session_deadline && g_connected.load()) {
                ESP_LOGW(kTag, "session hit 60 s cap; ending");
                runtime().session_deadline_us.store(0);
                g_listen_deadline_us.store(0);
                runtime().listening.store(false);
                g_want.store(false);
                close_realtime();
            }
        }
        flush_mic_to_xai();
        play_some();
        if (g_audio_failed.load()) {
            runtime().listening.store(false);
            g_want.store(false);
            close_realtime();
        }
        if (g_turn_done.load() && !tool_pending() && !pcm_pending() && !M5.Speaker.isPlaying()) {
            g_turn_done.store(false);
            mark_played_if_spoken();
            if (!runtime().ptt.load()) {
                runtime().listening.store(false);
                if (!tool_pending()) g_want.store(false);
            }
        }
        if (!g_want.load() && !runtime().ptt.load() && !tool_pending() && !pcm_pending() && !M5.Speaker.isPlaying()) {
            bool queued = false;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                queued = !g_speak.empty();
            }
            if (!queued && g_connected.load()) close_realtime();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ensure_voice_task()
{
    bool expected = false;
    if (g_started.compare_exchange_strong(expected, true)) {
        xTaskCreatePinnedToCore(voice_task, "voice", 8192, nullptr, 5, nullptr, 0);
    }
}
} // namespace

void speak_text(const std::string& text)
{
    speak_reply(text, "");
}

void speak_reply(const std::string& text, const std::string& request_id)
{
    if (text.empty() || text.size() > 2000) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_speak.size() >= 4) g_speak.pop_front();
        g_speak.push_back(SpeakItem{text, request_id});
    }
    g_want.store(true);
    ensure_voice_task();
}

void voice_start_turn()
{
    if (g_want.load()) g_interrupt.store(true);
    runtime().listening.store(true);
    runtime().face.store(g_connected.load() ? FaceState::Listening : FaceState::Connecting);
    g_ack_beep.store(true);
    {
        std::lock_guard<std::mutex> lock(g_mic_send_mu);
        mic_ring_clear();
    }
    g_want.store(true);
    ensure_voice_task();
}

void voice_stop_turn()
{
    runtime().listening.store(false);
    if (!runtime().ptt.load()) g_want.store(false);
}

void voice_feed_pcm(const std::int16_t* pcm, std::size_t n)
{
    if (pcm == nullptr || n == 0) return;
    if (runtime().speaking.load()) return;
    if (!runtime().ptt.load() && !runtime().listening.load()) return;
    std::lock_guard<std::mutex> lock(g_mic_send_mu);
    for (std::size_t i = 0; i < n; ++i) mic_ring_push(pcm[i]);
}

} // namespace jarvis
