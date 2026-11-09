#include "gates.hpp"

#include <esp_log.h>
#include <M5Unified.h>

namespace jarvis {

namespace {
constexpr const char* kTag = "gates";
}

bool MicGate::open()
{
    if (open_) return true;
    M5.Speaker.end();
    vTaskDelay(pdMS_TO_TICKS(20));
    // M5Unified sets StackChan/CoreS3 magnification to 2. That is quiet for WakeNet at
    // desk distance (idle peaks stay <500; close speech historically needs ~6k–11k).
    // Raise gain before begin(); clamp stays inside Mic_Class.
    {
        auto cfg = M5.Mic.config();
        if (cfg.magnification < 8) {
            cfg.magnification = 8;
            M5.Mic.config(cfg);
            ESP_LOGI(kTag, "mic magnification set to %u", static_cast<unsigned>(cfg.magnification));
        }
    }
    if (!M5.Mic.begin()) {
        ESP_LOGW(kTag, "microphone did not start");
        open_ = false;
        return false;
    }
    open_ = true;
    return true;
}

void MicGate::close()
{
    if (!open_) return;
    M5.Mic.end();
    open_ = false;
}

bool CameraGate::open()
{
    // UNTESTED, and intentionally not implemented. groki-bot's CoreS3 notes say
    // the GC0308 and WakeNet together exhaust internal DMA. Starting the sensor
    // here would also be the only place allowed to set open_ = true.
    ESP_LOGW(kTag, "camera start is not in this build; the camera dot stays off");
    open_ = false;
    return false;
}

void CameraGate::close()
{
    open_ = false;
}

DriverFlags driver_flags(const MicGate& mic, const CameraGate& camera)
{
    return DriverFlags{mic.is_open(), camera.is_open()};
}

} // namespace jarvis
