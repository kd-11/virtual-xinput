// virtual-xinput configurator
//
// Lists DirectInput controllers, shows what they report live alongside the
// XInput state they currently produce, and can learn a mapping by asking the
// user to press each control in turn.

#include "../common/config.h"
#include "../common/di_device.h"
#include "../common/games.h"
#include "../common/mapping.h"
#include "../common/pad_manager.h"
#include "../common/xinput_defs.h"
#include "../common/yaml.h"

#include <conio.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace vx;

namespace {

// An axis must move by at least this much from rest before it counts as a
// deliberate movement rather than noise or drift.
const float kMoveThreshold    = 0.45f;
// Everything must fall back inside this band before the next prompt is taken.
const float kReleaseThreshold = 0.20f;

const char* kAxisPromptLabel[XA_Count] = {
    "LEFT stick  ->  push RIGHT",
    "LEFT stick  ->  push UP",
    "RIGHT stick ->  push RIGHT",
    "RIGHT stick ->  push UP",
    "LEFT TRIGGER  ->  press fully",
    "RIGHT TRIGGER ->  press fully",
};

const char* kAxisKeyName[XA_Count] = {
    "left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger",
};

const char* kButtonPromptLabel[XB_Count] = {
    "A  (bottom face button)",
    "B  (right face button)",
    "X  (left face button)",
    "Y  (top face button)",
    "LB / L1  (left shoulder)",
    "RB / R1  (right shoulder)",
    "BACK / SELECT",
    "START",
    "L3  (press left stick in)",
    "R3  (press right stick in)",
    "GUIDE / HOME",
    "D-PAD UP",
    "D-PAD DOWN",
    "D-PAD LEFT",
    "D-PAD RIGHT",
};

const char* kButtonKeyName[XB_Count] = {
    "a", "b", "x", "y", "left_shoulder", "right_shoulder", "back", "start",
    "left_thumb", "right_thumb", "guide",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

void SetCursor(SHORT x, SHORT y) {
    COORD c;
    c.X = x;
    c.Y = y;
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), c);
}

void ClearScreen() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(h, &info)) return;

    DWORD cells = info.dwSize.X * info.dwSize.Y;
    DWORD written = 0;
    COORD origin;
    origin.X = 0;
    origin.Y = 0;
    FillConsoleOutputCharacterW(h, L' ', cells, origin, &written);
    FillConsoleOutputAttribute(h, info.wAttributes, cells, origin, &written);
    SetCursor(0, 0);
}

// Prints a line padded to a fixed width so redraws never leave stale text.
void PrintLine(const char* fmt, ...) {
    char    buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("%-100s\n", buf);
}

std::string Bar(float v, int width) {
    // Renders -1..1 as a centred bar.
    int   mid = width / 2;
    int   pos = (int)((v + 1.0f) * 0.5f * (width - 1) + 0.5f);
    if (pos < 0) pos = 0;
    if (pos >= width) pos = width - 1;

    std::string s(width, '.');
    s[mid] = '|';
    s[pos] = '#';
    return s;
}

// ---------------------------------------------------------------------------
// Learn-mode input detection
// ---------------------------------------------------------------------------

struct Detection {
    enum Kind { None, Axis, Button, Pov } kind;
    int   axisIndex;
    float baseline;
    float value;
    int   button;
    int   pov;
    int   povValue;

    Detection()
        : kind(None), axisIndex(-1), baseline(0), value(0), button(-1), pov(-1), povValue(-1) {}
};

bool StateIsNeutral(const RawState& cur, const RawState& base, const DeviceCaps& caps) {
    for (int i = 0; i < caps.buttonCount; ++i)
        if (cur.button[i] != base.button[i]) return false;
    for (int i = 0; i < caps.povCount; ++i)
        if (cur.pov[i] != base.pov[i]) return false;
    for (int i = 0; i < kAxisCount; ++i) {
        if (!caps.axisPresent[i]) continue;
        float d = cur.axis[i] - base.axis[i];
        if (d < 0) d = -d;
        if (d > kReleaseThreshold) return false;
    }
    return true;
}

enum WaitResult { Wait_Detected, Wait_Skipped, Wait_Aborted };

WaitResult WaitForInput(DiDevice* dev, DiSystem& di, Detection& out) {
    // Rest position, sampled once the user has stopped touching the pad.
    RawState base;
    for (int i = 0; i < 25; ++i) {
        dev->Poll(base);
        di.PumpMessages();
        Sleep(10);
    }

    const DeviceCaps& caps = dev->Caps();

    for (;;) {
        if (_kbhit()) {
            int c = _getch();
            if (c == 's' || c == 'S') return Wait_Skipped;
            if (c == 'q' || c == 'Q' || c == 27) return Wait_Aborted;
        }

        RawState cur;
        if (!dev->Poll(cur)) { di.PumpMessages(); Sleep(8); continue; }

        Detection found;

        for (int i = 0; i < caps.buttonCount && found.kind == Detection::None; ++i) {
            if (cur.button[i] && !base.button[i]) {
                found.kind   = Detection::Button;
                found.button = i;
            }
        }

        for (int i = 0; i < caps.povCount && found.kind == Detection::None; ++i) {
            if (cur.pov[i] >= 0 && base.pov[i] < 0) {
                found.kind     = Detection::Pov;
                found.pov      = i;
                found.povValue = cur.pov[i];
            }
        }

        if (found.kind == Detection::None) {
            // Largest deflection wins, so a stick that also nudges a
            // neighbouring axis still resolves to the axis actually pushed.
            float best = kMoveThreshold;
            for (int i = 0; i < kAxisCount; ++i) {
                if (!caps.axisPresent[i]) continue;
                float d   = cur.axis[i] - base.axis[i];
                float mag = d < 0 ? -d : d;
                if (mag > best) {
                    best           = mag;
                    found.kind     = Detection::Axis;
                    found.axisIndex = i;
                    found.baseline = base.axis[i];
                    found.value    = cur.axis[i];
                }
            }
        }

        if (found.kind != Detection::None) {
            out = found;
            // Let go before the next prompt, otherwise the release would be
            // read as the answer to it.
            printf("   ... release to continue\r");
            fflush(stdout);
            for (;;) {
                RawState after;
                if (dev->Poll(after) && StateIsNeutral(after, base, caps)) break;
                di.PumpMessages();
                Sleep(15);
            }
            printf("%-60s\r", "");
            return Wait_Detected;
        }

        di.PumpMessages();
        Sleep(8);
    }
}

// Turns a detected stick movement into a mapping. XInput's positive direction
// is right and up, so an axis that moved negative gets inverted.
AxisMapping AxisFromDetection(const Detection& d) {
    AxisMapping m;
    if (d.kind == Detection::Button) {
        m.kind   = AxisMapping::Kind::Button;
        m.button = d.button;
        return m;
    }
    if (d.kind != Detection::Axis) return m;

    m.kind   = AxisMapping::Kind::Axis;
    m.axis   = (DiAxis)d.axisIndex;
    m.invert = (d.value < d.baseline);
    return m;
}

// Triggers need more care than sticks: the same physical trigger may be a full
// axis resting at one end, or half of an axis that rests centred and is shared
// with the other trigger.
AxisMapping TriggerFromDetection(const Detection& d) {
    AxisMapping m;
    if (d.kind == Detection::Button) {
        m.kind   = AxisMapping::Kind::Button;
        m.button = d.button;
        return m;
    }
    if (d.kind != Detection::Axis) return m;

    m.kind = AxisMapping::Kind::Axis;
    m.axis = (DiAxis)d.axisIndex;

    bool restsAtEnd = (d.baseline < -0.6f) || (d.baseline > 0.6f);
    if (restsAtEnd) {
        m.half   = AxisHalf::Full;
        m.invert = (d.baseline > 0.6f);   // rests high, so the travel is downward
    } else {
        // Rests near centre: this trigger owns one half of the axis.
        m.half   = (d.value > d.baseline) ? AxisHalf::Positive : AxisHalf::Negative;
        m.invert = false;
    }
    return m;
}

ButtonMapping ButtonFromDetection(const Detection& d) {
    ButtonMapping m;
    if (d.kind == Detection::Button) {
        m.kind   = ButtonMapping::Kind::Button;
        m.button = d.button;
    } else if (d.kind == Detection::Pov) {
        m.kind    = ButtonMapping::Kind::Pov;
        m.pov     = d.pov;
        m.povMask = PovToMask(d.povValue);
    } else if (d.kind == Detection::Axis) {
        m.kind          = ButtonMapping::Kind::Axis;
        m.axis          = (DiAxis)d.axisIndex;
        m.axisPositive  = (d.value > d.baseline);
        m.axisThreshold = 0.5f;
    }
    return m;
}

// ---------------------------------------------------------------------------
// YAML emission
// ---------------------------------------------------------------------------

std::string AxisSpecString(const AxisMapping& m) {
    if (m.kind == AxisMapping::Kind::Button) {
        char buf[32];
        snprintf(buf, sizeof(buf), "button:%d", m.button);
        return buf;
    }
    if (m.kind != AxisMapping::Kind::Axis) return "none";

    std::string s;
    if (m.invert) s += "-";
    s += DiAxisName(m.axis);
    if (m.half == AxisHalf::Positive) s += "+";
    if (m.half == AxisHalf::Negative) s += "-";
    return s;
}

std::string ButtonSpecString(const ButtonMapping& m) {
    char buf[64];
    switch (m.kind) {
        case ButtonMapping::Kind::Button:
            snprintf(buf, sizeof(buf), "%d", m.button);
            return buf;
        case ButtonMapping::Kind::Pov: {
            const char* dir = "up";
            switch (m.povMask) {
                case VX_GAMEPAD_DPAD_UP:    dir = "up";    break;
                case VX_GAMEPAD_DPAD_DOWN:  dir = "down";  break;
                case VX_GAMEPAD_DPAD_LEFT:  dir = "left";  break;
                case VX_GAMEPAD_DPAD_RIGHT: dir = "right"; break;
                default: break;
            }
            snprintf(buf, sizeof(buf), "pov%d:%s", m.pov, dir);
            return buf;
        }
        case ButtonMapping::Kind::Axis:
            snprintf(buf, sizeof(buf), "axis:%s%s@%.2f", DiAxisName(m.axis),
                     m.axisPositive ? "+" : "-", m.axisThreshold);
            return buf;
        default:
            return "none";
    }
}

bool WriteConfig(const std::wstring& path, const DeviceInfo& info, const DeviceProfile& p) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return false;

    fprintf(f, "# virtual-xinput configuration\n");
    fprintf(f, "# Generated by virtual-xinput-config.\n");
    fprintf(f, "# Drop this next to xinput1_3.dll in the game folder.\n\n");
    fprintf(f, "log: false          # set true to write virtual-xinput.log beside the dll\n");
    fprintf(f, "poll_hz: 250\n\n");
    fprintf(f, "devices:\n");
    fprintf(f, "  - match: \"%s\"\n", Narrow(info.productName).c_str());
    fprintf(f, "    slot: %d\n", p.slot < 0 ? 0 : p.slot);
    fprintf(f, "    # Uncomment to bind this exact physical device instead of by name:\n");
    fprintf(f, "    # guid: \"%s\"\n\n", GuidToString(info.instanceGuid).c_str());

    fprintf(f, "    deadzone:\n");
    fprintf(f, "      left_stick:  %.3f\n", p.dz.leftStick);
    fprintf(f, "      right_stick: %.3f\n", p.dz.rightStick);
    fprintf(f, "      trigger:     %.3f\n", p.dz.trigger);
    fprintf(f, "      # Deflection treated as fully pushed; lower it if the\n");
    fprintf(f, "      # stick never quite reaches the corners.\n");
    fprintf(f, "      left_stick_max:  %.3f\n", p.dz.leftStickMax);
    fprintf(f, "      right_stick_max: %.3f\n\n", p.dz.rightStickMax);

    fprintf(f, "    axes:\n");
    for (int i = 0; i < XA_Count; ++i) {
        fprintf(f, "      %-14s %s\n", (std::string(kAxisKeyName[i]) + ":").c_str(),
                AxisSpecString(p.axes[i]).c_str());
    }
    fprintf(f, "\n    buttons:\n");
    for (int i = 0; i < XB_Count; ++i) {
        fprintf(f, "      %-15s %s\n", (std::string(kButtonKeyName[i]) + ":").c_str(),
                ButtonSpecString(p.buttons[i]).c_str());
    }

    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

void ShowDevices(const std::vector<DeviceInfo>& devices) {
    printf("\nDirectInput controllers found: %d\n\n", (int)devices.size());
    for (size_t i = 0; i < devices.size(); ++i) {
        printf("  [%d] %S\n", (int)i, devices[i].productName.c_str());
        printf("      instance guid: %s\n", GuidToString(devices[i].instanceGuid).c_str());
    }
    printf("\n");
}

void Monitor(DiSystem& di, DiDevice* dev, const DeviceProfile& baseProfile) {
    DeviceProfile profile = baseProfile;
    BuildAutoProfile(dev->Caps(), profile);

    ClearScreen();
    printf("Monitoring %S\n", dev->Info().productName.c_str());
    printf("Press any key to return to the menu.\n\n");

    const DeviceCaps& caps = dev->Caps();

    while (!_kbhit()) {
        RawState raw;
        if (!dev->Poll(raw)) { di.PumpMessages(); Sleep(30); continue; }

        XINPUT_GAMEPAD gp;
        MapState(profile, raw, gp);

        SetCursor(0, 4);
        PrintLine("--- DirectInput (raw) ---------------------------------------------");
        for (int i = 0; i < kAxisCount; ++i) {
            if (!caps.axisPresent[i]) continue;
            PrintLine("  %-8s %+.3f  %s", DiAxisName((DiAxis)i), raw.axis[i],
                      Bar(raw.axis[i], 41).c_str());
        }
        for (int i = 0; i < caps.povCount; ++i) {
            PrintLine("  pov%-5d %s", i, raw.pov[i] < 0 ? "centered"
                                                        : std::to_string(raw.pov[i] / 100).append(" deg").c_str());
        }

        std::string pressed;
        for (int i = 0; i < caps.buttonCount; ++i) {
            if (raw.button[i]) {
                if (!pressed.empty()) pressed += " ";
                pressed += std::to_string(i);
            }
        }
        PrintLine("  buttons  %s", pressed.empty() ? "(none)" : pressed.c_str());

        PrintLine("");
        PrintLine("--- XInput (mapped output) ----------------------------------------");
        PrintLine("  LeftThumb   X %+6d  Y %+6d", gp.sThumbLX, gp.sThumbLY);
        PrintLine("  RightThumb  X %+6d  Y %+6d", gp.sThumbRX, gp.sThumbRY);
        PrintLine("  Triggers    L %3d     R %3d", gp.bLeftTrigger, gp.bRightTrigger);

        static const char* kNames[XB_Count] = {
            "A", "B", "X", "Y", "LB", "RB", "Back", "Start", "L3", "R3", "Guide",
            "Up", "Down", "Left", "Right"};
        std::string on;
        for (int i = 0; i < XB_Count; ++i) {
            if (gp.wButtons & XButtonBit(i)) {
                if (!on.empty()) on += " ";
                on += kNames[i];
            }
        }
        PrintLine("  Buttons     %s", on.empty() ? "(none)" : on.c_str());

        di.PumpMessages();
        Sleep(30);
    }
    _getch();
}

bool RunWizard(DiSystem& di, DiDevice* dev, const DeviceInfo& info, DeviceProfile& profile) {
    ClearScreen();
    printf("Mapping wizard for %S\n\n", info.productName.c_str());
    printf("For each prompt, move or press the control named.\n");
    printf("  S = skip this control (leaves it unmapped)\n");
    printf("  Q = abort without saving\n\n");
    printf("Leave the controller untouched when a prompt appears; rest\n");
    printf("positions are sampled just before each one.\n\n");
    printf("Press Enter to begin...");
    while (_getch() != '\r') {}
    printf("\n\n");

    for (int i = 0; i < XA_Count; ++i) {
        printf("  %-34s : ", kAxisPromptLabel[i]);
        fflush(stdout);

        Detection  d;
        WaitResult r = WaitForInput(dev, di, d);
        if (r == Wait_Aborted) return false;
        if (r == Wait_Skipped) { printf("skipped\n"); continue; }

        bool isTrigger = (i == XA_LeftTrigger || i == XA_RightTrigger);
        profile.axes[i] = isTrigger ? TriggerFromDetection(d) : AxisFromDetection(d);
        printf("%s\n", AxisSpecString(profile.axes[i]).c_str());
    }
    profile.autoAxes = false;

    printf("\n");
    for (int i = 0; i < XB_Count; ++i) {
        printf("  %-34s : ", kButtonPromptLabel[i]);
        fflush(stdout);

        Detection  d;
        WaitResult r = WaitForInput(dev, di, d);
        if (r == Wait_Aborted) return false;
        if (r == Wait_Skipped) { printf("skipped\n"); continue; }

        profile.buttons[i] = ButtonFromDetection(d);
        printf("%s\n", ButtonSpecString(profile.buttons[i]).c_str());

        // A hat covers all four directions at once, so the remaining D-pad
        // prompts would only be asking the same question again.
        if (i == XB_DpadUp && profile.buttons[i].kind == ButtonMapping::Kind::Pov) {
            int  hat = profile.buttons[i].pov;
            const WORD masks[4] = {VX_GAMEPAD_DPAD_UP, VX_GAMEPAD_DPAD_DOWN,
                                   VX_GAMEPAD_DPAD_LEFT, VX_GAMEPAD_DPAD_RIGHT};
            for (int k = 0; k < 4; ++k) {
                profile.buttons[XB_DpadUp + k].kind    = ButtonMapping::Kind::Pov;
                profile.buttons[XB_DpadUp + k].pov     = hat;
                profile.buttons[XB_DpadUp + k].povMask = masks[k];
            }
            printf("  %-34s : detected as hat pov%d, remaining directions filled in\n",
                   "D-PAD (rest)", hat);
            break;
        }
    }
    profile.autoButtons = false;
    profile.autoDpad    = false;

    return true;
}

// Plays a short pattern through the motors so the user can confirm rumble
// actually works and can tell which motor is which.
void TestForceFeedback(DiSystem& di, DiDevice* dev) {
    printf("\n");

    if (!dev->ForceFeedbackCapable()) {
        printf("This device does not report force feedback to DirectInput.\n");
        printf("Many pads rumble only through their own vendor driver, which\n");
        printf("DirectInput cannot reach. There is nothing to test here.\n");
        return;
    }
    if (!dev->ForceFeedbackReady()) {
        printf("The device reports force feedback, but no effect could be created.\n");
        printf("Something else may be holding it exclusively - close any other\n");
        printf("program using the controller and try again.\n");
        return;
    }

    printf("Force feedback: %d actuator(s), %s\n\n",
           dev->ForceFeedbackAxisCount(), dev->ForceFeedbackKind());

    struct Step { const char* label; float left; float right; int ms; };
    static const Step kSteps[] = {
        {"left / strong motor", 1.0f, 0.0f, 900},
        {"right / weak motor ", 0.0f, 1.0f, 900},
        {"both, half strength", 0.5f, 0.5f, 900},
        {"both, full strength", 1.0f, 1.0f, 900},
    };

    for (int i = 0; i < (int)(sizeof(kSteps) / sizeof(kSteps[0])); ++i) {
        // With a single actuator the two motors are the same hardware, so the
        // second step would just repeat the first.
        if (dev->ForceFeedbackAxisCount() < 2 && i == 1) {
            printf("  %s  ... skipped (only one actuator)\n", kSteps[i].label);
            continue;
        }

        printf("  %s  ...", kSteps[i].label);
        fflush(stdout);

        dev->SetRumble(kSteps[i].left, kSteps[i].right);
        DWORD until = GetTickCount() + (DWORD)kSteps[i].ms;
        while (GetTickCount() < until) {
            di.PumpMessages();
            Sleep(15);
        }

        dev->SetRumble(0.0f, 0.0f);
        until = GetTickCount() + 250;
        while (GetTickCount() < until) {
            di.PumpMessages();
            Sleep(15);
        }
        printf(" done\n");
    }

    dev->StopRumble();
    printf("\nFelt nothing? Set `rumble: false` in the config; that keeps the pad\n");
    printf("in shared mode instead of claiming it exclusively.\n");
}

// Non-interactive diagnostics: what the device reports, what the automatic
// mapping makes of it, and a few live samples. Useful to paste when something
// is not behaving.
void Probe(DiSystem& di, DiDevice* dev) {
    const DeviceCaps& caps = dev->Caps();

    printf("axes present : ");
    for (int i = 0; i < kAxisCount; ++i) {
        if (caps.axisPresent[i]) printf("%s ", DiAxisName((DiAxis)i));
    }
    printf("\nbuttons      : %d\nhats         : %d\n", caps.buttonCount, caps.povCount);
    printf("force feedbk : %s\n\n",
           dev->ForceFeedbackReady()
               ? dev->ForceFeedbackKind()
               : (dev->ForceFeedbackCapable()
                      ? "reported, but no effect could be created"
                      : "not supported by this device"));

    DeviceProfile profile;
    profile.slot = 0;
    BuildAutoProfile(caps, profile);
    printf("auto mapping : %s\n\n", DescribeProfile(profile).c_str());

    printf("live samples (rest position):\n");
    for (int sample = 0; sample < 3; ++sample) {
        RawState raw;
        for (int i = 0; i < 10; ++i) { dev->Poll(raw); di.PumpMessages(); Sleep(10); }

        printf("  raw:");
        for (int i = 0; i < kAxisCount; ++i) {
            if (caps.axisPresent[i]) printf(" %s=%+.3f", DiAxisName((DiAxis)i), raw.axis[i]);
        }
        printf(" pov0=%d\n", raw.pov[0]);

        XINPUT_GAMEPAD gp;
        MapState(profile, raw, gp);
        printf("  xinput: LX=%+6d LY=%+6d RX=%+6d RY=%+6d LT=%3d RT=%3d buttons=0x%04X\n",
               gp.sThumbLX, gp.sThumbLY, gp.sThumbRX, gp.sThumbRY,
               gp.bLeftTrigger, gp.bRightTrigger, gp.wButtons);
    }
}

int PromptIndex(const char* label, int maxExclusive) {
    for (;;) {
        printf("%s", label);
        fflush(stdout);

        char line[64];
        if (!fgets(line, sizeof(line), stdin)) return -1;
        if (line[0] == '\n') return -1;

        char* end = nullptr;
        long  v   = strtol(line, &end, 10);
        if (end != line && v >= 0 && v < maxExclusive) return (int)v;

        printf("  Please enter a number between 0 and %d.\n", maxExclusive - 1);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Game library
// ---------------------------------------------------------------------------

std::string ReadLine(const char* prompt) {
    printf("%s", prompt);
    fflush(stdout);

    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) return std::string();

    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();

    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    s = s.substr(b, e - b);

    // "Copy as path" in Explorer wraps the path in quotes; drop them.
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

void ShowGames(const GameLibrary& lib) {
    if (lib.games.empty()) {
        printf("\nNo games in the list yet.\n");
        return;
    }

    printf("\n  #  %-28s %-6s %-9s %s\n", "name", "arch", "installed", "folder");
    printf("  -- ---------------------------- ------ --------- ------------------------\n");
    for (size_t i = 0; i < lib.games.size(); ++i) {
        const GameEntry& g = lib.games[i];
        printf("  %-2d %-28.28s %-6s %-9s %S\n", (int)i, g.name.c_str(),
               PeArchName(g.arch), IsInstalled(g) ? "yes" : "no", g.folder.c_str());
    }
}

// Resolves either a list index or a game name.
int ResolveGame(const GameLibrary& lib, const std::string& token) {
    if (token.empty()) return -1;

    bool digits = true;
    for (size_t i = 0; i < token.size(); ++i) {
        if (!isdigit((unsigned char)token[i])) { digits = false; break; }
    }
    if (digits) {
        int v = atoi(token.c_str());
        if (v >= 0 && v < (int)lib.games.size()) return v;
        return -1;
    }
    return lib.FindByName(token);
}

// Builds a GameEntry from a folder, working out the architecture from an
// executable inside it. `interactive` allows prompting when the choice is
// ambiguous.
bool BuildGameEntry(const std::wstring& folder, GameEntry& g, bool interactive,
                    std::string& err) {
    if (!DirectoryExists(folder)) {
        err = "no such folder: " + Narrow(folder);
        return false;
    }

    g.folder = folder;
    g.name   = Narrow(LeafName(folder));

    std::vector<std::wstring> exes = FindExecutables(folder);
    if (exes.empty()) {
        err = "no .exe found in that folder - is it the folder the game runs from?";
        return false;
    }

    size_t pick = 0;
    if (exes.size() > 1) {
        if (interactive) {
            printf("\nSeveral executables here. Which one is the game?\n");
            for (size_t i = 0; i < exes.size(); ++i) {
                PeArch a = DetectExeArch(JoinPath(folder, exes[i]));
                printf("  [%d] %-40S %s\n", (int)i, exes[i].c_str(), PeArchName(a));
            }
            std::string sel = ReadLine("Number (Enter for 0): ");
            if (!sel.empty()) {
                int v = atoi(sel.c_str());
                if (v >= 0 && v < (int)exes.size()) pick = (size_t)v;
            }
        } else {
            // Non-interactive: prefer the first 32-bit executable, since that is
            // overwhelmingly what old games are, and report the assumption.
            for (size_t i = 0; i < exes.size(); ++i) {
                if (DetectExeArch(JoinPath(folder, exes[i])) == PeArch::X86) { pick = i; break; }
            }
        }
    }

    g.exe  = exes[pick];
    g.arch = DetectExeArch(JoinPath(folder, g.exe));
    if (g.arch == PeArch::Unknown) {
        err = "could not read the PE header of " + Narrow(g.exe);
        return false;
    }
    return true;
}

// Prints what a game folder's config currently says, if it has one.
void ShowGameConfig(const GameEntry& g) {
    std::wstring path = JoinPath(g.folder, L"virtual-xinput.yml");
    if (!FileExists(path)) {
        printf("\nNo virtual-xinput.yml in that folder - the DLL will auto-detect.\n");
        return;
    }

    std::string text;
    if (!ReadFileUtf8(path, text)) {
        printf("\nCould not read %S\n", path.c_str());
        return;
    }

    Config cfg;
    if (!ConfigParse(text, cfg)) {
        printf("\n%S is INVALID: %s\n", path.c_str(), cfg.error.c_str());
        return;
    }

    printf("\n%S\n", path.c_str());
    printf("  log=%s poll_hz=%d devices=%d\n", cfg.log ? "true" : "false",
           cfg.pollHz, (int)cfg.devices.size());
    for (size_t i = 0; i < cfg.devices.size(); ++i) {
        const DeviceProfile& p = cfg.devices[i];
        printf("  [%d] match=\"%s\" slot=%d rumble=%s\n", (int)i, p.match.c_str(),
               p.slot, p.rumble ? "on" : "off");
        printf("      %s\n", DescribeProfile(p).c_str());
    }
}

void GamesMenu(GameLibrary& lib, const std::wstring& toolDir,
               const DeviceInfo* info, const DeviceProfile* profile,
               const DeviceCaps* caps) {
    std::wstring libPath = GamesFilePath(toolDir);

    for (;;) {
        ShowGames(lib);

        printf("\n  a  Add a game folder\n");
        printf("  i  Install into a game\n");
        printf("  u  Uninstall from a game\n");
        printf("  r  Remove a game from this list\n");
        printf("  s  Show a game's config\n");
        if (info && profile) {
            printf("  w  Write the current mapping into a game's folder\n");
        }
        printf("  b  Back\n");

        std::string choice = ReadLine("Choice: ");
        if (choice.empty()) continue;

        std::string err;

        if (choice == "b") return;

        if (choice == "a") {
            std::string folder = ReadLine("Game folder (where the .exe lives): ");
            if (folder.empty()) continue;

            GameEntry g;
            if (!BuildGameEntry(Widen(folder), g, true, err)) {
                printf("\nERROR: %s\n", err.c_str());
                continue;
            }

            std::string name = ReadLine(("Name [" + g.name + "]: ").c_str());
            if (!name.empty()) g.name = name;

            lib.games.push_back(g);
            if (!SaveGames(libPath, lib, err)) printf("\nERROR: %s\n", err.c_str());
            else printf("\nAdded %s (%s).\n", g.name.c_str(), PeArchName(g.arch));
            continue;
        }

        // Everything below acts on a chosen game.
        int index = ResolveGame(lib, ReadLine("Which game (number or name)? "));
        if (index < 0) {
            printf("\nNo such game.\n");
            continue;
        }
        GameEntry& g = lib.games[index];

        if (choice == "i") {
            if (InstallGame(toolDir, g, err)) {
                printf("\nInstalled %d file(s) into %S\n",
                       (int)g.installedFiles.size(), g.folder.c_str());
                SaveGames(libPath, lib, err);
            } else {
                printf("\nERROR: %s\n", err.c_str());
            }
        } else if (choice == "u") {
            if (UninstallGame(toolDir, g, err)) {
                printf("\nRemoved from %S\n", g.folder.c_str());
                if (!err.empty()) printf("Note: %s\n", err.c_str());
                SaveGames(libPath, lib, err);
            } else {
                printf("\nERROR: %s\n", err.c_str());
            }
        } else if (choice == "r") {
            if (IsInstalled(g)) {
                printf("\nStill installed there. Uninstall first, or it stays behind.\n");
                std::string sure = ReadLine("Remove from the list anyway? (y/N): ");
                if (sure != "y" && sure != "Y") continue;
            }
            printf("\nRemoved %s from the list.\n", g.name.c_str());
            lib.games.erase(lib.games.begin() + index);
            SaveGames(libPath, lib, err);
        } else if (choice == "s") {
            ShowGameConfig(g);
        } else if (choice == "w" && info && profile && caps) {
            DeviceProfile out = *profile;
            if (caps) BuildAutoProfile(*caps, out);

            std::wstring path = JoinPath(g.folder, L"virtual-xinput.yml");
            if (FileExists(path)) {
                std::string sure = ReadLine("A config is already there. Overwrite? (y/N): ");
                if (sure != "y" && sure != "Y") continue;
            }
            if (WriteConfig(path, *info, out)) printf("\nWrote %S\n", path.c_str());
            else printf("\nERROR: could not write %S\n", path.c_str());
        }
    }
}

// ---------------------------------------------------------------------------

void PrintUsage() {
    printf("\nUsage:\n");
    printf("  virtual-xinput-config                    interactive menu\n");
    printf("  virtual-xinput-config list               list attached controllers\n");
    printf("  virtual-xinput-config probe [index]      controller diagnostics\n");
    printf("  virtual-xinput-config validate <file>    check a config file\n");
    printf("  virtual-xinput-config games              list known games\n");
    printf("  virtual-xinput-config add <folder>       add a game folder\n");
    printf("  virtual-xinput-config install <game>     install into a game\n");
    printf("  virtual-xinput-config uninstall <game>   remove from a game\n");
    printf("\n<game> is either the number shown by `games` or the game's name.\n");
}

int main(int argc, char** argv) {
    printf("virtual-xinput configurator (%d-bit)\n", (int)(sizeof(void*) * 8));

    // The library lives beside this executable, so the whole folder can be
    // copied around and stays self-contained.
    std::wstring toolDir = ModuleDirectory(nullptr);
    std::wstring libPath = GamesFilePath(toolDir);

    GameLibrary lib;
    std::string err;
    if (!LoadGames(libPath, lib, err)) {
        printf("\nWARNING: %S is not readable: %s\n", libPath.c_str(), err.c_str());
    }

    std::string cmd = (argc > 1) ? argv[1] : "";
    std::string arg = (argc > 2) ? argv[2] : "";

    // ---- commands that need no controller ---------------------------------
    if (cmd == "help" || cmd == "--help" || cmd == "-h" || cmd == "/?") {
        PrintUsage();
        return 0;
    }

    if (cmd == "validate") {
        if (arg.empty()) { PrintUsage(); return 1; }

        std::string text;
        if (!ReadFileUtf8(Widen(arg), text)) {
            printf("\nERROR: cannot read %s\n", arg.c_str());
            return 1;
        }

        Config cfg;
        if (!ConfigParse(text, cfg)) {
            printf("\nINVALID: %s\n", cfg.error.c_str());
            return 1;
        }

        printf("\nOK: %s\n", arg.c_str());
        printf("  log=%s poll_hz=%d devices=%d\n", cfg.log ? "true" : "false",
               cfg.pollHz, (int)cfg.devices.size());
        for (size_t i = 0; i < cfg.devices.size(); ++i) {
            const DeviceProfile& p = cfg.devices[i];
            printf("  [%d] match=\"%s\" slot=%d rumble=%s dz(l=%.2f r=%.2f t=%.2f)\n",
                   (int)i, p.match.c_str(), p.slot, p.rumble ? "on" : "off",
                   p.dz.leftStick, p.dz.rightStick, p.dz.trigger);
            printf("      %s\n", DescribeProfile(p).c_str());
        }
        return 0;
    }

    if (cmd == "games") {
        ShowGames(lib);
        printf("\nLibrary: %S\n", libPath.c_str());
        return 0;
    }

    if (cmd == "add") {
        if (arg.empty()) { PrintUsage(); return 1; }

        GameEntry g;
        if (!BuildGameEntry(Widen(arg), g, false, err)) {
            printf("\nERROR: %s\n", err.c_str());
            return 1;
        }
        lib.games.push_back(g);
        if (!SaveGames(libPath, lib, err)) {
            printf("\nERROR: %s\n", err.c_str());
            return 1;
        }
        printf("\nAdded %s (%s, %S)\n", g.name.c_str(), PeArchName(g.arch), g.exe.c_str());
        return 0;
    }

    if (cmd == "install" || cmd == "uninstall") {
        if (arg.empty()) { PrintUsage(); return 1; }

        int index = ResolveGame(lib, arg);
        if (index < 0) {
            printf("\nERROR: no game matching '%s'. Try `games` to list them.\n", arg.c_str());
            return 1;
        }
        GameEntry& g = lib.games[index];

        bool ok = (cmd == "install") ? InstallGame(toolDir, g, err)
                                     : UninstallGame(toolDir, g, err);
        if (!ok) {
            printf("\nERROR: %s\n", err.c_str());
            return 1;
        }
        SaveGames(libPath, lib, err);
        printf("\n%s: %s -> %S\n", cmd.c_str(), g.name.c_str(), g.folder.c_str());
        return 0;
    }

    if (!cmd.empty() && cmd != "list" && cmd != "probe") {
        printf("\nUnknown command '%s'.\n", cmd.c_str());
        PrintUsage();
        return 1;
    }

    // ---- everything below wants a controller ------------------------------
    DiSystem di;
    if (!di.Init()) {
        printf("\nERROR: could not initialise DirectInput: %s\n", di.LastError().c_str());
        return 1;
    }

    std::vector<DeviceInfo> devices;
    di.Enumerate(devices);

    if (devices.empty()) {
        printf("\nNo DirectInput game controllers are attached.\n");
        if (cmd == "list" || cmd == "probe") return 1;

        printf("You can still manage installed games.\n");
        GamesMenu(lib, toolDir, nullptr, nullptr, nullptr);
        return 0;
    }

    ShowDevices(devices);

    if (cmd == "list") return 0;

    bool probeMode = (cmd == "probe");

    int index = 0;
    if (probeMode) {
        if (!arg.empty()) index = atoi(arg.c_str());
        if (index < 0 || index >= (int)devices.size()) index = 0;
    } else if (devices.size() > 1) {
        index = PromptIndex("Select a controller by number: ", (int)devices.size());
        if (index < 0) return 0;
    }

    // Requested here so the configurator can test rumble; it is only actually
    // taken (and only then exclusively) if the device supports it.
    DiDevice* dev = di.Open(devices[index], true);
    if (!dev) {
        printf("ERROR: could not open that controller.\n");
        return 1;
    }

    const DeviceCaps& caps = dev->Caps();
    printf("Opened: %S\n", devices[index].productName.c_str());
    printf("  axes:    ");
    for (int i = 0; i < kAxisCount; ++i) {
        if (caps.axisPresent[i]) printf("%s ", DiAxisName((DiAxis)i));
    }
    printf("\n  buttons: %d\n  hats:    %d\n", caps.buttonCount, caps.povCount);
    printf("  rumble:  %s\n",
           dev->ForceFeedbackReady()
               ? dev->ForceFeedbackKind()
               : (dev->ForceFeedbackCapable() ? "reported, unavailable"
                                              : "not supported"));

    if (probeMode) {
        printf("\n");
        Probe(di, dev);
        delete dev;
        return 0;
    }

    DeviceProfile profile;
    profile.slot = 0;

    for (;;) {
        printf("\n----------------------------------------------\n");
        printf(" 1  Monitor input (raw + mapped values)\n");
        printf(" 2  Run the mapping wizard\n");
        printf(" 3  Test force feedback\n");
        printf(" 4  Save virtual-xinput.yml here\n");
        printf(" 5  Manage games (install / uninstall / per-game config)\n");
        printf(" 6  Quit\n");

        std::string choice = ReadLine("Choice: ");
        if (choice.empty()) continue;

        if (choice == "1") {
            Monitor(di, dev, profile);
            ClearScreen();
        } else if (choice == "2") {
            DeviceProfile learned;
            learned.slot = profile.slot;
            learned.dz   = profile.dz;
            if (RunWizard(di, dev, devices[index], learned)) {
                profile = learned;
                printf("\nMapping complete. Save it with 4, or write it straight\n");
                printf("into a game folder with 5.\n");
            } else {
                printf("\nWizard aborted; nothing changed.\n");
            }
        } else if (choice == "3") {
            TestForceFeedback(di, dev);
        } else if (choice == "4") {
            DeviceProfile out = profile;
            BuildAutoProfile(caps, out);

            wchar_t cwd[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, cwd);
            std::wstring path = std::wstring(cwd) + L"\\virtual-xinput.yml";

            if (WriteConfig(path, devices[index], out)) {
                printf("\nWrote %S\n", path.c_str());
                printf("Copy it into the game folder next to xinput1_3.dll.\n");
            } else {
                printf("\nERROR: could not write %S\n", path.c_str());
            }
        } else if (choice == "5") {
            GamesMenu(lib, toolDir, &devices[index], &profile, &caps);
        } else if (choice == "6") {
            break;
        }
    }

    dev->StopRumble();
    delete dev;
    return 0;
}
