#pragma once

#include <cstdint>

namespace jarvis {

// Screen and LED indicators are derived from driver flags. There is no
// separate "show the dot" switch, so a dot cannot be off while the driver is open.
struct DriverFlags {
    bool mic_stream_open = false;
    bool camera_stream_open = false;
};

struct PrivacyDots {
    bool mic = false;
    bool camera = false;
};

inline PrivacyDots dots_from_drivers(DriverFlags flags) noexcept
{
    return PrivacyDots{flags.mic_stream_open, flags.camera_stream_open};
}

// Jarvis accent is brass on a near-black face. The other ten colors are ours.
enum class Accent : std::uint8_t {
    Brass = 0,
    Ivory,
    SignalRed,
    IonBlue,
    Mint,
    Violet,
    Coral,
    Cyan,
    Rose,
    Lime,
    Slate,
    Count
};

struct Rgb {
    std::uint8_t r, g, b;
};

inline constexpr Rgb kAccents[static_cast<int>(Accent::Count)] = {
    {224, 161, 6},   // brass, the Jarvis default
    {244, 239, 230}, // ivory
    {226, 61, 61},   // signal red, also the mic dot
    {61, 126, 255},  // ion blue
    {61, 220, 151},  // mint, also the camera dot
    {139, 108, 255}, // violet
    {255, 107, 74},  // coral
    {46, 196, 214},  // cyan
    {255, 92, 138},  // rose
    {182, 224, 61},  // lime
    {138, 160, 180}, // slate
};

// Face theme matches the Grok Bot avatar: black oval eyes on red (#FC2D3E).
inline constexpr Rgb kFaceBackground{252, 45, 62};
inline constexpr Rgb kEyeColor{0, 0, 0};
// On-screen dots get a black ring so the red mic dot stays visible on the red face.
inline constexpr Rgb kDotRing{0, 0, 0};
inline constexpr Rgb kMicDotScreen{255, 255, 255};
inline constexpr Rgb kMicDot = kAccents[static_cast<int>(Accent::SignalRed)];
inline constexpr Rgb kCameraDot = kAccents[static_cast<int>(Accent::Mint)];

} // namespace jarvis
