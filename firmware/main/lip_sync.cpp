#include "runtime.hpp"

#include <cmath>
#include <cstdint>

#include <esp_err.h>

#include "esp_dsp.h"

namespace jarvis {

namespace {
constexpr std::size_t kN = 256;
bool g_ready = false;

float energy(const std::int16_t* pcm, std::size_t n)
{
    // Voice-band estimate. The full groki-bot FFT task is in
    // third_party/groki-bot/mic_lip_sync_task.cpp. This port uses the same
    // 256-sample chunk and esp-dsp's real FFT, then keeps bins 5..48.
    // UNTESTED on the CoreS3 microphone.
    if (!g_ready) {
        if (dsps_fft2r_init_fc32(nullptr, kN) != ESP_OK) return 0.0f;
        g_ready = true;
    }
    float io[kN * 2];
    const std::size_t count = n < kN ? n : kN;
    for (std::size_t i = 0; i < kN; ++i) {
        const float sample = i < count ? static_cast<float>(pcm[i]) / 32768.0f : 0.0f;
        const float window = 0.5f * (1.0f - std::cos(6.2831853f * static_cast<float>(i) / kN));
        io[i * 2] = sample * window;
        io[i * 2 + 1] = 0.0f;
    }
    dsps_fft2r_fc32(io, kN);
    dsps_bit_rev_fc32(io, kN);
    float sum = 0.0f;
    for (std::size_t bin = 5; bin <= 48; ++bin) {
        const float re = io[bin * 2];
        const float im = io[bin * 2 + 1];
        sum += std::sqrt(re * re + im * im);
    }
    const float level = sum / 44.0f;
    return level > 1.0f ? 1.0f : level;
}
} // namespace

void lip_feed(const std::int16_t* pcm, std::size_t n)
{
    if (!runtime().speaking.load()) {
        runtime().lip_milli.store(0);
        return;
    }
    const int milli = static_cast<int>(energy(pcm, n) * 1000.0f);
    runtime().lip_milli.store(milli > 1000 ? 1000 : milli);
}

} // namespace jarvis
