#include "config.h"

#include "xinput_defs.h"
#include "yaml.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace vx {
namespace {

std::string Lower(const std::string& in) {
    std::string s = in;
    for (size_t i = 0; i < s.size(); ++i) s[i] = (char)tolower((unsigned char)s[i]);
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) ++b;
    while (e > b && isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool AllDigits(const std::string& s, bool allowSign) {
    if (s.empty()) return false;
    size_t i = 0;
    if (allowSign && (s[0] == '-' || s[0] == '+')) i = 1;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

float Clamp01(double v) {
    if (v < 0.0) return 0.0f;
    if (v > 1.0) return 1.0f;
    return (float)v;
}

struct NameSlot { const char* name; int slot; };

// Accepts both the canonical names and the shorthands people reach for first.
const NameSlot kAxisNames[] = {
    {"left_x",        XA_LeftX},        {"lx", XA_LeftX},
    {"left_y",        XA_LeftY},        {"ly", XA_LeftY},
    {"right_x",       XA_RightX},       {"rx", XA_RightX},
    {"right_y",       XA_RightY},       {"ry", XA_RightY},
    {"left_trigger",  XA_LeftTrigger},  {"lt", XA_LeftTrigger},
    {"right_trigger", XA_RightTrigger}, {"rt", XA_RightTrigger},
};

const NameSlot kButtonNames[] = {
    {"a", XB_A}, {"b", XB_B}, {"x", XB_X}, {"y", XB_Y},
    {"left_shoulder",  XB_LeftShoulder},  {"lb", XB_LeftShoulder},  {"l1", XB_LeftShoulder},
    {"right_shoulder", XB_RightShoulder}, {"rb", XB_RightShoulder}, {"r1", XB_RightShoulder},
    {"back",   XB_Back},  {"select", XB_Back},  {"share", XB_Back},
    {"start",  XB_Start}, {"options", XB_Start},
    {"left_thumb",  XB_LeftThumb},  {"l3", XB_LeftThumb},  {"ls", XB_LeftThumb},
    {"right_thumb", XB_RightThumb}, {"r3", XB_RightThumb}, {"rs", XB_RightThumb},
    {"guide", XB_Guide}, {"home", XB_Guide}, {"xbox", XB_Guide},
    {"dpad_up",    XB_DpadUp},    {"up",    XB_DpadUp},
    {"dpad_down",  XB_DpadDown},  {"down",  XB_DpadDown},
    {"dpad_left",  XB_DpadLeft},  {"left",  XB_DpadLeft},
    {"dpad_right", XB_DpadRight}, {"right", XB_DpadRight},
};

int LookupSlot(const NameSlot* table, size_t count, const std::string& name) {
    for (size_t i = 0; i < count; ++i)
        if (name == table[i].name) return table[i].slot;
    return -1;
}

WORD PovMaskFromName(const std::string& name) {
    if (name == "up")        return VX_GAMEPAD_DPAD_UP;
    if (name == "down")      return VX_GAMEPAD_DPAD_DOWN;
    if (name == "left")      return VX_GAMEPAD_DPAD_LEFT;
    if (name == "right")     return VX_GAMEPAD_DPAD_RIGHT;
    if (name == "upleft"   || name == "up_left")    return VX_GAMEPAD_DPAD_UP   | VX_GAMEPAD_DPAD_LEFT;
    if (name == "upright"  || name == "up_right")   return VX_GAMEPAD_DPAD_UP   | VX_GAMEPAD_DPAD_RIGHT;
    if (name == "downleft" || name == "down_left")  return VX_GAMEPAD_DPAD_DOWN | VX_GAMEPAD_DPAD_LEFT;
    if (name == "downright"|| name == "down_right") return VX_GAMEPAD_DPAD_DOWN | VX_GAMEPAD_DPAD_RIGHT;
    return 0;
}

void ParseDeadzone(const YamlNode& n, Deadzone& dz) {
    dz.leftStick     = Clamp01(n.Num("left_stick",      dz.leftStick));
    dz.rightStick    = Clamp01(n.Num("right_stick",     dz.rightStick));
    dz.leftStickMax  = Clamp01(n.Num("left_stick_max",  dz.leftStickMax));
    dz.rightStickMax = Clamp01(n.Num("right_stick_max", dz.rightStickMax));
    dz.trigger       = Clamp01(n.Num("trigger",         dz.trigger));
    dz.triggerMax    = Clamp01(n.Num("trigger_max",     dz.triggerMax));

    // A deadzone at or above the saturation point would divide by zero.
    if (dz.leftStickMax  <= dz.leftStick)  dz.leftStickMax  = dz.leftStick  + 0.01f;
    if (dz.rightStickMax <= dz.rightStick) dz.rightStickMax = dz.rightStick + 0.01f;
    if (dz.triggerMax    <= dz.trigger)    dz.triggerMax    = dz.trigger    + 0.01f;
}

bool ParseGuidString(const std::string& s, GUID& out) {
    std::string t = Trim(s);
    if (t.size() >= 2 && t.front() == '{' && t.back() == '}') t = t.substr(1, t.size() - 2);
    if (t.size() != 36) return false;

    wchar_t wide[64];
    std::wstring braced = L"{";
    for (size_t i = 0; i < t.size(); ++i) braced += (wchar_t)(unsigned char)t[i];
    braced += L"}";
    if (braced.size() >= 64) return false;
    wcscpy_s(wide, braced.c_str());

    return CLSIDFromString(wide, &out) == NOERROR;
}

} // namespace

// ---------------------------------------------------------------------------

const char* DiAxisName(DiAxis a) {
    switch (a) {
        case DiAxis::X:       return "x";
        case DiAxis::Y:       return "y";
        case DiAxis::Z:       return "z";
        case DiAxis::Rx:      return "rx";
        case DiAxis::Ry:      return "ry";
        case DiAxis::Rz:      return "rz";
        case DiAxis::Slider0: return "slider0";
        case DiAxis::Slider1: return "slider1";
        default:              return "none";
    }
}

DiAxis DiAxisFromName(const std::string& raw) {
    std::string n = Lower(Trim(raw));
    if (n == "x")  return DiAxis::X;
    if (n == "y")  return DiAxis::Y;
    if (n == "z")  return DiAxis::Z;
    if (n == "rx" || n == "rotx" || n == "xrot") return DiAxis::Rx;
    if (n == "ry" || n == "roty" || n == "yrot") return DiAxis::Ry;
    if (n == "rz" || n == "rotz" || n == "zrot") return DiAxis::Rz;
    if (n == "slider0" || n == "slider" || n == "s0") return DiAxis::Slider0;
    if (n == "slider1" || n == "s1")                  return DiAxis::Slider1;
    return DiAxis::None;
}

WORD XButtonBit(int slot) {
    switch (slot) {
        case XB_A:             return VX_GAMEPAD_A;
        case XB_B:             return VX_GAMEPAD_B;
        case XB_X:             return VX_GAMEPAD_X;
        case XB_Y:             return VX_GAMEPAD_Y;
        case XB_LeftShoulder:  return VX_GAMEPAD_LEFT_SHOULDER;
        case XB_RightShoulder: return VX_GAMEPAD_RIGHT_SHOULDER;
        case XB_Back:          return VX_GAMEPAD_BACK;
        case XB_Start:         return VX_GAMEPAD_START;
        case XB_LeftThumb:     return VX_GAMEPAD_LEFT_THUMB;
        case XB_RightThumb:    return VX_GAMEPAD_RIGHT_THUMB;
        case XB_Guide:         return VX_GAMEPAD_GUIDE;
        case XB_DpadUp:        return VX_GAMEPAD_DPAD_UP;
        case XB_DpadDown:      return VX_GAMEPAD_DPAD_DOWN;
        case XB_DpadLeft:      return VX_GAMEPAD_DPAD_LEFT;
        case XB_DpadRight:     return VX_GAMEPAD_DPAD_RIGHT;
        default:               return 0;
    }
}

bool ParseAxisSpec(const std::string& raw, AxisMapping& out) {
    out = AxisMapping();
    std::string s = Lower(Trim(raw));
    if (s.empty() || s == "none" || s == "-" || s == "null") return true;

    if (s.compare(0, 7, "button:") == 0) {
        std::string idx = Trim(s.substr(7));
        if (!AllDigits(idx, false)) return false;
        out.kind   = AxisMapping::Kind::Button;
        out.button = atoi(idx.c_str());
        return true;
    }

    if (s[0] == '-') { out.invert = true; s = s.substr(1); }
    else if (s[0] == '+') { s = s.substr(1); }
    if (s.empty()) return false;

    if (s.back() == '+') { out.half = AxisHalf::Positive; s.pop_back(); }
    else if (s.back() == '-') { out.half = AxisHalf::Negative; s.pop_back(); }
    if (s.empty()) return false;

    DiAxis a = DiAxisFromName(s);
    if (a == DiAxis::None) return false;

    out.kind = AxisMapping::Kind::Axis;
    out.axis = a;
    return true;
}

bool ParseButtonSpec(const std::string& raw, ButtonMapping& out) {
    out = ButtonMapping();
    std::string s = Lower(Trim(raw));
    if (s.empty() || s == "none" || s == "-1" || s == "null") return true;

    if (AllDigits(s, false)) {
        out.kind   = ButtonMapping::Kind::Button;
        out.button = atoi(s.c_str());
        return true;
    }

    if (s.compare(0, 7, "button:") == 0) {
        std::string idx = Trim(s.substr(7));
        if (!AllDigits(idx, false)) return false;
        out.kind   = ButtonMapping::Kind::Button;
        out.button = atoi(idx.c_str());
        return true;
    }

    if (s.compare(0, 5, "axis:") == 0) {
        std::string spec = Trim(s.substr(5));
        float       thr  = 0.5f;
        size_t      at   = spec.find('@');
        if (at != std::string::npos) {
            thr  = Clamp01(atof(spec.c_str() + at + 1));
            spec = Trim(spec.substr(0, at));
        }
        bool positive = true;
        if (!spec.empty() && (spec.back() == '+' || spec.back() == '-')) {
            positive = (spec.back() == '+');
            spec.pop_back();
        }
        DiAxis a = DiAxisFromName(spec);
        if (a == DiAxis::None) return false;

        out.kind          = ButtonMapping::Kind::Axis;
        out.axis          = a;
        out.axisPositive  = positive;
        out.axisThreshold = thr;
        return true;
    }

    if (s.compare(0, 3, "pov") == 0) {
        size_t colon = s.find(':');
        if (colon == std::string::npos) return false;
        std::string idx = Trim(s.substr(3, colon - 3));
        std::string dir = Trim(s.substr(colon + 1));
        WORD        m   = PovMaskFromName(dir);
        if (m == 0) return false;

        out.kind    = ButtonMapping::Kind::Pov;
        out.pov     = idx.empty() ? 0 : atoi(idx.c_str());
        out.povMask = m;
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------

namespace {

bool ParseDevice(const YamlNode& n, DeviceProfile& p, std::string& err) {
    p.match = n.Str("match", n.Str("name", "*"));

    std::string guidStr = n.Str("guid", "");
    if (!guidStr.empty()) {
        if (!ParseGuidString(guidStr, p.guid)) {
            err = "device '" + p.match + "': bad guid '" + guidStr + "'";
            return false;
        }
        p.hasGuid = true;
    }

    p.slot = n.Int("slot", -1);
    if (p.slot < -1 || p.slot >= VX_USER_MAX_COUNT) {
        err = "device '" + p.match + "': slot must be 0.." +
              std::to_string(VX_USER_MAX_COUNT - 1);
        return false;
    }

    p.rumble     = n.Bool("rumble", p.rumble);
    p.rumbleGain = Clamp01(n.Num("rumble_gain", p.rumbleGain));
    if (p.rumbleGain <= 0.0f) p.rumble = false;

    if (const YamlNode* dz = n.Find("deadzone")) ParseDeadzone(*dz, p.dz);

    if (const YamlNode* axes = n.Find("axes")) {
        if (axes->IsMap()) {
            p.autoAxes = false;
            for (size_t i = 0; i < axes->map.size(); ++i) {
                const std::string& key = axes->map[i].first;
                int slot = LookupSlot(kAxisNames, _countof(kAxisNames), Lower(key));
                if (slot < 0) { err = "unknown axis '" + key + "'"; return false; }
                if (!ParseAxisSpec(axes->map[i].second.scalar, p.axes[slot])) {
                    err = "bad axis spec for '" + key + "': " + axes->map[i].second.scalar;
                    return false;
                }
            }
        }
    }

    if (const YamlNode* btns = n.Find("buttons")) {
        if (btns->IsMap()) {
            p.autoButtons = false;
            for (size_t i = 0; i < btns->map.size(); ++i) {
                const std::string& key = btns->map[i].first;
                int slot = LookupSlot(kButtonNames, _countof(kButtonNames), Lower(key));
                if (slot < 0) { err = "unknown button '" + key + "'"; return false; }
                if (!ParseButtonSpec(btns->map[i].second.scalar, p.buttons[slot])) {
                    err = "bad button spec for '" + key + "': " + btns->map[i].second.scalar;
                    return false;
                }
                if (slot >= XB_DpadUp) p.autoDpad = false;
            }
        }
    }

    // `dpad: pov0` is shorthand for wiring all four directions to one hat.
    std::string dpad = Lower(n.Str("dpad", ""));
    if (!dpad.empty()) {
        p.autoDpad = false;
        if (dpad == "none") {
            for (int i = XB_DpadUp; i <= XB_DpadRight; ++i) p.buttons[i] = ButtonMapping();
        } else if (dpad.compare(0, 3, "pov") == 0) {
            int hat = (dpad.size() > 3) ? atoi(dpad.c_str() + 3) : 0;
            const WORD masks[4] = {VX_GAMEPAD_DPAD_UP, VX_GAMEPAD_DPAD_DOWN,
                                   VX_GAMEPAD_DPAD_LEFT, VX_GAMEPAD_DPAD_RIGHT};
            for (int i = 0; i < 4; ++i) {
                ButtonMapping& b = p.buttons[XB_DpadUp + i];
                b.kind    = ButtonMapping::Kind::Pov;
                b.pov     = hat;
                b.povMask = masks[i];
            }
        } else {
            err = "bad dpad value '" + dpad + "' (expected pov0..pov3 or none)";
            return false;
        }
    }

    return true;
}

} // namespace

bool ConfigParse(const std::string& text, Config& cfg) {
    YamlNode    root;
    std::string err;
    if (!YamlParse(text, root, err)) {
        cfg.error = err;
        return false;
    }

    cfg.log    = root.Bool("log", cfg.log);
    cfg.pollHz = root.Int("poll_hz", cfg.pollHz);
    if (cfg.pollHz < 10)   cfg.pollHz = 10;
    if (cfg.pollHz > 1000) cfg.pollHz = 1000;

    // Deadzones and rumble may be set once at the top level as a default for
    // every device.
    Deadzone defaults;
    if (const YamlNode* dz = root.Find("deadzone")) ParseDeadzone(*dz, defaults);

    DeviceProfile base;
    base.rumble     = root.Bool("rumble", base.rumble);
    base.rumbleGain = Clamp01(root.Num("rumble_gain", base.rumbleGain));

    const YamlNode* devices = root.Find("devices");
    if (devices && devices->IsSeq()) {
        for (size_t i = 0; i < devices->seq.size(); ++i) {
            DeviceProfile p    = base;
            p.dz               = defaults;
            if (!ParseDevice(devices->seq[i], p, cfg.error)) return false;
            cfg.devices.push_back(p);
        }
    }

    // A file that only sets deadzones still needs a device to apply them to.
    if (cfg.devices.empty()) {
        DeviceProfile p = base;
        p.dz            = defaults;
        cfg.devices.push_back(p);
    }

    return true;
}

Config ConfigLoad(const std::wstring& moduleDir, const std::wstring& exeDir) {
    Config cfg;

    const wchar_t* names[] = {L"virtual-xinput.yml", L"virtual-xinput.yaml"};
    std::wstring   dirs[]  = {moduleDir, exeDir};

    for (int d = 0; d < 2; ++d) {
        if (dirs[d].empty()) continue;
        if (d == 1 && dirs[1] == dirs[0]) continue;

        for (int n = 0; n < 2; ++n) {
            std::wstring path = dirs[d];
            if (!path.empty() && path.back() != L'\\') path += L'\\';
            path += names[n];

            std::string text;
            if (!ReadFileUtf8(path, text)) continue;

            cfg.fileFound = true;
            cfg.path      = path;
            if (!ConfigParse(text, cfg)) {
                // Keep the error for the log, but hand back usable defaults so a
                // typo in the config never stops the game seeing a controller.
                Config fallback;
                fallback.fileFound = true;
                fallback.path      = path;
                fallback.error     = cfg.error;
                fallback.devices.push_back(DeviceProfile());
                return fallback;
            }
            return cfg;
        }
    }

    cfg.devices.push_back(DeviceProfile());   // zero-config default
    return cfg;
}

} // namespace vx
