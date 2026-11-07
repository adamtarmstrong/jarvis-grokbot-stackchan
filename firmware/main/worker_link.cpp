// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jarvis contributors
//
// UNTESTED on hardware. This file was not flashed to a CoreS3 in CI.
// The HMAC vector itself is locked by firmware/host/test_hmac.py and worker/test.

#include "link.hpp"

#include <ctime>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_websocket_client.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sys/time.h>

#include "jarvis/sign.hpp"

namespace jarvis {

namespace {
constexpr const char* kTag = "link";
constexpr const char* kNs = "jarvis";

DeviceConfig g_cfg;
bool g_cfg_ready = false;
std::mutex g_mu;
std::deque<std::string> g_outbox;
esp_websocket_client_handle_t g_client = nullptr;
std::string g_ws_accum;
std::string g_ota_url;
bool g_clock_set = false;

void run_pending_ota()
{
    std::string url;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        url.swap(g_ota_url);
    }
    if (url.empty()) return;
    ESP_LOGW(kTag, "OTA starting. UNTESTED on hardware; the URL is not logged");
    esp_http_client_config_t http = {};
    http.url = url.c_str();
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 20000;
    esp_https_ota_config_t ota = {};
    ota.http_config = &http;
    if (esp_https_ota(&ota) == ESP_OK) esp_restart();
    ESP_LOGE(kTag, "OTA failed");
}

std::string hex_encode(const std::uint8_t* data, std::size_t len)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (std::size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

std::string sha256_hex(const std::uint8_t* data, std::size_t len)
{
    std::uint8_t out[32];
    const unsigned char empty = 0;
    const unsigned char* input = data == nullptr ? &empty : data;
    const std::size_t n = data == nullptr ? 0 : len;
    if (mbedtls_sha256(input, n, out, 0) != 0) return {};
    return hex_encode(out, sizeof(out));
}

std::string hmac_sha256_hex(const std::string& key, const std::string& message)
{
    std::uint8_t out[32] = {};
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr || mbedtls_md_setup(&ctx, info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return {};
    }
    mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char*>(key.data()), key.size());
    mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(message.data()), message.size());
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return hex_encode(out, sizeof(out));
}

std::string random_nonce()
{
    std::uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    return hex_encode(raw, sizeof(raw));
}

bool nvs_get(nvs_handle_t handle, const char* key, std::string& out)
{
    std::size_t len = 0;
    esp_err_t err = nvs_get_str(handle, key, nullptr, &len);
    if (err != ESP_OK || len == 0) return false;
    std::string buf(len, '\0');
    err = nvs_get_str(handle, key, buf.data(), &len);
    if (err != ESP_OK) return false;
    if (!buf.empty() && buf.back() == '\0') buf.pop_back();
    out = std::move(buf);
    return !out.empty();
}

bool nvs_put(nvs_handle_t handle, const char* key, const std::string& value)
{
    return nvs_set_str(handle, key, value.c_str()) == ESP_OK;
}

std::string request_path(const DeviceConfig& cfg, const char* route)
{
    return "/" + cfg.path_prefix + route;
}

bool https_base(const std::string& base)
{
    return base.rfind("https://", 0) == 0 && base.find('@') == std::string::npos;
}

esp_err_t http_event(esp_http_client_event_t* evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr && evt->data != nullptr && evt->data_len > 0) {
        auto* body = static_cast<std::string*>(evt->user_data);
        if (body->size() + static_cast<std::size_t>(evt->data_len) <= 4096) {
            body->append(static_cast<const char*>(evt->data), static_cast<std::size_t>(evt->data_len));
        }
    }
    return ESP_OK;
}

void handle_worker_message(const std::string& payload)
{
    cJSON* root = cJSON_ParseWithLength(payload.c_str(), payload.size());
    if (root == nullptr) return;
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    if (std::strcmp(type->valuestring, "reply") == 0) {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(id) && cJSON_IsString(text)) {
            cJSON* ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "type", "state");
            cJSON_AddStringToObject(ack, "request_id", id->valuestring);
            cJSON_AddStringToObject(ack, "state", "received");
            char* printed = cJSON_PrintUnformatted(ack);
            cJSON_Delete(ack);
            if (printed != nullptr) {
                worker_send_text(printed);
                cJSON_free(printed);
            }
            // speak_text is defined in xai_voice.cpp. The reply text is not logged.
            speak_reply(text->valuestring, id->valuestring);
        }
    } else if (std::strcmp(type->valuestring, "state") == 0) {
        ESP_LOGI(kTag, "state event");
    } else if (std::strcmp(type->valuestring, "ota") == 0) {
        const cJSON* url = cJSON_GetObjectItemCaseSensitive(root, "url");
        if (cJSON_IsString(url)) {
            const std::string candidate = url->valuestring;
            if (candidate.rfind("https://", 0) == 0 && candidate.find('@') == std::string::npos && candidate.size() <= 200) {
                std::lock_guard<std::mutex> lock(g_mu);
                g_ota_url = candidate;
            } else {
                ESP_LOGW(kTag, "ota url rejected");
            }
        }
    } else if (std::strcmp(type->valuestring, "session_end") == 0) {
        ESP_LOGI(kTag, "session_end ack");
    }
    cJSON_Delete(root);
}

void ws_event(void*, esp_event_base_t, std::int32_t id, void* data)
{
    if (id != WEBSOCKET_EVENT_DATA || data == nullptr) return;
    const auto* evt = static_cast<esp_websocket_event_data_t*>(data);
    if (evt->op_code != 0x1 || evt->data_ptr == nullptr || evt->data_len <= 0) return;
    if (evt->payload_offset == 0) g_ws_accum.clear();
    if (g_ws_accum.size() + static_cast<std::size_t>(evt->data_len) > 8192) {
        g_ws_accum.clear();
        ESP_LOGW(kTag, "dropped an oversized websocket frame");
        return;
    }
    g_ws_accum.append(evt->data_ptr, static_cast<std::size_t>(evt->data_len));
    if (evt->payload_offset + evt->data_len >= evt->payload_len) {
        handle_worker_message(g_ws_accum);
        g_ws_accum.clear();
    }
}

void drain_outbox()
{
    while (g_client != nullptr && esp_websocket_client_is_connected(g_client)) {
        // Keep the head queued on disconnect or failed transmission. In particular,
        // losing session_end keeps the full ten-minute daily reservation charged.
        std::string msg;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_outbox.empty()) return;
            msg = g_outbox.front();
        }
        const int sent = esp_websocket_client_send_text(g_client, msg.c_str(), static_cast<int>(msg.size()), pdMS_TO_TICKS(1000));
        if (sent != static_cast<int>(msg.size())) return;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (!g_outbox.empty() && g_outbox.front() == msg) g_outbox.pop_front();
        }
    }
}

void link_task(void*)
{
    ESP_LOGW(kTag, "worker websocket task UNTESTED on hardware");
    for (;;) {
        if (!g_clock_set && !clock_sync()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        std::string timestamp;
        std::string nonce;
        const std::string path = request_path(g_cfg, "/ws");
        const std::string signature = sign_request(g_cfg, "GET", path, nullptr, 0, timestamp, nonce);
        if (signature.empty()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        // Kept for the life of this client. The driver may retain the pointers.
        static std::string headers;
        static std::string uri;
        headers = "Authorization: Bearer " + g_cfg.device_token + "\r\n" +
                  "X-Jarvis-Device-Id: " + g_cfg.device_id + "\r\n" +
                  "X-Jarvis-Timestamp: " + timestamp + "\r\n" +
                  "X-Jarvis-Nonce: " + nonce + "\r\n" +
                  "X-Jarvis-Signature: " + signature + "\r\n";
        uri = g_cfg.worker_base;
        if (uri.rfind("https://", 0) == 0) uri.replace(0, 8, "wss://");
        while (!uri.empty() && uri.back() == '/') uri.pop_back();
        uri += path;

        esp_websocket_client_config_t ws = {};
        ws.uri = uri.c_str();
        ws.headers = headers.c_str();
        ws.buffer_size = 8192;
        ws.crt_bundle_attach = esp_crt_bundle_attach;
        ws.network_timeout_ms = 10000;
        ws.reconnect_timeout_ms = 5000;
        g_client = esp_websocket_client_init(&ws);
        if (g_client == nullptr) {
            ESP_LOGE(kTag, "websocket init failed");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        esp_websocket_register_events(g_client, WEBSOCKET_EVENT_ANY, ws_event, nullptr);
        if (esp_websocket_client_start(g_client) != ESP_OK) {
            ESP_LOGE(kTag, "websocket start failed");
            esp_websocket_client_destroy(g_client);
            g_client = nullptr;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGI(kTag, "websocket connecting");
        for (int i = 0; i < 50 && !esp_websocket_client_is_connected(g_client); ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        while (g_client != nullptr && esp_websocket_client_is_connected(g_client)) {
            drain_outbox();
            run_pending_ota();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (g_client != nullptr) {
            esp_websocket_client_stop(g_client);
            esp_websocket_client_destroy(g_client);
            g_client = nullptr;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
} // namespace

bool config_load(DeviceConfig& out)
{
    nvs_handle_t handle;
    if (nvs_open(kNs, NVS_READONLY, &handle) != ESP_OK) return false;
    const bool ok = nvs_get(handle, "worker_base", out.worker_base) && nvs_get(handle, "path_prefix", out.path_prefix) &&
                    nvs_get(handle, "device_id", out.device_id) && nvs_get(handle, "device_token", out.device_token) &&
                    nvs_get(handle, "hmac_key", out.hmac_key);
    nvs_get(handle, "ota_url", out.ota_url);
    nvs_close(handle);
    return ok && https_base(out.worker_base);
}

bool config_save(const DeviceConfig& cfg)
{
    nvs_handle_t handle;
    if (nvs_open(kNs, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = nvs_put(handle, "worker_base", cfg.worker_base) && nvs_put(handle, "path_prefix", cfg.path_prefix) &&
                    nvs_put(handle, "device_id", cfg.device_id) && nvs_put(handle, "device_token", cfg.device_token) &&
                    nvs_put(handle, "hmac_key", cfg.hmac_key);
    if (!cfg.ota_url.empty()) nvs_put(handle, "ota_url", cfg.ota_url);
    const esp_err_t committed = nvs_commit(handle);
    nvs_close(handle);
    if (ok && committed == ESP_OK) {
        g_cfg = cfg;
        g_cfg_ready = true;
    }
    return ok && committed == ESP_OK;
}

bool config_is_ready()
{
    DeviceConfig cfg;
    if (!config_load(cfg)) return false;
    if (cfg.path_prefix.size() < 8 || cfg.path_prefix.find('/') != std::string::npos) return false;
    g_cfg = cfg;
    g_cfg_ready = true;
    // A reset can leave the Worker's session record live for its 10-minute TTL, which
    // answers /token with 429. This device owns that record, so end it once the link is up.
    worker_send_text("{\"type\":\"session_end\"}");
    return true;
}

std::string sign_request(const DeviceConfig& cfg, const char* method, const std::string& path, const std::uint8_t* body,
                         std::size_t len, std::string& timestamp, std::string& nonce)
{
    const std::time_t now = std::time(nullptr);
    // std::to_string, not "%lld": newlib nano printf drops 64-bit conversions and signed garbage times.
    timestamp = std::to_string(static_cast<long long>(now));
    if (timestamp.size() < 10) timestamp.insert(0, 10 - timestamp.size(), '0');
    nonce = random_nonce();
    const std::string hash = sha256_hex(body, len);
    const std::string message = canonical_string(method, path, cfg.device_id, timestamp, nonce, hash);
    return hmac_sha256_hex(cfg.hmac_key, message);
}

bool worker_post(const DeviceConfig& cfg, const char* route, const std::string& json, std::string& response)
{
    if (!https_base(cfg.worker_base)) return false;
    std::string timestamp;
    std::string nonce;
    const std::string path = request_path(cfg, route);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(json.data());
    const std::string signature = sign_request(cfg, "POST", path, bytes, json.size(), timestamp, nonce);
    if (signature.empty()) return false;
    std::string url = cfg.worker_base;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += path;

    response.clear();
    esp_http_client_config_t http = {};
    http.url = url.c_str();
    http.method = HTTP_METHOD_POST;
    http.timeout_ms = 10000;
    http.event_handler = http_event;
    http.user_data = &response;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (client == nullptr) return false;
    esp_http_client_set_header(client, "Authorization", ("Bearer " + cfg.device_token).c_str());
    esp_http_client_set_header(client, "X-Jarvis-Device-Id", cfg.device_id.c_str());
    esp_http_client_set_header(client, "X-Jarvis-Timestamp", timestamp.c_str());
    esp_http_client_set_header(client, "X-Jarvis-Nonce", nonce.c_str());
    esp_http_client_set_header(client, "X-Jarvis-Signature", signature.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json.c_str(), static_cast<int>(json.size()));
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(kTag, "POST %s status %d", route, status);
    return err == ESP_OK && status >= 200 && status < 300;
}

void worker_send_text(const std::string& json)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_outbox.size() >= 8) g_outbox.pop_front();
    g_outbox.push_back(json);
}

void worker_ws_start(const DeviceConfig& cfg)
{
    g_cfg = cfg;
    g_cfg_ready = true;
    xTaskCreatePinnedToCore(link_task, "link", 8192, nullptr, 4, nullptr, 0);
}

bool clock_sync()
{
    if (g_clock_set) return true;
    // Init SNTP once; later calls only wait again (re-init fails with "already initialized").
    static bool sntp_started = false;
    if (!sntp_started) {
        esp_sntp_config_t config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3, ESP_SNTP_SERVER_LIST("time.cloudflare.com", "time.google.com", "pool.ntp.org"));
        if (esp_netif_sntp_init(&config) != ESP_OK) return false;
        sntp_started = true;
    }
    const esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    g_clock_set = err == ESP_OK || std::time(nullptr) > 1767225600; // after 2026-01-01
    if (g_clock_set) {
        const std::time_t now = std::time(nullptr);
        std::tm utc = {};
        gmtime_r(&now, &utc);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &utc);
        ESP_LOGI(kTag, "clock synced: %s", buf);
    }
    if (!g_clock_set) ESP_LOGW(kTag, "SNTP did not sync; signatures will be rejected");
    return g_clock_set;
}

} // namespace jarvis
