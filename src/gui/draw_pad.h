#pragma once

#include "../common/config.h"
#include "../common/xinput_defs.h"

#include "imgui.h"

namespace vx {
namespace gui {

// Which kind of XInput destination a hit refers to. Axis indices are
// XAxisSlot, button indices are XButtonSlot.
enum class PadControl { None, Axis, Button };

struct PadHit {
    PadControl kind  = PadControl::None;
    int        index = -1;
    bool       clear = false;   // right-clicked: unbind rather than rebind

    bool Any() const { return kind != PadControl::None; }
};

// Everything the pad drawing needs to know about the current state.
//
// Both the mapped output and the pre-deadzone input are carried, because the
// point of drawing a deadzone ring is to show the gap between the two.
struct PadView {
    XINPUT_GAMEPAD out;                  // after mapping and deadzones
    float          rawAxis[XA_Count];    // before deadzones
    bool           deviceOpen = false;

    PadView() {
        ZeroMemory(&out, sizeof(out));
        for (int i = 0; i < XA_Count; ++i) rawAxis[i] = 0.0f;
    }
};

// Width divided by height. The pad is drawn from a fixed design space, so the
// caller can use this to pick a width that also fits the height available
// rather than overflowing and scrolling.
float PadAspectRatio();

// Draws the virtual Xbox pad at the current cursor position, `width` wide, and
// returns whatever control the user clicked. Height follows from the aspect
// ratio; the drawing never squashes to fit.
//
// `highlightKind`/`highlightIndex` mark the control currently being captured,
// which pulses so it is obvious what the app is waiting for.
PadHit DrawVirtualPad(const PadView& view, const Deadzone& dz,
                      PadControl highlightKind, int highlightIndex,
                      float width);

// Human-readable name of an XInput destination, e.g. "A" or "Left stick X".
const char* XAxisName(int slot);
const char* XButtonName(int slot);

// The instruction shown while capturing one, e.g. "push the LEFT stick RIGHT".
const char* XAxisPrompt(int slot);
const char* XButtonPrompt(int slot);

} // namespace gui
} // namespace vx
