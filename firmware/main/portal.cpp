// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jarvis contributors
//
// First-boot setup. The softAP name is the product name, not a user's network.
// UNTESTED: captive portal, DNS hijack, and BLE provisioning were not run here.

#include "link.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <cstdint>
#include <lwip/sockets.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

namespace jarvis {

namespace {
constexpr const char* kTag = "portal";
constexpr const char* kApSsid = "Jarvis-Setup";
bool g_stack_ready = false;
bool g_wifi_ready = false;

void ensure_stack()
{
    if (g_stack_ready) return;
    esp_netif_init();
    esp_event_loop_create_default();
    g_stack_ready = true;
}

std::string url_decode(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') out.push_back(' ');
        else if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi < 0 || lo < 0) out.push_back(in[i]);
            else {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            }
        } else out.push_back(in[i]);
    }
    return out;
}

std::string form_value(std::string_view body, std::string_view key)
{
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        const std::size_t amp = body.find('&', cursor);
        const std::string_view pair = body.substr(cursor, amp == std::string_view::npos ? body.size() - cursor : amp - cursor);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos && url_decode(pair.substr(0, eq)) == key) return url_decode(pair.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        cursor = amp + 1;
    }
    return {};
}

bool prefix_ok(const std::string& prefix)
{
    if (prefix.size() < 8 || prefix.size() > 64) return false;
    for (char c : prefix) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool save_wifi(const std::string& ssid, const std::string& pass)
{
    nvs_handle_t handle;
    if (nvs_open("jarvis", NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = nvs_set_str(handle, "wifi_ssid", ssid.c_str()) == ESP_OK &&
                    nvs_set_str(handle, "wifi_pass", pass.c_str()) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool load_wifi(std::string& ssid, std::string& pass)
{
    nvs_handle_t handle;
    if (nvs_open("jarvis", NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t ssid_len = 0;
    std::size_t pass_len = 0;
    if (nvs_get_str(handle, "wifi_ssid", nullptr, &ssid_len) != ESP_OK || ssid_len == 0) {
        nvs_close(handle);
        return false;
    }
    ssid.assign(ssid_len, '\0');
    nvs_get_str(handle, "wifi_ssid", ssid.data(), &ssid_len);
    if (!ssid.empty() && ssid.back() == '\0') ssid.pop_back();
    pass.clear();
    if (nvs_get_str(handle, "wifi_pass", nullptr, &pass_len) == ESP_OK && pass_len > 0) {
        pass.assign(pass_len, '\0');
        nvs_get_str(handle, "wifi_pass", pass.data(), &pass_len);
        if (!pass.empty() && pass.back() == '\0') pass.pop_back();
    }
    nvs_close(handle);
    return !ssid.empty();
}

esp_err_t page_get(httpd_req_t* req)
{
    static constexpr char kPage[] =
        "<!doctype html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Jarvis setup</title></head><body>"
        "<h1>Jarvis setup</h1>"
        "<p>This page stores Wi-Fi and the desk bridge on the robot. Nothing here is written into the firmware.</p>"
        "<form method=post action=/save>"
        "<label>Wi-Fi name<br><input name=ssid required maxlength=32></label><br>"
        "<label>Wi-Fi password<br><input name=pass type=password maxlength=64></label><br>"
        "<label>Worker URL<br><input name=worker_base required placeholder=https://your-worker.example></label><br>"
        "<label>Path prefix<br><input name=path_prefix required minlength=8></label><br>"
        "<label>Device id<br><input name=device_id required></label><br>"
        "<label>Device token<br><input name=device_token type=password required></label><br>"
        "<label>HMAC key<br><input name=hmac_key type=password required></label><br>"
        "<button type=submit>Save</button></form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kPage, sizeof(kPage) - 1);
}

esp_err_t save_post(httpd_req_t* req)
{
    if (req->content_len <= 0 || req->content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form too large");
        return ESP_OK;
    }
    std::string body(static_cast<std::size_t>(req->content_len), '\0');
    int received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, body.data() + received, static_cast<std::size_t>(req->content_len - received));
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete form");
            return ESP_OK;
        }
        received += n;
    }
    DeviceConfig cfg;
    cfg.worker_base = form_value(body, "worker_base");
    cfg.path_prefix = form_value(body, "path_prefix");
    cfg.device_id = form_value(body, "device_id");
    cfg.device_token = form_value(body, "device_token");
    cfg.hmac_key = form_value(body, "hmac_key");
    const std::string ssid = form_value(body, "ssid");
    const std::string pass = form_value(body, "pass");
    while (!cfg.worker_base.empty() && cfg.worker_base.back() == '/') cfg.worker_base.pop_back();
    const bool base_ok = cfg.worker_base.rfind("https://", 0) == 0 && cfg.worker_base.find('@') == std::string::npos &&
                         cfg.worker_base.find('/', 8) == std::string::npos;
    if (!base_ok || !prefix_ok(cfg.path_prefix) || cfg.device_id.empty() || cfg.device_id.size() > 64 ||
        cfg.device_token.size() < 16 || cfg.hmac_key.size() < 16 || ssid.empty() || ssid.size() > 32) {
        httpd_resp_send(req, "Check the Worker URL, path prefix, and that the token and HMAC key are filled in.", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!save_wifi(ssid, pass) || !config_save(cfg)) {
        httpd_resp_send(req, "The robot could not store setup.", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI(kTag, "setup stored");
    httpd_resp_send(req, "Saved. The robot is restarting.", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

esp_err_t redirect_get(httpd_req_t* req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, "", 0);
}

void dns_task(void*)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(kTag, "dns socket failed");
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(kTag, "dns bind failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }
    std::uint8_t buf[512];
    ESP_LOGI(kTag, "dns hijack up");
    for (;;) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 12 || n > 400) continue;
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = buf[4];
        buf[7] = buf[5];
        const int query_end = n;
        if (query_end + 16 > static_cast<int>(sizeof(buf))) continue;
        buf[query_end] = 0xC0;
        buf[query_end + 1] = 0x0C;
        buf[query_end + 2] = 0x00;
        buf[query_end + 3] = 0x01;
        buf[query_end + 4] = 0x00;
        buf[query_end + 5] = 0x01;
        buf[query_end + 6] = 0x00;
        buf[query_end + 7] = 0x00;
        buf[query_end + 8] = 0x00;
        buf[query_end + 9] = 60;
        buf[query_end + 10] = 0x00;
        buf[query_end + 11] = 0x04;
        buf[query_end + 12] = 192;
        buf[query_end + 13] = 168;
        buf[query_end + 14] = 4;
        buf[query_end + 15] = 1;
        sendto(sock, buf, query_end + 16, 0, reinterpret_cast<sockaddr*>(&from), from_len);
    }
}

void on_prov(void*, esp_event_base_t, std::int32_t id, void*)
{
    if (id != WIFI_PROV_CRED_SUCCESS) return;
    wifi_config_t wifi = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi) != ESP_OK) return;
    wifi.sta.ssid[sizeof(wifi.sta.ssid) - 1] = 0;
    wifi.sta.password[sizeof(wifi.sta.password) - 1] = 0;
    if (wifi.sta.ssid[0] == 0) return;
    if (save_wifi(reinterpret_cast<char*>(wifi.sta.ssid), reinterpret_cast<char*>(wifi.sta.password))) {
        ESP_LOGI(kTag, "ble provisioning stored wifi");
    }
}

void start_ble()
{
    wifi_prov_mgr_config_t config = {};
    config.scheme = wifi_prov_scheme_ble;
    config.scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
    if (wifi_prov_mgr_init(config) != ESP_OK) {
        ESP_LOGW(kTag, "ble provisioning did not start");
        return;
    }
    std::uint8_t pop_raw[4];
    esp_fill_random(pop_raw, sizeof(pop_raw));
    char pop[9];
    std::snprintf(pop, sizeof(pop), "%02x%02x%02x%02x", pop_raw[0], pop_raw[1], pop_raw[2], pop_raw[3]);
    ESP_LOGI(kTag, "ble proof-of-possession for this boot: %s", pop);
    esp_event_handler_register(WIFI_PROV_EVENT, WIFI_PROV_CRED_SUCCESS, on_prov, nullptr);
    const esp_err_t err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, pop, "Jarvis", nullptr);
    if (err != ESP_OK) ESP_LOGW(kTag, "ble provisioning start failed");
}

void start_radio()
{
    ensure_stack();
    if (!g_wifi_ready) {
        esp_netif_create_default_wifi_ap();
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        g_wifi_ready = true;
    } else {
        esp_wifi_stop();
        esp_netif_create_default_wifi_ap();
    }
    wifi_config_t ap = {};
    std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), kApSsid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = static_cast<std::uint8_t>(std::strlen(kApSsid));
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
}
} // namespace

void portal_start()
{
    ESP_LOGW(kTag, "setup portal UNTESTED. Join the open network Jarvis-Setup, then open http://192.168.4.1/");
    start_radio();
    xTaskCreate(dns_task, "dns", 4096, nullptr, 3, nullptr);
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.uri_match_fn = httpd_uri_match_wildcard;
    http.stack_size = 8192;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &http) == ESP_OK) {
        httpd_uri_t root = {};
        root.uri = "/";
        root.method = HTTP_GET;
        root.handler = page_get;
        httpd_uri_t save = {};
        save.uri = "/save";
        save.method = HTTP_POST;
        save.handler = save_post;
        httpd_uri_t any = {};
        any.uri = "/*";
        any.method = HTTP_GET;
        any.handler = redirect_get;
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &save);
        httpd_register_uri_handler(server, &any);
    }
    start_ble();
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

namespace {
// Reconnect with backoff after a station drop: 1 s doubling to a 30 s cap, reset on a new IP.
esp_timer_handle_t g_reconnect_timer = nullptr;
std::uint32_t g_backoff_ms = 1000;
bool g_sta_handlers = false;

void reconnect_cb(void*)
{
    const esp_err_t err = esp_wifi_connect();
    ESP_LOGI(kTag, "wifi reconnect attempt (%s)", esp_err_to_name(err));
}

void sta_event(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
        ESP_LOGW(kTag, "wifi disconnected (reason %d, rssi %d); retry in %u ms", ev ? ev->reason : -1,
                 ev ? ev->rssi : 0, static_cast<unsigned>(g_backoff_ms));
        if (g_reconnect_timer != nullptr) {
            esp_timer_stop(g_reconnect_timer);
            esp_timer_start_once(g_reconnect_timer, static_cast<std::uint64_t>(g_backoff_ms) * 1000ULL);
        }
        g_backoff_ms = g_backoff_ms * 2 > 30000 ? 30000 : g_backoff_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_backoff_ms = 1000;
        wifi_ap_record_t info = {};
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) ESP_LOGI(kTag, "wifi up, rssi %d", info.rssi);
    }
}

void install_sta_handlers()
{
    if (g_sta_handlers) return;
    esp_timer_create_args_t args = {};
    args.callback = reconnect_cb;
    args.name = "wifi_reconn";
    esp_timer_create(&args, &g_reconnect_timer);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, sta_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, sta_event, nullptr);
    g_sta_handlers = true;
}
} // namespace

bool wifi_join_saved()
{
    std::string ssid;
    std::string pass;
    if (!load_wifi(ssid, pass)) return false;
    ensure_stack();
    if (!g_wifi_ready) {
        { esp_netif_t* sta_if = esp_netif_create_default_wifi_sta(); if (sta_if != nullptr) esp_netif_set_hostname(sta_if, "Jarvis"); }
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&init) != ESP_OK) return false;
        g_wifi_ready = true;
    }
    wifi_config_t sta = {};
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), ssid.c_str(), sizeof(sta.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta.sta.password), pass.c_str(), sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK ||
        esp_wifi_start() != ESP_OK || esp_wifi_connect() != ESP_OK) {
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    install_sta_handlers();
    // UNTESTED. Wait for an address, then give up and let setup run again.
    for (int i = 0; i < 80; ++i) {
        wifi_ap_record_t info = {};
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGW(kTag, "wifi did not associate");
    return false;
}

} // namespace jarvis
