#pragma once

#include "config.h"
#include "xinput_defs.h"

namespace vx {

const int kMaxButtons = 128;
const int kMaxPovs    = 4;
const int kAxisCount  = (int)DiAxis::Count;

// What a device actually reports, discovered by enumerating its objects. Drives
// the auto-mapping heuristic.
struct DeviceCaps {
    bool axisPresent[kAxisCount];
    int  buttonCount;
    int  povCount;

    DeviceCaps() : buttonCount(0), povCount(0) {
        for (int i = 0; i < kAxisCount; ++i) axisPresent[i] = false;
    }
};

// One poll of a device, normalised so the mapping code never sees DirectInput
// types. Axes are -1..1 (we force every axis to a symmetric range at open
// time), POVs are -1 when centred or degrees*100 otherwise.
struct RawState {
    float axis[kAxisCount];
    int   pov[kMaxPovs];
    bool  button[kMaxButtons];

    RawState() {
        for (int i = 0; i < kAxisCount; ++i) axis[i] = 0.0f;
        for (int i = 0; i < kMaxPovs; ++i)   pov[i]  = -1;
        for (int i = 0; i < kMaxButtons; ++i) button[i] = false;
    }
};

// Fills any mapping the profile left unset, based on what the device reports.
// Explicit entries from the config file are never overwritten.
void BuildAutoProfile(const DeviceCaps& caps, DeviceProfile& profile);

// Translates one poll into an XInput gamepad, applying deadzones.
void MapState(const DeviceProfile& profile, const RawState& raw, XINPUT_GAMEPAD& out);

// Resolves the axis mappings to their values *before* deadzones are applied:
// sticks as -1..1, triggers as 0..1. The pad preview draws these alongside the
// deadzoned output, so what a deadzone is doing can be seen rather than
// inferred from a stick that mysteriously ignores small movements.
void ReadAxesRaw(const DeviceProfile& profile, const RawState& raw, float out[XA_Count]);

// Converts a POV reading into D-pad direction bits.
WORD PovToMask(int pov);

// Human-readable one-line summary of a profile, for the log and the
// configurator.
std::string DescribeProfile(const DeviceProfile& profile);

} // namespace vx
