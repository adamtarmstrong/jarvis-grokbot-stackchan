#pragma once

#include "jarvis/privacy.hpp"

namespace jarvis {

// MicGate's flag is set only after M5.Mic.begin() returns true, and cleared
// before any later work. Privacy dots read this object. They do not keep a copy.
class MicGate {
public:
    bool is_open() const { return open_; }
    bool open();
    void close();

private:
    bool open_ = false;
};

// The GC0308 is not started by this firmware (see docs/known_issues.md).
// open() fails and leaves the flag false, so the camera dot stays off.
class CameraGate {
public:
    bool is_open() const { return open_; }
    bool open();
    void close();

private:
    bool open_ = false;
};

DriverFlags driver_flags(const MicGate& mic, const CameraGate& camera);

MicGate& mic_gate();
CameraGate& camera_gate();

} // namespace jarvis
