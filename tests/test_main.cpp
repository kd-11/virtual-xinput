// Self-tests for the parts that can be checked without a physical controller:
// the YAML subset parser, the config schema, the auto-mapping heuristic and the
// deadzone maths.

#include "../src/common/config.h"
#include "../src/common/di_device.h"
#include "../src/common/games.h"
#include "../src/common/mapping.h"
#include "../src/common/xinput_defs.h"
#include "../src/common/yaml.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace vx;

namespace {

int g_failures = 0;
int g_checks   = 0;

void Check(bool ok, const char* what, const char* detail = "") {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL: %s %s\n", what, detail);
    }
}

void CheckEqInt(long long got, long long want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        printf("  FAIL: %s (got %lld, want %lld)\n", what, got, want);
    }
}

void CheckNear(float got, float want, float tol, const char* what) {
    ++g_checks;
    if (fabsf(got - want) > tol) {
        ++g_failures;
        printf("  FAIL: %s (got %.4f, want %.4f)\n", what, got, want);
    }
}

void Section(const char* name) { printf("\n%s\n", name); }

// ---------------------------------------------------------------------------

void TestYamlBasics() {
    Section("YAML: scalars, nesting, comments");

    const char* text =
        "# leading comment\n"
        "log: true\n"
        "poll_hz: 125   # trailing comment\n"
        "deadzone:\n"
        "  left_stick: 0.2\n"
        "  right_stick: 0.25\n"
        "quoted: \"a: b # not a comment\"\n"
        "empty:\n";

    YamlNode    root;
    std::string err;
    Check(YamlParse(text, root, err), "parses", err.c_str());

    Check(root.Bool("log", false) == true, "log is true");
    CheckEqInt(root.Int("poll_hz", 0), 125, "poll_hz");
    Check(root.Str("quoted", "") == "a: b # not a comment", "quotes protect ':' and '#'");

    const YamlNode* dz = root.Find("deadzone");
    Check(dz != nullptr && dz->IsMap(), "deadzone is a nested map");
    if (dz) {
        CheckNear((float)dz->Num("left_stick", 0), 0.2f, 0.0001f, "nested left_stick");
        CheckNear((float)dz->Num("right_stick", 0), 0.25f, 0.0001f, "nested right_stick");
    }
}

void TestYamlSequences() {
    Section("YAML: sequences of maps");

    const char* text =
        "devices:\n"
        "  - match: \"Pad One\"\n"
        "    slot: 0\n"
        "    axes:\n"
        "      left_x: x\n"
        "      left_y: -y\n"
        "  - match: \"Pad Two\"\n"
        "    slot: 1\n";

    YamlNode    root;
    std::string err;
    Check(YamlParse(text, root, err), "parses", err.c_str());

    const YamlNode* devs = root.Find("devices");
    Check(devs != nullptr && devs->IsSeq(), "devices is a sequence");
    if (!devs || !devs->IsSeq()) return;

    CheckEqInt((long long)devs->seq.size(), 2, "two devices");
    Check(devs->seq[0].Str("match", "") == "Pad One", "first device name");
    CheckEqInt(devs->seq[0].Int("slot", -1), 0, "first device slot");
    Check(devs->seq[1].Str("match", "") == "Pad Two", "second device name");
    CheckEqInt(devs->seq[1].Int("slot", -1), 1, "second device slot");

    const YamlNode* axes = devs->seq[0].Find("axes");
    Check(axes != nullptr && axes->IsMap(), "nested axes map inside a seq item");
    if (axes) {
        Check(axes->Str("left_x", "") == "x", "left_x");
        Check(axes->Str("left_y", "") == "-y", "left_y");
    }
}

void TestSpecParsing() {
    Section("Spec strings");

    AxisMapping a;
    Check(ParseAxisSpec("x", a) && a.kind == AxisMapping::Kind::Axis &&
              a.axis == DiAxis::X && !a.invert && a.half == AxisHalf::Full,
          "plain axis 'x'");

    Check(ParseAxisSpec("-y", a) && a.axis == DiAxis::Y && a.invert, "inverted '-y'");
    Check(ParseAxisSpec("z+", a) && a.axis == DiAxis::Z && a.half == AxisHalf::Positive,
          "positive half 'z+'");
    Check(ParseAxisSpec("z-", a) && a.axis == DiAxis::Z && a.half == AxisHalf::Negative,
          "negative half 'z-'");
    Check(ParseAxisSpec("-rz+", a) && a.axis == DiAxis::Rz && a.invert &&
              a.half == AxisHalf::Positive, "inverted positive half '-rz+'");
    Check(ParseAxisSpec("button:6", a) && a.kind == AxisMapping::Kind::Button &&
              a.button == 6, "axis from button");
    Check(ParseAxisSpec("none", a) && !a.IsSet(), "'none' clears the mapping");
    Check(ParseAxisSpec("slider1", a) && a.axis == DiAxis::Slider1, "slider1");
    Check(!ParseAxisSpec("bogus", a), "unknown axis name is rejected");

    ButtonMapping b;
    Check(ParseButtonSpec("3", b) && b.kind == ButtonMapping::Kind::Button && b.button == 3,
          "bare button index");
    Check(ParseButtonSpec("none", b) && !b.IsSet(), "'none' button");
    Check(ParseButtonSpec("-1", b) && !b.IsSet(), "'-1' means unmapped");
    Check(ParseButtonSpec("pov0:up", b) && b.kind == ButtonMapping::Kind::Pov &&
              b.pov == 0 && b.povMask == VX_GAMEPAD_DPAD_UP, "pov direction");
    Check(ParseButtonSpec("pov1:downright", b) && b.pov == 1 &&
              b.povMask == (VX_GAMEPAD_DPAD_DOWN | VX_GAMEPAD_DPAD_RIGHT),
          "diagonal pov direction");
    Check(ParseButtonSpec("axis:z+@0.30", b) && b.kind == ButtonMapping::Kind::Axis &&
              b.axis == DiAxis::Z && b.axisPositive &&
              fabsf(b.axisThreshold - 0.30f) < 0.001f, "axis-as-button with threshold");
    Check(!ParseButtonSpec("pov0:sideways", b), "bad pov direction is rejected");
}

void TestConfigSchema() {
    Section("Config: full document");

    const char* text =
        "log: true\n"
        "poll_hz: 500\n"
        "devices:\n"
        "  - match: \"Wireless Controller\"\n"
        "    slot: 2\n"
        "    deadzone:\n"
        "      left_stick: 0.18\n"
        "      trigger: 0.05\n"
        "    axes:\n"
        "      left_x: x\n"
        "      left_y: -y\n"
        "      right_x: z\n"
        "      right_y: -rz\n"
        "      left_trigger: rx\n"
        "      right_trigger: ry\n"
        "    buttons:\n"
        "      a: 1\n"
        "      b: 2\n"
        "      x: 0\n"
        "      y: 3\n"
        "      guide: none\n"
        "    dpad: pov0\n";

    Config cfg;
    Check(ConfigParse(text, cfg), "parses", cfg.error.c_str());
    Check(cfg.log, "log enabled");
    CheckEqInt(cfg.pollHz, 500, "poll_hz");
    CheckEqInt((long long)cfg.devices.size(), 1, "one device");
    if (cfg.devices.empty()) return;

    const DeviceProfile& p = cfg.devices[0];
    Check(p.match == "Wireless Controller", "match string");
    CheckEqInt(p.slot, 2, "slot");
    CheckNear(p.dz.leftStick, 0.18f, 0.0001f, "left stick deadzone");
    CheckNear(p.dz.trigger, 0.05f, 0.0001f, "trigger deadzone");
    CheckNear(p.dz.rightStick, 0.15f, 0.0001f, "right stick keeps its default");

    Check(p.axes[XA_RightY].axis == DiAxis::Rz && p.axes[XA_RightY].invert, "right_y = -rz");
    Check(p.axes[XA_LeftTrigger].axis == DiAxis::Rx, "left_trigger = rx");
    CheckEqInt(p.buttons[XB_A].button, 1, "A -> button 1");
    CheckEqInt(p.buttons[XB_X].button, 0, "X -> button 0");
    Check(!p.buttons[XB_Guide].IsSet(), "guide explicitly unmapped");
    Check(p.buttons[XB_DpadLeft].kind == ButtonMapping::Kind::Pov &&
              p.buttons[XB_DpadLeft].povMask == VX_GAMEPAD_DPAD_LEFT,
          "dpad shorthand wires all four directions");

    // A deadzone above the saturation point would divide by zero downstream.
    const char* degenerate =
        "devices:\n"
        "  - match: \"*\"\n"
        "    deadzone:\n"
        "      left_stick: 0.9\n"
        "      left_stick_max: 0.5\n";
    Config cfg2;
    Check(ConfigParse(degenerate, cfg2), "degenerate deadzone parses", cfg2.error.c_str());
    Check(cfg2.devices[0].dz.leftStickMax > cfg2.devices[0].dz.leftStick,
          "saturation forced above deadzone");

    Config bad;
    Check(!ConfigParse("devices:\n  - match: \"x\"\n    axes:\n      left_x: nonsense\n", bad),
          "bad axis spec is reported");
    Check(!bad.error.empty(), "error message is set");
}

void TestPovMask() {
    Section("POV decoding");

    CheckEqInt(PovToMask(-1), 0, "centered");
    CheckEqInt(PovToMask(0), VX_GAMEPAD_DPAD_UP, "0 deg = up");
    CheckEqInt(PovToMask(4500), VX_GAMEPAD_DPAD_UP | VX_GAMEPAD_DPAD_RIGHT, "45 deg = up+right");
    CheckEqInt(PovToMask(9000), VX_GAMEPAD_DPAD_RIGHT, "90 deg = right");
    CheckEqInt(PovToMask(13500), VX_GAMEPAD_DPAD_DOWN | VX_GAMEPAD_DPAD_RIGHT, "135 deg");
    CheckEqInt(PovToMask(18000), VX_GAMEPAD_DPAD_DOWN, "180 deg = down");
    CheckEqInt(PovToMask(22500), VX_GAMEPAD_DPAD_DOWN | VX_GAMEPAD_DPAD_LEFT, "225 deg");
    CheckEqInt(PovToMask(27000), VX_GAMEPAD_DPAD_LEFT, "270 deg = left");
    CheckEqInt(PovToMask(31500), VX_GAMEPAD_DPAD_UP | VX_GAMEPAD_DPAD_LEFT, "315 deg");
}

DeviceProfile SimpleProfile() {
    DeviceProfile p;
    p.axes[XA_LeftX].kind = AxisMapping::Kind::Axis;
    p.axes[XA_LeftX].axis = DiAxis::X;
    p.axes[XA_LeftY].kind = AxisMapping::Kind::Axis;
    p.axes[XA_LeftY].axis = DiAxis::Y;
    p.axes[XA_LeftY].invert = true;
    return p;
}

void TestDeadzones() {
    Section("Deadzone maths");

    DeviceProfile p = SimpleProfile();
    p.dz.leftStick    = 0.15f;
    p.dz.leftStickMax = 1.0f;
    p.dz.trigger      = 0.10f;
    p.dz.triggerMax   = 1.0f;

    RawState       raw;
    XINPUT_GAMEPAD gp;

    // At rest everything reads zero.
    MapState(p, raw, gp);
    CheckEqInt(gp.sThumbLX, 0, "rest LX");
    CheckEqInt(gp.sThumbLY, 0, "rest LY");

    // Inside the deadzone stays zero.
    raw.axis[(int)DiAxis::X] = 0.10f;
    MapState(p, raw, gp);
    CheckEqInt(gp.sThumbLX, 0, "drift inside deadzone is suppressed");

    // Full deflection reaches the top of the range.
    raw.axis[(int)DiAxis::X] = 1.0f;
    MapState(p, raw, gp);
    CheckEqInt(gp.sThumbLX, 32767, "full right = 32767");

    // Just past the deadzone the output starts from zero, not from a jump.
    raw.axis[(int)DiAxis::X] = 0.16f;
    MapState(p, raw, gp);
    Check(gp.sThumbLX > 0 && gp.sThumbLX < 1200, "output ramps from zero past the deadzone");

    // Halfway between deadzone and saturation is half output.
    raw.axis[(int)DiAxis::X] = 0.575f;
    MapState(p, raw, gp);
    Check(abs(gp.sThumbLX - 16383) < 400, "midpoint is roughly half deflection");

    // Y is inverted: DirectInput down is XInput negative.
    raw.axis[(int)DiAxis::X] = 0.0f;
    raw.axis[(int)DiAxis::Y] = -1.0f;   // stick pushed up
    MapState(p, raw, gp);
    CheckEqInt(gp.sThumbLY, 32767, "stick up is positive Y");

    // The deadzone is radial, so a diagonal inside the radius is suppressed even
    // though neither component alone would be.
    raw = RawState();
    raw.axis[(int)DiAxis::X] = 0.10f;
    raw.axis[(int)DiAxis::Y] = -0.10f;
    MapState(p, raw, gp);
    CheckEqInt(gp.sThumbLX, 0, "diagonal inside radius: X suppressed");
    CheckEqInt(gp.sThumbLY, 0, "diagonal inside radius: Y suppressed");

    // A diagonal beyond the radius keeps its direction.
    raw.axis[(int)DiAxis::X] = 0.707f;
    raw.axis[(int)DiAxis::Y] = -0.707f;
    MapState(p, raw, gp);
    Check(gp.sThumbLX > 20000 && gp.sThumbLY > 20000, "diagonal push survives");
    Check(abs(gp.sThumbLX - gp.sThumbLY) < 200, "diagonal stays symmetric");
}

void TestTriggers() {
    Section("Trigger handling");

    DeviceProfile p;
    p.dz.trigger    = 0.10f;
    p.dz.triggerMax = 1.0f;

    // Full-axis trigger: rests at the minimum, travels to the maximum.
    p.axes[XA_LeftTrigger].kind = AxisMapping::Kind::Axis;
    p.axes[XA_LeftTrigger].axis = DiAxis::Rx;

    RawState       raw;
    XINPUT_GAMEPAD gp;

    raw.axis[(int)DiAxis::Rx] = -1.0f;   // released
    MapState(p, raw, gp);
    CheckEqInt(gp.bLeftTrigger, 0, "full-axis trigger released = 0");

    raw.axis[(int)DiAxis::Rx] = 1.0f;    // fully pressed
    MapState(p, raw, gp);
    CheckEqInt(gp.bLeftTrigger, 255, "full-axis trigger pressed = 255");

    // Split axis: both triggers share one axis that rests centred.
    DeviceProfile s;
    s.dz.trigger    = 0.10f;
    s.dz.triggerMax = 1.0f;
    s.axes[XA_LeftTrigger].kind  = AxisMapping::Kind::Axis;
    s.axes[XA_LeftTrigger].axis  = DiAxis::Z;
    s.axes[XA_LeftTrigger].half  = AxisHalf::Positive;
    s.axes[XA_RightTrigger].kind = AxisMapping::Kind::Axis;
    s.axes[XA_RightTrigger].axis = DiAxis::Z;
    s.axes[XA_RightTrigger].half = AxisHalf::Negative;

    RawState split;
    split.axis[(int)DiAxis::Z] = 0.0f;
    MapState(s, split, gp);
    CheckEqInt(gp.bLeftTrigger, 0, "split axis centred: LT = 0");
    CheckEqInt(gp.bRightTrigger, 0, "split axis centred: RT = 0");

    split.axis[(int)DiAxis::Z] = 1.0f;
    MapState(s, split, gp);
    CheckEqInt(gp.bLeftTrigger, 255, "split axis high: LT = 255");
    CheckEqInt(gp.bRightTrigger, 0, "split axis high: RT stays 0");

    split.axis[(int)DiAxis::Z] = -1.0f;
    MapState(s, split, gp);
    CheckEqInt(gp.bLeftTrigger, 0, "split axis low: LT stays 0");
    CheckEqInt(gp.bRightTrigger, 255, "split axis low: RT = 255");
}

void TestButtonsAndDpad() {
    Section("Buttons and D-pad");

    DeviceProfile p;
    p.buttons[XB_A].kind   = ButtonMapping::Kind::Button;
    p.buttons[XB_A].button = 0;
    p.buttons[XB_Start].kind   = ButtonMapping::Kind::Button;
    p.buttons[XB_Start].button = 7;
    p.buttons[XB_DpadUp].kind    = ButtonMapping::Kind::Pov;
    p.buttons[XB_DpadUp].pov     = 0;
    p.buttons[XB_DpadUp].povMask = VX_GAMEPAD_DPAD_UP;
    p.buttons[XB_DpadRight].kind    = ButtonMapping::Kind::Pov;
    p.buttons[XB_DpadRight].pov     = 0;
    p.buttons[XB_DpadRight].povMask = VX_GAMEPAD_DPAD_RIGHT;

    RawState       raw;
    XINPUT_GAMEPAD gp;

    MapState(p, raw, gp);
    CheckEqInt(gp.wButtons, 0, "nothing pressed");

    raw.button[0] = true;
    raw.button[7] = true;
    MapState(p, raw, gp);
    Check((gp.wButtons & VX_GAMEPAD_A) != 0, "A pressed");
    Check((gp.wButtons & VX_GAMEPAD_START) != 0, "Start pressed");
    Check((gp.wButtons & VX_GAMEPAD_B) == 0, "B not pressed");

    // A diagonal on the hat must light both directions at once.
    raw = RawState();
    raw.pov[0] = 4500;
    MapState(p, raw, gp);
    Check((gp.wButtons & VX_GAMEPAD_DPAD_UP) != 0, "hat up-right lights up");
    Check((gp.wButtons & VX_GAMEPAD_DPAD_RIGHT) != 0, "hat up-right lights right");

    raw.pov[0] = 18000;
    MapState(p, raw, gp);
    Check((gp.wButtons & VX_GAMEPAD_DPAD_UP) == 0, "hat down does not light up");
}

void TestAutoProfiles() {
    Section("Auto-mapping heuristic");

    // Layout 1: X/Y/Z/Rz - DualShock-style and most generic pads.
    {
        DeviceCaps c;
        c.axisPresent[(int)DiAxis::X]  = true;
        c.axisPresent[(int)DiAxis::Y]  = true;
        c.axisPresent[(int)DiAxis::Z]  = true;
        c.axisPresent[(int)DiAxis::Rz] = true;
        c.buttonCount = 12;
        c.povCount    = 1;

        DeviceProfile p;
        BuildAutoProfile(c, p);
        Check(p.axes[XA_LeftX].axis == DiAxis::X && !p.axes[XA_LeftX].invert, "L1: LX = x");
        Check(p.axes[XA_LeftY].axis == DiAxis::Y && p.axes[XA_LeftY].invert, "L1: LY = -y");
        Check(p.axes[XA_RightX].axis == DiAxis::Z, "L1: RX = z");
        Check(p.axes[XA_RightY].axis == DiAxis::Rz && p.axes[XA_RightY].invert, "L1: RY = -rz");
        CheckEqInt(p.buttons[XB_A].button, 0, "L1: A = button 0");
        CheckEqInt(p.buttons[XB_RightThumb].button, 9, "L1: R3 = button 9");
        Check(p.buttons[XB_DpadUp].kind == ButtonMapping::Kind::Pov, "L1: dpad from hat");
    }

    // Layout 2: X/Y/Z/Rx/Ry - Xbox-style pad through the generic HID driver,
    // where Z carries both triggers.
    {
        DeviceCaps c;
        c.axisPresent[(int)DiAxis::X]  = true;
        c.axisPresent[(int)DiAxis::Y]  = true;
        c.axisPresent[(int)DiAxis::Z]  = true;
        c.axisPresent[(int)DiAxis::Rx] = true;
        c.axisPresent[(int)DiAxis::Ry] = true;
        c.buttonCount = 10;
        c.povCount    = 1;

        DeviceProfile p;
        BuildAutoProfile(c, p);
        Check(p.axes[XA_RightX].axis == DiAxis::Rx, "L2: RX = rx");
        Check(p.axes[XA_RightY].axis == DiAxis::Ry && p.axes[XA_RightY].invert, "L2: RY = -ry");
        Check(p.axes[XA_LeftTrigger].axis == DiAxis::Z &&
                  p.axes[XA_LeftTrigger].half == AxisHalf::Positive, "L2: LT = z+");
        Check(p.axes[XA_RightTrigger].axis == DiAxis::Z &&
                  p.axes[XA_RightTrigger].half == AxisHalf::Negative, "L2: RT = z-");
    }

    // Layout 3: X/Y/Rz/Slider - several Logitech pads.
    {
        DeviceCaps c;
        c.axisPresent[(int)DiAxis::X]       = true;
        c.axisPresent[(int)DiAxis::Y]       = true;
        c.axisPresent[(int)DiAxis::Rz]      = true;
        c.axisPresent[(int)DiAxis::Slider0] = true;
        c.buttonCount = 12;
        c.povCount    = 1;

        DeviceProfile p;
        BuildAutoProfile(c, p);
        Check(p.axes[XA_RightX].axis == DiAxis::Rz, "L3: RX = rz");
        Check(p.axes[XA_RightY].axis == DiAxis::Slider0 && p.axes[XA_RightY].invert,
              "L3: RY = -slider0");
    }

    // Explicit config entries must survive auto-completion.
    {
        DeviceCaps c;
        c.axisPresent[(int)DiAxis::X]  = true;
        c.axisPresent[(int)DiAxis::Y]  = true;
        c.axisPresent[(int)DiAxis::Z]  = true;
        c.axisPresent[(int)DiAxis::Rz] = true;
        c.buttonCount = 12;
        c.povCount    = 1;

        DeviceProfile p;
        p.autoAxes = false;
        p.axes[XA_RightX].kind = AxisMapping::Kind::Axis;
        p.axes[XA_RightX].axis = DiAxis::Slider1;

        p.autoButtons = false;
        p.buttons[XB_A].kind   = ButtonMapping::Kind::Button;
        p.buttons[XB_A].button = 5;

        BuildAutoProfile(c, p);
        Check(p.axes[XA_RightX].axis == DiAxis::Slider1, "explicit axis is not overwritten");
        CheckEqInt(p.buttons[XB_A].button, 5, "explicit button is not overwritten");
        Check(!p.axes[XA_LeftX].IsSet(), "autoAxes off leaves the rest alone");
    }

    // A device with too few buttons must not have mappings invented for them.
    {
        DeviceCaps c;
        c.axisPresent[(int)DiAxis::X] = true;
        c.axisPresent[(int)DiAxis::Y] = true;
        c.buttonCount = 4;
        c.povCount    = 0;

        DeviceProfile p;
        BuildAutoProfile(c, p);
        Check(p.buttons[XB_Y].IsSet(), "button 3 exists and is mapped");
        Check(!p.buttons[XB_LeftShoulder].IsSet(), "button 4 does not exist, stays unmapped");
        Check(!p.buttons[XB_DpadUp].IsSet(), "no hat, no dpad");
    }
}


void TestYamlScalarSequences() {
    Section("YAML: sequences of plain scalars");

    const char* text =
        "installed:\n"
        "  - 'xinput1_3.dll'\n"
        "  - 'xinput1_4.dll'\n"
        "  - xinput9_1_0.dll\n"
        "after: 7\n";

    YamlNode    root;
    std::string err;
    Check(YamlParse(text, root, err), "parses", err.c_str());

    const YamlNode* files = root.Find("installed");
    Check(files != nullptr && files->IsSeq(), "installed is a sequence");
    if (files && files->IsSeq()) {
        CheckEqInt((long long)files->seq.size(), 3, "three entries");
        Check(files->seq[0].IsScalar() && files->seq[0].scalar == "xinput1_3.dll",
              "quoted scalar item");
        Check(files->seq[2].IsScalar() && files->seq[2].scalar == "xinput9_1_0.dll",
              "unquoted scalar item");
    }
    // A scalar list must not swallow the key that follows it.
    CheckEqInt(root.Int("after", -1), 7, "parsing resumes after the sequence");
}

void TestYamlQuoting() {
    Section("YAML: quoting rules");

    // Single quotes keep backslashes literal, which is why paths use them.
    const char* text =
        "path: 'C:\\Games\\Dark Souls'\n"
        "apostrophe: 'Baldur''s Gate'\n"
        "escaped: \"line\\nbreak\"\n";

    YamlNode    root;
    std::string err;
    Check(YamlParse(text, root, err), "parses", err.c_str());

    Check(root.Str("path", "") == "C:\\Games\\Dark Souls", "backslashes survive single quotes");
    Check(root.Str("apostrophe", "") == "Baldur's Gate", "'' becomes one quote");
    Check(root.Str("escaped", "") == "line\nbreak", "double quotes process escapes");
}

void TestRumbleConfig() {
    Section("Config: rumble");

    // On by default, so a force-feedback pad works without being configured.
    Config def;
    Check(ConfigParse("devices:\n  - match: \"*\"\n", def), "parses", def.error.c_str());
    Check(def.devices[0].rumble, "rumble defaults to on");
    CheckNear(def.devices[0].rumbleGain, 1.0f, 0.001f, "gain defaults to 1.0");

    Config off;
    Check(ConfigParse("devices:\n  - match: \"*\"\n    rumble: false\n", off),
          "parses", off.error.c_str());
    Check(!off.devices[0].rumble, "rumble can be turned off per device");

    // A top-level value is the default for every device.
    Config top;
    Check(ConfigParse("rumble: false\ndevices:\n  - match: \"a\"\n  - match: \"b\"\n", top),
          "parses", top.error.c_str());
    CheckEqInt((long long)top.devices.size(), 2, "two devices");
    Check(!top.devices[0].rumble && !top.devices[1].rumble,
          "top-level rumble applies to all devices");

    Config over;
    Check(ConfigParse("rumble: false\ndevices:\n  - match: \"a\"\n    rumble: true\n", over),
          "parses", over.error.c_str());
    Check(over.devices[0].rumble, "per-device value overrides the top-level one");

    Config gain;
    Check(ConfigParse("devices:\n  - match: \"*\"\n    rumble_gain: 0.5\n", gain),
          "parses", gain.error.c_str());
    CheckNear(gain.devices[0].rumbleGain, 0.5f, 0.001f, "gain is read");

    // Zero gain would be a silent motor, so it is treated as off outright.
    Config zero;
    Check(ConfigParse("devices:\n  - match: \"*\"\n    rumble_gain: 0\n", zero),
          "parses", zero.error.c_str());
    Check(!zero.devices[0].rumble, "zero gain disables rumble");
}

void TestPathHelpers() {
    Section("Path helpers");

    Check(JoinPath(L"C:\\Games", L"a.dll") == L"C:\\Games\\a.dll", "join adds a separator");
    Check(JoinPath(L"C:\\Games\\", L"a.dll") == L"C:\\Games\\a.dll", "join keeps one separator");
    Check(LeafName(L"C:\\Games\\Dark Souls") == L"Dark Souls", "leaf of a path");
    Check(LeafName(L"C:\\Games\\Dark Souls\\") == L"Dark Souls", "trailing separator ignored");

    Check(PeArchFromName("x86") == PeArch::X86, "x86 name");
    Check(PeArchFromName("x64") == PeArch::X64, "x64 name");
    Check(PeArchFromName("nonsense") == PeArch::Unknown, "unknown name");
    Check(std::string(PeArchName(PeArch::X86)) == "x86", "x86 round-trip");
}

void TestExeArchDetection() {
    Section("PE architecture detection");

    // This binary is the most reliable sample available: whatever it was built
    // as is what the detector must report.
    wchar_t self[MAX_PATH];
    DWORD   n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    Check(n > 0 && n < MAX_PATH, "got our own path");

    PeArch expected = (sizeof(void*) == 8) ? PeArch::X64 : PeArch::X86;
    PeArch got      = DetectExeArch(self);
    Check(got == expected, "detects this executable's architecture",
          PeArchName(got));

    Check(DetectExeArch(L"C:\\does\\not\\exist.exe") == PeArch::Unknown,
          "missing file is Unknown");

    // A non-PE file must be rejected rather than guessed at.
    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring junk = JoinPath(tmpDir, L"vx_not_a_pe.bin");
    FILE* f = nullptr;
    if (_wfopen_s(&f, junk.c_str(), L"wb") == 0 && f) {
        fputs("this is not an executable", f);
        fclose(f);
        Check(DetectExeArch(junk) == PeArch::Unknown, "non-PE file is Unknown");
        _wremove(junk.c_str());
    }
}

void TestGameLibraryRoundTrip() {
    Section("Game library: save and reload");

    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring path = JoinPath(tmpDir, L"vx_games_test.yml");

    GameLibrary out;

    GameEntry a;
    a.name   = "Dark Souls";
    a.folder = L"C:\\Games\\Dark Souls\\DATA";   // backslashes must survive
    a.exe    = L"DARKSOULS.exe";
    a.arch   = PeArch::X86;
    a.installedFiles.push_back(L"xinput1_3.dll");
    a.installedFiles.push_back(L"xinput9_1_0.dll");
    out.games.push_back(a);

    GameEntry b;
    b.name   = "Baldur's Gate";                  // an apostrophe in the name
    b.folder = L"D:\\Games\\BG";
    b.exe    = L"BGMain.exe";
    b.arch   = PeArch::X64;
    out.games.push_back(b);

    std::string err;
    Check(SaveGames(path, out, err), "saves", err.c_str());

    GameLibrary in;
    Check(LoadGames(path, in, err), "loads", err.c_str());
    CheckEqInt((long long)in.games.size(), 2, "two games survive the round trip");
    if (in.games.size() != 2) { _wremove(path.c_str()); return; }

    Check(in.games[0].name == "Dark Souls", "name");
    Check(in.games[0].folder == L"C:\\Games\\Dark Souls\\DATA", "folder with backslashes");
    Check(in.games[0].exe == L"DARKSOULS.exe", "exe");
    Check(in.games[0].arch == PeArch::X86, "arch");
    CheckEqInt((long long)in.games[0].installedFiles.size(), 2, "installed file list");
    Check(in.games[0].installedFiles.size() == 2 &&
              in.games[0].installedFiles[1] == L"xinput9_1_0.dll",
          "installed file names");

    Check(in.games[1].name == "Baldur's Gate", "apostrophe in a name survives");
    Check(in.games[1].arch == PeArch::X64, "second arch");
    Check(in.games[1].installedFiles.empty(), "no installed files recorded");

    CheckEqInt(in.FindByName("Dark Souls"), 0, "lookup by name");
    CheckEqInt(in.FindByName("dark souls"), 0, "lookup is case-insensitive");
    CheckEqInt(in.FindByName("Nope"), -1, "unknown name");

    // A library that does not exist yet is an empty one, not an error.
    GameLibrary missing;
    Check(LoadGames(JoinPath(tmpDir, L"vx_no_such_library.yml"), missing, err),
          "absent library is not an error");
    Check(missing.games.empty(), "absent library is empty");

    _wremove(path.c_str());
}

} // namespace

int main() {
    printf("virtual-xinput self-tests (%d-bit)\n", (int)(sizeof(void*) * 8));

    TestYamlBasics();
    TestYamlSequences();
    TestSpecParsing();
    TestConfigSchema();
    TestPovMask();
    TestDeadzones();
    TestTriggers();
    TestButtonsAndDpad();
    TestAutoProfiles();
    TestYamlScalarSequences();
    TestYamlQuoting();
    TestRumbleConfig();
    TestPathHelpers();
    TestExeArchDetection();
    TestGameLibraryRoundTrip();

    printf("\n----------------------------------------\n");
    if (g_failures == 0) {
        printf("PASSED: all %d checks\n", g_checks);
        return 0;
    }
    printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
