#include "draw_pad.h"

#include <cmath>
#include <cstdio>

namespace vx {
namespace gui {

namespace {

// Everything is laid out in a fixed 640x400 design space and scaled to the
// width the caller asks for, so the proportions never depend on window size.
const float kDesignW = 640.0f;
const float kDesignH = 400.0f;

// ---- palette --------------------------------------------------------------
const ImU32 kBodyFill  = IM_COL32( 38,  40,  46, 255);
const ImU32 kBodyEdge  = IM_COL32( 62,  66,  76, 255);
const ImU32 kCtrlFill  = IM_COL32( 58,  61,  70, 255);
const ImU32 kCtrlEdge  = IM_COL32( 96, 101, 113, 255);
const ImU32 kLabel     = IM_COL32(168, 173, 186, 255);
const ImU32 kLabelDim  = IM_COL32(112, 117, 128, 255);
const ImU32 kAccent    = IM_COL32( 92, 170, 255, 255);
const ImU32 kHover     = IM_COL32(150, 160, 180, 255);
const ImU32 kDeadzone  = IM_COL32( 74,  52,  52, 190);
const ImU32 kDeadzoneEdge = IM_COL32(196, 104,  96, 235);
const ImU32 kSatRing   = IM_COL32(110, 100,  60, 220);
const ImU32 kRawDot    = IM_COL32(130, 136, 150, 255);

// The Xbox face-button colours are worth keeping: they are how most people
// identify the buttons, and a rotated A/B/X/Y is the single most common thing
// this app exists to fix.
struct FaceColour { ImU32 idle, active; };
const FaceColour kFace[4] = {
    { IM_COL32( 42,  72,  50, 255), IM_COL32( 86, 200, 110, 255) },  // A green
    { IM_COL32( 78,  44,  44, 255), IM_COL32(232,  92,  92, 255) },  // B red
    { IM_COL32( 40,  54,  84, 255), IM_COL32( 92, 140, 245, 255) },  // X blue
    { IM_COL32( 80,  68,  38, 255), IM_COL32(240, 196,  84, 255) },  // Y amber
};

struct Layout {
    ImVec2 origin;
    float  s;                        // design units -> pixels

    ImVec2 P(float x, float y) const { return ImVec2(origin.x + x * s, origin.y + y * s); }
    float  L(float v)          const { return v * s; }
};

// Control geometry, in design units.
//
// The vertical bands, top to bottom, none of which may overlap:
//
//     12 ..  52   triggers
//     60 ..  92   shoulders
//    100 .. 330   body
//      106 .. 146   Y          (kFaceY - kFaceOff -/+ kFaceR)
//      124 .. 212   left stick
//      152 .. 188   Back / Guide / Start
//      198 .. 238   A
//      216 .. 320   d-pad
//      224 .. 312   right stick
//    210 .. 390   grips
//
// Changing any of kFaceY, kFaceOff, kFaceR or the shoulder band means
// re-checking that list. The shoulders once sat at 74..92 against a Y button
// whose top edge was 94, and the two drew straight through each other.
const float kLeftStickX  = 185.0f, kLeftStickY  = 168.0f, kStickR = 44.0f;
const float kRightStickX = 375.0f, kRightStickY = 268.0f;
const float kDpadX       = 255.0f, kDpadY       = 268.0f;
const float kDpadArm     =  52.0f, kDpadThick   =  24.0f;
const float kFaceX       = 470.0f, kFaceY       = 172.0f;
const float kFaceOff     =  46.0f, kFaceR       =  20.0f;

// A click inside a stick past this fraction of its radius means the stick
// click (L3/R3) rather than the stick axes.
const float kStickRingFrom = 0.70f;

ImU32 Lerp(ImU32 a, ImU32 b, float t) {
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
    ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
        ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

// 0..1 pulse used to mark the control the app is waiting for.
float Pulse() {
    return 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.0f);
}

void CenteredText(ImDrawList* dl, ImVec2 centre, const char* text, ImU32 col, float scale) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale,
                ImVec2(centre.x - sz.x * scale * 0.5f, centre.y - sz.y * scale * 0.5f),
                col, text);
}

// A control's interaction state for one frame, gathered before drawing so the
// drawing can react to it without a frame of lag.
struct Hot {
    bool hovered = false;
    bool clicked = false;
    bool rclick  = false;
    ImVec2 clickPos;
};

Hot Zone(const char* id, ImVec2 a, ImVec2 b) {
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    Hot h;
    h.hovered  = ImGui::IsItemHovered();
    h.clicked  = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    h.rclick   = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    h.clickPos = ImGui::GetIO().MousePos;
    return h;
}

void Record(PadHit& hit, const Hot& h, PadControl kind, int index) {
    if (!h.clicked && !h.rclick) return;
    hit.kind  = kind;
    hit.index = index;
    hit.clear = h.rclick;
}

// Outline drawn around whichever control is being captured or hovered.
void Marker(ImDrawList* dl, ImVec2 a, ImVec2 b, bool capturing, bool hovered, float s) {
    if (capturing) {
        dl->AddRect(ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s),
                    Lerp(kAccent, IM_COL32(255, 255, 255, 255), Pulse()),
                    5.0f * s, 0, 2.5f * s);
    } else if (hovered) {
        dl->AddRect(ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s),
                    kHover, 5.0f * s, 0, 1.5f * s);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Names and prompts
// ---------------------------------------------------------------------------

const char* XAxisName(int slot) {
    switch (slot) {
    case XA_LeftX:        return "Left stick X";
    case XA_LeftY:        return "Left stick Y";
    case XA_RightX:       return "Right stick X";
    case XA_RightY:       return "Right stick Y";
    case XA_LeftTrigger:  return "Left trigger";
    case XA_RightTrigger: return "Right trigger";
    default:              return "?";
    }
}

const char* XButtonName(int slot) {
    switch (slot) {
    case XB_A:             return "A";
    case XB_B:             return "B";
    case XB_X:             return "X";
    case XB_Y:             return "Y";
    case XB_LeftShoulder:  return "LB";
    case XB_RightShoulder: return "RB";
    case XB_Back:          return "Back";
    case XB_Start:         return "Start";
    case XB_LeftThumb:     return "L3";
    case XB_RightThumb:    return "R3";
    case XB_Guide:         return "Guide";
    case XB_DpadUp:        return "D-pad Up";
    case XB_DpadDown:      return "D-pad Down";
    case XB_DpadLeft:      return "D-pad Left";
    case XB_DpadRight:     return "D-pad Right";
    default:               return "?";
    }
}

const char* XAxisPrompt(int slot) {
    switch (slot) {
    case XA_LeftX:        return "Push the LEFT stick RIGHT";
    case XA_LeftY:        return "Push the LEFT stick UP";
    case XA_RightX:       return "Push the RIGHT stick RIGHT";
    case XA_RightY:       return "Push the RIGHT stick UP";
    case XA_LeftTrigger:  return "Press the LEFT TRIGGER fully";
    case XA_RightTrigger: return "Press the RIGHT TRIGGER fully";
    default:              return "Move a control";
    }
}

const char* XButtonPrompt(int slot) {
    switch (slot) {
    case XB_DpadUp:    return "Press D-PAD UP";
    case XB_DpadDown:  return "Press D-PAD DOWN";
    case XB_DpadLeft:  return "Press D-PAD LEFT";
    case XB_DpadRight: return "Press D-PAD RIGHT";
    case XB_LeftThumb: return "Press the LEFT STICK in (L3)";
    case XB_RightThumb:return "Press the RIGHT STICK in (R3)";
    default:           return nullptr;   // caller falls back to "Press <name>"
    }
}

// ---------------------------------------------------------------------------
// The pad
// ---------------------------------------------------------------------------

float PadAspectRatio() { return kDesignW / kDesignH; }

PadHit DrawVirtualPad(const PadView& view, const Deadzone& dz,
                      PadControl highlightKind, int highlightIndex,
                      float width) {
    PadHit hit;

    Layout lay;
    lay.origin = ImGui::GetCursorScreenPos();
    lay.s      = width / kDesignW;

    const float s  = lay.s;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Reserve the space first; the invisible buttons below reposition the
    // cursor freely and it has to be put back afterwards.
    const ImVec2 canvasEnd(lay.origin.x + width, lay.origin.y + kDesignH * s);
    ImGui::Dummy(ImVec2(width, kDesignH * s));

    const bool  live   = view.deviceOpen;
    const WORD  btns   = view.out.wButtons;
    auto        down   = [&](int slot) { return live && (btns & XButtonBit(slot)) != 0; };
    auto        capB   = [&](int slot) { return highlightKind == PadControl::Button && highlightIndex == slot; };
    auto        capA   = [&](int slot) { return highlightKind == PadControl::Axis   && highlightIndex == slot; };

    // ---- interaction zones, gathered before drawing ------------------------
    const ImVec2 lsA = lay.P(kLeftStickX - kStickR, kLeftStickY - kStickR);
    const ImVec2 lsB = lay.P(kLeftStickX + kStickR, kLeftStickY + kStickR);
    const ImVec2 rsA = lay.P(kRightStickX - kStickR, kRightStickY - kStickR);
    const ImVec2 rsB = lay.P(kRightStickX + kStickR, kRightStickY + kStickR);

    const Hot hLs = Zone("##lstick", lsA, lsB);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click the middle to bind the stick axes.\n"
                          "Click the outer ring to bind the stick click (L3).");
    }
    const Hot hRs = Zone("##rstick", rsA, rsB);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click the middle to bind the stick axes.\n"
                          "Click the outer ring to bind the stick click (R3).");
    }

    // A stick carries two destinations: its axes, and the click. Which one was
    // meant is decided by how far from the centre the click landed - the outer
    // ring is drawn to advertise that.
    auto stickHit = [&](const Hot& h, ImVec2 centre, int axisSlot, int thumbSlot) {
        if (!h.clicked && !h.rclick) return;
        const float dx  = h.clickPos.x - centre.x;
        const float dy  = h.clickPos.y - centre.y;
        const float d   = sqrtf(dx * dx + dy * dy);
        const bool  ring = d > kStickR * s * kStickRingFrom;
        hit.kind  = ring ? PadControl::Button : PadControl::Axis;
        hit.index = ring ? thumbSlot : axisSlot;
        hit.clear = h.rclick;
    };

    const ImVec2 lsC = lay.P(kLeftStickX, kLeftStickY);
    const ImVec2 rsC = lay.P(kRightStickX, kRightStickY);
    stickHit(hLs, lsC, XA_LeftX,  XB_LeftThumb);
    stickHit(hRs, rsC, XA_RightX, XB_RightThumb);

    // D-pad arms.
    struct Arm { const char* id; int slot; float x0, y0, x1, y1; };
    const Arm arms[4] = {
        { "##dup",    XB_DpadUp,    kDpadX - kDpadThick / 2, kDpadY - kDpadArm,       kDpadX + kDpadThick / 2, kDpadY - kDpadThick / 2 },
        { "##ddown",  XB_DpadDown,  kDpadX - kDpadThick / 2, kDpadY + kDpadThick / 2, kDpadX + kDpadThick / 2, kDpadY + kDpadArm       },
        { "##dleft",  XB_DpadLeft,  kDpadX - kDpadArm,       kDpadY - kDpadThick / 2, kDpadX - kDpadThick / 2, kDpadY + kDpadThick / 2 },
        { "##dright", XB_DpadRight, kDpadX + kDpadThick / 2, kDpadY - kDpadThick / 2, kDpadX + kDpadArm,       kDpadY + kDpadThick / 2 },
    };
    Hot hArm[4];
    for (int i = 0; i < 4; ++i) {
        hArm[i] = Zone(arms[i].id, lay.P(arms[i].x0, arms[i].y0), lay.P(arms[i].x1, arms[i].y1));
        Record(hit, hArm[i], PadControl::Button, arms[i].slot);
    }

    // Face buttons, in A B X Y order to match XButtonSlot.
    const ImVec2 faceC[4] = {
        lay.P(kFaceX,             kFaceY + kFaceOff),   // A
        lay.P(kFaceX + kFaceOff,  kFaceY),              // B
        lay.P(kFaceX - kFaceOff,  kFaceY),              // X
        lay.P(kFaceX,             kFaceY - kFaceOff),   // Y
    };
    const char* faceId[4] = { "##fa", "##fb", "##fx", "##fy" };
    Hot hFace[4];
    for (int i = 0; i < 4; ++i) {
        const float r = kFaceR * s;
        hFace[i] = Zone(faceId[i], ImVec2(faceC[i].x - r, faceC[i].y - r),
                                   ImVec2(faceC[i].x + r, faceC[i].y + r));
        Record(hit, hFace[i], PadControl::Button, XB_A + i);
    }

    // Shoulders and triggers.
    const ImVec2 lbA = lay.P(112, 60), lbB = lay.P(238, 92);
    const ImVec2 rbA = lay.P(402, 60), rbB = lay.P(528, 92);
    const ImVec2 ltA = lay.P(112, 12), ltB = lay.P(238, 52);
    const ImVec2 rtA = lay.P(402, 12), rtB = lay.P(528, 52);

    const Hot hLb = Zone("##lb", lbA, lbB); Record(hit, hLb, PadControl::Button, XB_LeftShoulder);
    const Hot hRb = Zone("##rb", rbA, rbB); Record(hit, hRb, PadControl::Button, XB_RightShoulder);
    const Hot hLt = Zone("##lt", ltA, ltB); Record(hit, hLt, PadControl::Axis,   XA_LeftTrigger);
    const Hot hRt = Zone("##rt", rtA, rtB); Record(hit, hRt, PadControl::Axis,   XA_RightTrigger);

    // Centre cluster.
    const ImVec2 backC = lay.P(275, 170), startC = lay.P(365, 170), guideC = lay.P(320, 170);
    const float  smallR = 13.0f * s, guideR = 18.0f * s;

    const Hot hBack  = Zone("##back",  ImVec2(backC.x - smallR, backC.y - smallR),
                                       ImVec2(backC.x + smallR, backC.y + smallR));
    const Hot hStart = Zone("##start", ImVec2(startC.x - smallR, startC.y - smallR),
                                       ImVec2(startC.x + smallR, startC.y + smallR));
    const Hot hGuide = Zone("##guide", ImVec2(guideC.x - guideR, guideC.y - guideR),
                                       ImVec2(guideC.x + guideR, guideC.y + guideR));
    Record(hit, hBack,  PadControl::Button, XB_Back);
    Record(hit, hStart, PadControl::Button, XB_Start);
    Record(hit, hGuide, PadControl::Button, XB_Guide);

    // ---- body --------------------------------------------------------------
    // Three overlapping rounded rectangles read as one controller silhouette.
    // They are deliberately not outlined: an outline on each would draw the
    // seams where they overlap, and the fill alone separates the body from the
    // background perfectly well.
    dl->AddRectFilled(lay.P( 80, 100), lay.P(560, 330), kBodyFill, 56.0f * s);
    dl->AddRectFilled(lay.P( 98, 210), lay.P(238, 390), kBodyFill, 66.0f * s);
    dl->AddRectFilled(lay.P(402, 210), lay.P(542, 390), kBodyFill, 66.0f * s);

    // ---- triggers ----------------------------------------------------------
    struct Trig { ImVec2 a, b; const char* label; float value; bool cap, hov; int slot; };
    const Trig trigs[2] = {
        { ltA, ltB, "LT", live ? view.out.bLeftTrigger  / 255.0f : 0.0f, capA(XA_LeftTrigger),  hLt.hovered, XA_LeftTrigger  },
        { rtA, rtB, "RT", live ? view.out.bRightTrigger / 255.0f : 0.0f, capA(XA_RightTrigger), hRt.hovered, XA_RightTrigger },
    };
    for (int i = 0; i < 2; ++i) {
        const Trig& t = trigs[i];
        dl->AddRectFilled(t.a, t.b, kCtrlFill, 6.0f * s);
        if (t.value > 0.001f) {
            // Fills upward, the way a trigger is pulled.
            const float top = t.b.y - (t.b.y - t.a.y) * t.value;
            dl->AddRectFilled(ImVec2(t.a.x, top), t.b, Lerp(kAccent, IM_COL32(255, 255, 255, 255), 0.15f), 6.0f * s);
        }
        dl->AddRect(t.a, t.b, kCtrlEdge, 6.0f * s, 0, 1.2f * s);

        char buf[32];
        const int raw = live ? (int)(t.value * 255.0f + 0.5f) : 0;
        snprintf(buf, sizeof(buf), "%s  %d", t.label, raw);
        CenteredText(dl, ImVec2((t.a.x + t.b.x) * 0.5f, (t.a.y + t.b.y) * 0.5f), buf,
                     t.value > 0.45f ? IM_COL32(20, 24, 32, 255) : kLabel, 1.0f);

        // The pre-deadzone reading, so a trigger sitting inside its deadzone is
        // visibly doing something rather than looking broken.
        const float rawIn = view.rawAxis[t.slot];
        if (live && rawIn > 0.004f && raw == 0) {
            dl->AddRectFilled(ImVec2(t.a.x, t.b.y - (t.b.y - t.a.y) * rawIn),
                              ImVec2(t.a.x + 4.0f * s, t.b.y), kRawDot, 2.0f * s);
        }
        Marker(dl, t.a, t.b, t.cap, t.hov, s);
    }

    // ---- shoulders ---------------------------------------------------------
    struct Bump { ImVec2 a, b; const char* label; int slot; bool hov; };
    const Bump bumps[2] = {
        { lbA, lbB, "LB", XB_LeftShoulder,  hLb.hovered },
        { rbA, rbB, "RB", XB_RightShoulder, hRb.hovered },
    };
    for (int i = 0; i < 2; ++i) {
        const Bump& b = bumps[i];
        const bool  on = down(b.slot);
        dl->AddRectFilled(b.a, b.b, on ? kAccent : kCtrlFill, 10.0f * s);
        dl->AddRect(b.a, b.b, kCtrlEdge, 10.0f * s, 0, 1.2f * s);
        CenteredText(dl, ImVec2((b.a.x + b.b.x) * 0.5f, (b.a.y + b.b.y) * 0.5f), b.label,
                     on ? IM_COL32(20, 24, 32, 255) : kLabel, 1.0f);
        Marker(dl, b.a, b.b, capB(b.slot), b.hov, s);
    }

    // ---- sticks ------------------------------------------------------------
    struct Stick {
        ImVec2 centre; float outX, outY, rawX, rawY;
        float dz, dzMax; bool click; int axisSlot; int thumbSlot;
        bool capAxis, capThumb, hov;
    };
    const Stick sticks[2] = {
        { lsC,
          live ? view.out.sThumbLX / 32767.0f : 0.0f, live ? view.out.sThumbLY / 32767.0f : 0.0f,
          live ? view.rawAxis[XA_LeftX] : 0.0f,       live ? view.rawAxis[XA_LeftY] : 0.0f,
          dz.leftStick, dz.leftStickMax, down(XB_LeftThumb), XA_LeftX, XB_LeftThumb,
          capA(XA_LeftX) || capA(XA_LeftY), capB(XB_LeftThumb), hLs.hovered },
        { rsC,
          live ? view.out.sThumbRX / 32767.0f : 0.0f, live ? view.out.sThumbRY / 32767.0f : 0.0f,
          live ? view.rawAxis[XA_RightX] : 0.0f,      live ? view.rawAxis[XA_RightY] : 0.0f,
          dz.rightStick, dz.rightStickMax, down(XB_RightThumb), XA_RightX, XB_RightThumb,
          capA(XA_RightX) || capA(XA_RightY), capB(XB_RightThumb), hRs.hovered },
    };

    for (int i = 0; i < 2; ++i) {
        const Stick& st = sticks[i];
        const float  R  = kStickR * s;

        dl->AddCircleFilled(st.centre, R, kCtrlFill, 48);

        // Deadzone as a shaded disc and saturation as a ring: tuning either one
        // becomes a matter of watching where the dot starts and stops counting.
        //
        // The disc is outlined as well as filled. At a typical deadzone of 0.15
        // it is only a few pixels across, and the output dot sits exactly on
        // top of it - without the ring the one thing these sliders are meant to
        // make visible is the one thing you cannot see.
        if (st.dz > 0.001f) dl->AddCircleFilled(st.centre, R * st.dz, kDeadzone, 40);

        // The ring that means "stick click".
        dl->AddCircle(st.centre, R, st.click ? kAccent : kCtrlEdge, 48,
                      st.click ? 4.0f * s : 1.6f * s);
        dl->AddCircle(st.centre, R * kStickRingFrom, kLabelDim, 48, 1.0f * s);

        // Raw position, dim, so the deadzone's effect is visible.
        if (fabsf(st.rawX) > 0.004f || fabsf(st.rawY) > 0.004f) {
            dl->AddCircleFilled(ImVec2(st.centre.x + st.rawX * R, st.centre.y - st.rawY * R),
                                4.0f * s, kRawDot, 16);
        }
        // Mapped output, bright.
        dl->AddCircleFilled(ImVec2(st.centre.x + st.outX * R, st.centre.y - st.outY * R),
                            6.0f * s, kAccent, 20);

        // The deadzone and saturation rings go on last, over the dots. At a
        // typical 0.15 the deadzone is the same size as the output dot, which
        // rests exactly on it - drawn underneath, the one thing these sliders
        // exist to make visible would be the one thing hidden.
        if (st.dz > 0.001f)    dl->AddCircle(st.centre, R * st.dz, kDeadzoneEdge, 40, 1.5f * s);
        if (st.dzMax < 0.999f) dl->AddCircle(st.centre, R * st.dzMax, kSatRing, 48, 1.5f * s);

        CenteredText(dl, ImVec2(st.centre.x, st.centre.y + R + 11.0f * s),
                     i == 0 ? "L3" : "R3", st.click ? kAccent : kLabelDim, 0.85f);

        const ImVec2 a(st.centre.x - R, st.centre.y - R);
        const ImVec2 b(st.centre.x + R, st.centre.y + R);
        Marker(dl, a, b, st.capAxis || st.capThumb, st.hov, s);
    }

    // ---- d-pad -------------------------------------------------------------
    // Drawn as a single cross so it reads as one control, with the pressed arm
    // lit on top. Four separate rectangles look like four buttons.
    const ImVec2 dpV0 = lay.P(kDpadX - kDpadThick / 2, kDpadY - kDpadArm);
    const ImVec2 dpV1 = lay.P(kDpadX + kDpadThick / 2, kDpadY + kDpadArm);
    const ImVec2 dpH0 = lay.P(kDpadX - kDpadArm,       kDpadY - kDpadThick / 2);
    const ImVec2 dpH1 = lay.P(kDpadX + kDpadArm,       kDpadY + kDpadThick / 2);
    dl->AddRectFilled(dpV0, dpV1, kCtrlFill, 5.0f * s);
    dl->AddRectFilled(dpH0, dpH1, kCtrlFill, 5.0f * s);

    for (int i = 0; i < 4; ++i) {
        const ImVec2 a  = lay.P(arms[i].x0, arms[i].y0);
        const ImVec2 b  = lay.P(arms[i].x1, arms[i].y1);
        if (down(arms[i].slot)) dl->AddRectFilled(a, b, kAccent, 4.0f * s);
        Marker(dl, a, b, capB(arms[i].slot), hArm[i].hovered, s);
    }
    dl->AddRect(dpV0, dpV1, kCtrlEdge, 5.0f * s, 0, 1.0f * s);
    dl->AddRect(dpH0, dpH1, kCtrlEdge, 5.0f * s, 0, 1.0f * s);

    // ---- face buttons ------------------------------------------------------
    const char* faceLabel[4] = { "A", "B", "X", "Y" };
    for (int i = 0; i < 4; ++i) {
        const bool  on = down(XB_A + i);
        const float r  = kFaceR * s;
        dl->AddCircleFilled(faceC[i], r, on ? kFace[i].active : kFace[i].idle, 32);
        dl->AddCircle(faceC[i], r, kCtrlEdge, 32, 1.2f * s);
        CenteredText(dl, faceC[i], faceLabel[i],
                     on ? IM_COL32(16, 20, 26, 255) : kLabel, 1.0f);
        Marker(dl, ImVec2(faceC[i].x - r, faceC[i].y - r),
                   ImVec2(faceC[i].x + r, faceC[i].y + r),
               capB(XB_A + i), hFace[i].hovered, s);
    }

    // ---- centre cluster ----------------------------------------------------
    struct Small { ImVec2 c; float r; const char* label; int slot; bool hov; };
    // Labels are plain ASCII on purpose: the bundled font has no glyphs beyond
    // Latin-1, and a missing one renders as a question mark rather than as
    // nothing, which looks like a bug.
    const Small smalls[3] = {
        { backC,  smallR, "",  XB_Back,  hBack.hovered  },
        { startC, smallR, "",  XB_Start, hStart.hovered },
        { guideC, guideR, "G", XB_Guide, hGuide.hovered },
    };
    for (int i = 0; i < 3; ++i) {
        const Small& sm = smalls[i];
        const bool   on = down(sm.slot);
        dl->AddCircleFilled(sm.c, sm.r, on ? kAccent : kCtrlFill, 24);
        dl->AddCircle(sm.c, sm.r, kCtrlEdge, 24, 1.2f * s);
        if (sm.label[0]) {
            CenteredText(dl, sm.c, sm.label, on ? IM_COL32(16, 20, 26, 255) : kLabelDim, 0.8f);
        }
        Marker(dl, ImVec2(sm.c.x - sm.r, sm.c.y - sm.r), ImVec2(sm.c.x + sm.r, sm.c.y + sm.r),
               capB(sm.slot), sm.hov, s);
    }

    CenteredText(dl, lay.P(275, 196), "Back",  kLabelDim, 0.75f);
    CenteredText(dl, lay.P(365, 196), "Start", kLabelDim, 0.75f);

    if (!live) {
        // Below the body, between the grips, where nothing else is drawn.
        CenteredText(dl, lay.P(320, 360), "no device selected", kLabelDim, 1.0f);
    }

    ImGui::SetCursorScreenPos(ImVec2(lay.origin.x, canvasEnd.y));
    return hit;
}

} // namespace gui
} // namespace vx
