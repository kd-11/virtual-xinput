#pragma once

#include <windows.h>
#include <objbase.h>   // GUID_NULL, CLSIDFromString
#include <string>
#include <vector>

namespace vx {

// ---------------------------------------------------------------------------
// DirectInput source axes, in DIJOYSTATE2 order.
// ---------------------------------------------------------------------------
enum class DiAxis { None = -1, X = 0, Y, Z, Rx, Ry, Rz, Slider0, Slider1, Count };

const char* DiAxisName(DiAxis a);
DiAxis      DiAxisFromName(const std::string& name);

// ---------------------------------------------------------------------------
// XInput destinations.
// ---------------------------------------------------------------------------
enum XAxisSlot {
    XA_LeftX = 0, XA_LeftY, XA_RightX, XA_RightY, XA_LeftTrigger, XA_RightTrigger,
    XA_Count
};

enum XButtonSlot {
    XB_A = 0, XB_B, XB_X, XB_Y,
    XB_LeftShoulder, XB_RightShoulder,
    XB_Back, XB_Start,
    XB_LeftThumb, XB_RightThumb,
    XB_Guide,
    XB_DpadUp, XB_DpadDown, XB_DpadLeft, XB_DpadRight,
    XB_Count
};

// wButtons bit for each XButtonSlot.
WORD XButtonBit(int slot);

// ---------------------------------------------------------------------------
// Mapping descriptions
// ---------------------------------------------------------------------------

// Half-axis selection. A trigger pair often shares one DI axis (common on
// XInput-style HID pads, where Z rests at centre and each trigger pulls it one
// way), so a mapping can take just one half and rescale it to full range.
enum class AxisHalf { Full, Positive, Negative };

struct AxisMapping {
    enum class Kind { None, Axis, Button } kind = Kind::None;

    DiAxis   axis   = DiAxis::None;
    int      button = -1;            // Kind::Button - digital source, full deflection
    bool     invert = false;
    AxisHalf half   = AxisHalf::Full;

    bool IsSet() const { return kind != Kind::None; }
};

struct ButtonMapping {
    enum class Kind { None, Button, Axis, Pov } kind = Kind::None;

    int    button        = -1;       // Kind::Button
    DiAxis axis          = DiAxis::None;  // Kind::Axis
    bool   axisPositive  = true;
    float  axisThreshold = 0.5f;
    int    pov           = 0;        // Kind::Pov - hat index
    WORD   povMask       = 0;        // required D-pad direction bits

    bool IsSet() const { return kind != Kind::None; }
};

struct Deadzone {
    // Radial deadzone as a fraction of full stick deflection.
    float leftStick     = 0.15f;
    float rightStick    = 0.15f;
    // Saturation point: deflection at or beyond this reads as fully pushed.
    float leftStickMax  = 1.0f;
    float rightStickMax = 1.0f;
    // Triggers are one-dimensional, so a simple threshold.
    float trigger       = 0.10f;
    float triggerMax    = 1.0f;
};

struct DeviceProfile {
    std::string match = "*";         // case-insensitive substring of the product name
    GUID        guid = GUID_NULL;    // exact instance GUID, when given
    bool        hasGuid = false;

    int  slot = -1;                  // XInput slot 0..3; -1 = assign in enumeration order

    // Rumble is only ever attempted on devices that report force feedback, and
    // taking it requires exclusive access to the device, so it can be turned
    // off for a pad whose force feedback misbehaves.
    bool  rumble     = true;
    float rumbleGain = 1.0f;

    Deadzone      dz;
    AxisMapping   axes[XA_Count];
    ButtonMapping buttons[XB_Count];

    // When no axes/buttons block appears in the file we fill the gaps from the
    // device's own capabilities instead. Explicit entries always win.
    bool autoAxes    = true;
    bool autoButtons = true;
    bool autoDpad    = true;
};

struct Config {
    bool                       log     = false;
    int                        pollHz  = 250;
    std::vector<DeviceProfile> devices;

    // Diagnostics about where the settings came from.
    bool        fileFound = false;
    std::wstring path;
    std::string error;               // non-empty if the file failed to parse
};

// Parses YAML text into `cfg`. Returns false and sets cfg.error on a syntax
// error; the caller should then fall back to defaults.
bool ConfigParse(const std::string& text, Config& cfg);

// Looks for virtual-xinput.yml next to the module, then next to the running
// executable. A missing file is not an error: the caller gets defaults, which
// is what makes the DLL work as a zero-config drop-in.
Config ConfigLoad(const std::wstring& moduleDir, const std::wstring& exeDir);

// Parses one axis spec, e.g. "x", "-y", "z+", "button:6", "none".
bool ParseAxisSpec(const std::string& spec, AxisMapping& out);
// Parses one button spec, e.g. "3", "none", "axis:z+@0.5", "pov0:up".
bool ParseButtonSpec(const std::string& spec, ButtonMapping& out);

// The exact inverse of the two parsers above. Kept beside them so a change to
// the syntax cannot be made on one side only, and used both to write config
// files and to show a mapping in the configurators.
std::string AxisSpecString(const AxisMapping& m);
std::string ButtonSpecString(const ButtonMapping& m);

} // namespace vx
