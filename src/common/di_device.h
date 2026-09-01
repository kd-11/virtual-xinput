#pragma once

#define DIRECTINPUT_VERSION 0x0800

#include <windows.h>
#include <dinput.h>

#include <string>
#include <vector>

#include "mapping.h"
#include "strutil.h"

namespace vx {

struct DeviceInfo {
    GUID         instanceGuid;
    GUID         productGuid;
    std::wstring productName;
    std::wstring instanceName;
};

class DiSystem;

// One opened DirectInput device, polled through the DIJOYSTATE2 format.
class DiDevice {
public:
    DiDevice();
    ~DiDevice();

    void Close();
    bool IsOpen() const { return device_ != nullptr; }

    // Reads one sample. Re-acquires transparently when the device is lost (the
    // game alt-tabbing, the device sleeping) and reports false only when no
    // fresh data could be obtained this tick.
    bool Poll(RawState& out);

    const DeviceCaps&  Caps() const { return caps_; }
    const DeviceInfo&  Info() const { return info_; }

    // ---- Force feedback -------------------------------------------------
    // The device advertises force feedback.
    bool ForceFeedbackCapable() const { return ffCapable_; }
    // Effects were created successfully and rumble can actually be played.
    bool ForceFeedbackReady() const { return ffEffectCount_ > 0; }
    // Number of independently driveable actuators found (1 or 2).
    int  ForceFeedbackAxisCount() const { return ffEffectCount_; }
    // Description of the effect type in use, for logs and the configurator.
    const char* ForceFeedbackKind() const;

    // Sets motor strength, 0..1 each. On a single-actuator device the stronger
    // of the two wins. Must be called from the thread that owns this device.
    void SetRumble(float left, float right);
    void StopRumble();

private:
    friend class DiSystem;

    bool Open(IDirectInput8W* di, const DeviceInfo& info, HWND hwnd, bool wantForceFeedback);
    void DiscoverCaps();
    bool CreateEffects();
    void ReleaseEffects();
    bool ApplyMagnitude(int index, float value);

    static BOOL CALLBACK EnumObjectsCb(LPCDIDEVICEOBJECTINSTANCEW obj, LPVOID ctx);
    static BOOL CALLBACK EnumFfAxesCb(LPCDIDEVICEOBJECTINSTANCEW obj, LPVOID ctx);

    IDirectInputDevice8W* device_;
    DeviceInfo            info_;
    DeviceCaps            caps_;
    bool                  acquired_;

    // DirectInput hands back a zeroed state for the first few reads after an
    // Acquire, before the device has actually reported. Zero is not neutral for
    // a trigger that rests at its minimum, so those samples are counted down
    // and discarded rather than published as a half-pulled trigger.
    int                   discardSamples_;

    // Actual reported range per axis. Devices are asked for -32768..32767 but
    // not all of them comply, so values are normalised against what they really
    // report rather than what we requested.
    LONG axisMin_[kAxisCount];
    LONG axisMax_[kAxisCount];

    // Force feedback state. Up to two effects: when the device exposes two
    // actuators they are driven separately as the strong and weak motor,
    // otherwise a single effect carries both.
    bool                ffCapable_;
    bool                ffWanted_;
    bool                ffPeriodic_;      // fell back to a periodic effect
    int                 ffEffectCount_;
    IDirectInputEffect* ffEffects_[2];
    DWORD               ffAxisOffsets_[2];
    int                 ffAxisFound_;
    float               ffLast_[2];
};

// Owns the IDirectInput8 instance and the hidden window used for cooperative
// level. Everything here must be used from a single thread.
class DiSystem {
public:
    DiSystem();
    ~DiSystem();

    bool Init();
    void Shutdown();
    bool IsReady() const { return di_ != nullptr; }

    // Attached game controllers, in DirectInput enumeration order.
    bool Enumerate(std::vector<DeviceInfo>& out);

    // Opens a device by instance GUID. Returns null on failure. Force feedback
    // requires exclusive acquisition, so it is only requested when asked for
    // and only taken when the device actually supports it.
    DiDevice* Open(const DeviceInfo& info, bool wantForceFeedback = false);

    // Pumps the hidden window's queue. Call periodically from the owning thread
    // so the window never looks hung to the system.
    void PumpMessages();

    const std::string& LastError() const { return lastError_; }

private:
    static BOOL CALLBACK EnumDevicesCb(LPCDIDEVICEINSTANCEW inst, LPVOID ctx);

    bool CreateHiddenWindow();

    IDirectInput8W* di_;
    HWND            hwnd_;
    HMODULE         dinputModule_;
    ATOM            wndClass_;
    std::string     lastError_;
};

// Case-insensitive substring test used to match config entries against product
// names. An empty or "*" pattern matches everything.
bool MatchesPattern(const std::wstring& name, const std::string& pattern);

} // namespace vx
