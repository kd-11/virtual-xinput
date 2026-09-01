#include "mapping.h"

#include <cmath>
#include <cstdio>

namespace vx {
namespace {

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

SHORT ToShort(float v) {
    v = Clamp(v, -1.0f, 1.0f);
    return (SHORT)(v * 32767.0f);
}

BYTE ToByte(float v) {
    v = Clamp(v, 0.0f, 1.0f);
    return (BYTE)(v * 255.0f + 0.5f);
}

bool Present(const DeviceCaps& c, DiAxis a) {
    int i = (int)a;
    return i >= 0 && i < kAxisCount && c.axisPresent[i];
}

// Reads a mapping as a bipolar stick value in -1..1.
float ReadStick(const AxisMapping& m, const RawState& raw) {
    if (m.kind == AxisMapping::Kind::Button) {
        if (m.button < 0 || m.button >= kMaxButtons) return 0.0f;
        float v = raw.button[m.button] ? 1.0f : 0.0f;
        return m.invert ? -v : v;
    }
    if (m.kind != AxisMapping::Kind::Axis) return 0.0f;

    int i = (int)m.axis;
    if (i < 0 || i >= kAxisCount) return 0.0f;

    float v = raw.axis[i];
    if (m.invert) v = -v;

    switch (m.half) {
        case AxisHalf::Positive: return v > 0.0f ? v : 0.0f;
        case AxisHalf::Negative: return v < 0.0f ? -v : 0.0f;
        default:                 return v;
    }
}

// Reads a mapping as a unipolar trigger value in 0..1. A full axis rests at its
// minimum, so it is rebased rather than centred.
float ReadTrigger(const AxisMapping& m, const RawState& raw) {
    if (m.kind == AxisMapping::Kind::Button) {
        if (m.button < 0 || m.button >= kMaxButtons) return 0.0f;
        return raw.button[m.button] ? 1.0f : 0.0f;
    }
    if (m.kind != AxisMapping::Kind::Axis) return 0.0f;

    int i = (int)m.axis;
    if (i < 0 || i >= kAxisCount) return 0.0f;

    float v = raw.axis[i];
    if (m.invert) v = -v;

    switch (m.half) {
        case AxisHalf::Positive: return v > 0.0f ? v : 0.0f;
        case AxisHalf::Negative: return v < 0.0f ? -v : 0.0f;
        default:                 return (v + 1.0f) * 0.5f;
    }
}

bool ReadButton(const ButtonMapping& m, const RawState& raw) {
    switch (m.kind) {
        case ButtonMapping::Kind::Button:
            if (m.button < 0 || m.button >= kMaxButtons) return false;
            return raw.button[m.button];

        case ButtonMapping::Kind::Axis: {
            int i = (int)m.axis;
            if (i < 0 || i >= kAxisCount) return false;
            float v = raw.axis[i];
            return m.axisPositive ? (v >= m.axisThreshold) : (v <= -m.axisThreshold);
        }

        case ButtonMapping::Kind::Pov: {
            if (m.pov < 0 || m.pov >= kMaxPovs) return false;
            WORD mask = PovToMask(raw.pov[m.pov]);
            return (mask & m.povMask) == m.povMask && m.povMask != 0;
        }

        default:
            return false;
    }
}

// Radial deadzone: kills drift in every direction equally, then rescales what
// is left so the stick still reaches full deflection.
void ApplyStickDeadzone(float x, float y, float dz, float dzMax, SHORT& outX, SHORT& outY) {
    float mag = sqrtf(x * x + y * y);
    if (mag <= dz || mag <= 0.0f) { outX = 0; outY = 0; return; }

    float scale = (mag >= dzMax) ? 1.0f : (mag - dz) / (dzMax - dz);
    float nx    = x / mag;
    float ny    = y / mag;

    outX = ToShort(nx * scale);
    outY = ToShort(ny * scale);
}

BYTE ApplyTriggerDeadzone(float v, float dz, float dzMax) {
    if (v <= dz) return 0;
    float scale = (v >= dzMax) ? 1.0f : (v - dz) / (dzMax - dz);
    return ToByte(scale);
}

void SetAxisIfUnset(DeviceProfile& p, int slot, DiAxis axis, bool invert, AxisHalf half) {
    if (p.axes[slot].IsSet()) return;
    p.axes[slot].kind   = AxisMapping::Kind::Axis;
    p.axes[slot].axis   = axis;
    p.axes[slot].invert = invert;
    p.axes[slot].half   = half;
}

const char* HalfSuffix(AxisHalf h) {
    switch (h) {
        case AxisHalf::Positive: return "+";
        case AxisHalf::Negative: return "-";
        default:                 return "";
    }
}

} // namespace

WORD PovToMask(int pov) {
    if (pov < 0) return 0;

    int deg = (pov / 100) % 360;
    if (deg < 0) deg += 360;

    WORD m = 0;
    if (deg > 270 || deg < 90)   m |= VX_GAMEPAD_DPAD_UP;
    if (deg > 0   && deg < 180)  m |= VX_GAMEPAD_DPAD_RIGHT;
    if (deg > 90  && deg < 270)  m |= VX_GAMEPAD_DPAD_DOWN;
    if (deg > 180 && deg < 360)  m |= VX_GAMEPAD_DPAD_LEFT;
    return m;
}

void BuildAutoProfile(const DeviceCaps& caps, DeviceProfile& p) {
    // Left stick is the one thing every DirectInput pad agrees on. DirectInput
    // Y grows downward, XInput Y grows upward, hence the inversion.
    if (p.autoAxes) {
        SetAxisIfUnset(p, XA_LeftX, DiAxis::X, false, AxisHalf::Full);
        SetAxisIfUnset(p, XA_LeftY, DiAxis::Y, true,  AxisHalf::Full);

        // Right stick: pick the first layout the device can actually satisfy.
        // These three cover the overwhelming majority of PC pads.
        bool rightDone = p.axes[XA_RightX].IsSet() && p.axes[XA_RightY].IsSet();
        bool usedZ = false, usedRz = false, usedRxRy = false, usedSlider0 = false;

        if (!rightDone) {
            if (Present(caps, DiAxis::Z) && Present(caps, DiAxis::Rz)) {
                // DualShock-style and most generic pads.
                SetAxisIfUnset(p, XA_RightX, DiAxis::Z,  false, AxisHalf::Full);
                SetAxisIfUnset(p, XA_RightY, DiAxis::Rz, true,  AxisHalf::Full);
                usedZ = usedRz = true;
            } else if (Present(caps, DiAxis::Rx) && Present(caps, DiAxis::Ry)) {
                // Xbox-style pads seen through the generic HID driver.
                SetAxisIfUnset(p, XA_RightX, DiAxis::Rx, false, AxisHalf::Full);
                SetAxisIfUnset(p, XA_RightY, DiAxis::Ry, true,  AxisHalf::Full);
                usedRxRy = true;
            } else if (Present(caps, DiAxis::Rz) && Present(caps, DiAxis::Slider0)) {
                // Several Logitech pads land here.
                SetAxisIfUnset(p, XA_RightX, DiAxis::Rz,      false, AxisHalf::Full);
                SetAxisIfUnset(p, XA_RightY, DiAxis::Slider0, true,  AxisHalf::Full);
                usedRz = usedSlider0 = true;
            }
        }

        // Triggers from whatever axes the right stick did not claim.
        bool trigDone = p.axes[XA_LeftTrigger].IsSet() && p.axes[XA_RightTrigger].IsSet();
        if (!trigDone) {
            if (!usedRxRy && Present(caps, DiAxis::Rx) && Present(caps, DiAxis::Ry)) {
                SetAxisIfUnset(p, XA_LeftTrigger,  DiAxis::Rx, false, AxisHalf::Full);
                SetAxisIfUnset(p, XA_RightTrigger, DiAxis::Ry, false, AxisHalf::Full);
            } else if (!usedZ && Present(caps, DiAxis::Z)) {
                // One shared axis: it rests centred and each trigger pulls it a
                // different way, so each takes one half.
                SetAxisIfUnset(p, XA_LeftTrigger,  DiAxis::Z, false, AxisHalf::Positive);
                SetAxisIfUnset(p, XA_RightTrigger, DiAxis::Z, false, AxisHalf::Negative);
            } else if (!usedSlider0 && Present(caps, DiAxis::Slider0) &&
                       Present(caps, DiAxis::Slider1)) {
                SetAxisIfUnset(p, XA_LeftTrigger,  DiAxis::Slider0, false, AxisHalf::Full);
                SetAxisIfUnset(p, XA_RightTrigger, DiAxis::Slider1, false, AxisHalf::Full);
            }
        }
    }

    // The order below is what the large majority of generic HID pads report.
    // Devices that disagree (DualShock 4, notably) need the configurator.
    if (p.autoButtons) {
        const int kDefault[] = {
            XB_A, XB_B, XB_X, XB_Y,
            XB_LeftShoulder, XB_RightShoulder,
            XB_Back, XB_Start,
            XB_LeftThumb, XB_RightThumb,
        };
        for (int i = 0; i < (int)(sizeof(kDefault) / sizeof(kDefault[0])); ++i) {
            if (i >= caps.buttonCount) break;
            ButtonMapping& b = p.buttons[kDefault[i]];
            if (b.IsSet()) continue;
            b.kind   = ButtonMapping::Kind::Button;
            b.button = i;
        }
    }

    if (p.autoDpad && caps.povCount > 0) {
        const WORD masks[4] = {VX_GAMEPAD_DPAD_UP, VX_GAMEPAD_DPAD_DOWN,
                               VX_GAMEPAD_DPAD_LEFT, VX_GAMEPAD_DPAD_RIGHT};
        for (int i = 0; i < 4; ++i) {
            ButtonMapping& b = p.buttons[XB_DpadUp + i];
            if (b.IsSet()) continue;
            b.kind    = ButtonMapping::Kind::Pov;
            b.pov     = 0;
            b.povMask = masks[i];
        }
    }
}

void MapState(const DeviceProfile& p, const RawState& raw, XINPUT_GAMEPAD& out) {
    out.wButtons      = 0;
    out.bLeftTrigger  = 0;
    out.bRightTrigger = 0;
    out.sThumbLX = out.sThumbLY = out.sThumbRX = out.sThumbRY = 0;

    ApplyStickDeadzone(ReadStick(p.axes[XA_LeftX], raw),
                       ReadStick(p.axes[XA_LeftY], raw),
                       p.dz.leftStick, p.dz.leftStickMax,
                       out.sThumbLX, out.sThumbLY);

    ApplyStickDeadzone(ReadStick(p.axes[XA_RightX], raw),
                       ReadStick(p.axes[XA_RightY], raw),
                       p.dz.rightStick, p.dz.rightStickMax,
                       out.sThumbRX, out.sThumbRY);

    out.bLeftTrigger  = ApplyTriggerDeadzone(ReadTrigger(p.axes[XA_LeftTrigger], raw),
                                             p.dz.trigger, p.dz.triggerMax);
    out.bRightTrigger = ApplyTriggerDeadzone(ReadTrigger(p.axes[XA_RightTrigger], raw),
                                             p.dz.trigger, p.dz.triggerMax);

    for (int i = 0; i < XB_Count; ++i) {
        if (!p.buttons[i].IsSet()) continue;
        if (ReadButton(p.buttons[i], raw)) out.wButtons |= XButtonBit(i);
    }
}

std::string DescribeProfile(const DeviceProfile& p) {
    static const char* kAxisLabel[XA_Count] = {"LX", "LY", "RX", "RY", "LT", "RT"};
    static const char* kBtnLabel[XB_Count]  = {
        "A", "B", "X", "Y", "LB", "RB", "Back", "Start", "L3", "R3", "Guide",
        "Up", "Down", "Left", "Right"};

    std::string s = "axes[";
    for (int i = 0; i < XA_Count; ++i) {
        char buf[64];
        const AxisMapping& m = p.axes[i];
        if (m.kind == AxisMapping::Kind::Axis) {
            snprintf(buf, sizeof(buf), "%s%s=%s%s%s", i ? " " : "", kAxisLabel[i],
                     m.invert ? "-" : "", DiAxisName(m.axis), HalfSuffix(m.half));
        } else if (m.kind == AxisMapping::Kind::Button) {
            snprintf(buf, sizeof(buf), "%s%s=btn%d", i ? " " : "", kAxisLabel[i], m.button);
        } else {
            snprintf(buf, sizeof(buf), "%s%s=-", i ? " " : "", kAxisLabel[i]);
        }
        s += buf;
    }
    s += "] buttons[";

    bool first = true;
    for (int i = 0; i < XB_Count; ++i) {
        const ButtonMapping& m = p.buttons[i];
        if (!m.IsSet()) continue;
        char buf[64];
        if (m.kind == ButtonMapping::Kind::Button) {
            snprintf(buf, sizeof(buf), "%s%s=%d", first ? "" : " ", kBtnLabel[i], m.button);
        } else if (m.kind == ButtonMapping::Kind::Pov) {
            snprintf(buf, sizeof(buf), "%s%s=pov%d", first ? "" : " ", kBtnLabel[i], m.pov);
        } else {
            snprintf(buf, sizeof(buf), "%s%s=%s%s", first ? "" : " ", kBtnLabel[i],
                     DiAxisName(m.axis), m.axisPositive ? "+" : "-");
        }
        s += buf;
        first = false;
    }
    s += "]";
    return s;
}

} // namespace vx
