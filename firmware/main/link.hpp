#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace jarvis {

struct DeviceConfig {
    std::string worker_base; // https://host, no path
    std::string path_prefix;
    std::string device_id;
    std::string device_token;
    std::string hmac_key;
    std::string ha_url; // ws://Pi-IP:10700/voice; nonempty selects LAN voice, never xAI
    std::string ha_key; // LAN bridge pairing key, stored only in NVS
    // Reserved settings hooks. Camera/tracking and palette application are not enabled yet.
    std::string follow_enabled = "1";
    std::string follow_days = "1234567"; // Monday=1 ... Sunday=7
    std::string follow_start = "08:00";
    std::string follow_end = "17:00";
    std::string robot_color = "white";
    std::string ota_url;
};

bool config_load(DeviceConfig& out);
bool config_save(const DeviceConfig& cfg);
bool config_is_ready();

// HMAC-SHA256 of the JARVIS1 canonical string. Matches worker/src/crypto.ts.
std::string sign_request(const DeviceConfig& cfg, const char* method, const std::string& path,
                         const std::uint8_t* body, std::size_t len, std::string& timestamp, std::string& nonce);

bool worker_post(const DeviceConfig& cfg, const char* route, const std::string& json, std::string& response);
void worker_ws_start(const DeviceConfig& cfg);
void worker_send_text(const std::string& json);
bool clock_sync();

void speak_text(const std::string& text);
// Speak a bot reply, then tell the Worker the device played that request_id.
void speak_reply(const std::string& text, const std::string& request_id);
void voice_start_turn();
void voice_stop_turn();
void voice_feed_pcm(const std::int16_t* pcm, std::size_t n);

void wake_start();
void portal_start();
bool wifi_join_saved();

} // namespace jarvis
