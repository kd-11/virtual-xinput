#include "app.h"

#include "platform.h"

#include "../common/mapping.h"

#include <cstdio>
#include <cstring>

namespace vx {
namespace gui {

namespace {

const DWORD kEnumIntervalMs = 2000;
const DWORD kStatusHoldMs   = 4000;

// Slots offered for rebinding, in the order the "rebind everything" pass walks
// them. Guide is deliberately last and skippable: most pads have no such
// button, and being asked for one you do not have is confusing.
const int kAxisOrder[XA_Count] = {
    XA_LeftX, XA_LeftY, XA_RightX, XA_RightY, XA_LeftTrigger, XA_RightTrigger
};
const int kButtonOrder[] = {
    XB_A, XB_B, XB_X, XB_Y,
    XB_LeftShoulder, XB_RightShoulder,
    XB_Back, XB_Start,
    XB_LeftThumb, XB_RightThumb,
    XB_DpadUp, XB_DpadDown, XB_DpadLeft, XB_DpadRight,
    XB_Guide,
};
const int kButtonOrderCount = (int)(sizeof(kButtonOrder) / sizeof(kButtonOrder[0]));

void HelpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A -1..1 bar, centred, for a raw DirectInput axis.
void BipolarBar(float v, float width) {
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    const float  h  = ImGui::GetFrameHeight() * 0.62f;
    ImDrawList*  dl = ImGui::GetWindowDrawList();

    const ImU32 bg    = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 fill  = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    const ImU32 tick  = ImGui::GetColorU32(ImGuiCol_Separator);

    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), bg, 2.0f);

    const float mid = p.x + width * 0.5f;
    if (v >= 0.0f) dl->AddRectFilled(ImVec2(mid, p.y), ImVec2(mid + width * 0.5f * v, p.y + h), fill, 2.0f);
    else           dl->AddRectFilled(ImVec2(mid + width * 0.5f * v, p.y), ImVec2(mid, p.y + h), fill, 2.0f);

    dl->AddLine(ImVec2(mid, p.y), ImVec2(mid, p.y + h), tick);
    ImGui::Dummy(ImVec2(width, h));
}

} // namespace

// ---------------------------------------------------------------------------

App::App()
    : selected_(-1), dev_(nullptr), polled_(false),
      lastEnumTick_(0), statusTick_(0) {}

App::~App() { Shutdown(); }

bool App::Init(std::string& err) {
    if (!di_.Init()) {
        err = "DirectInput initialisation failed: " + di_.LastError();
        return false;
    }
    RefreshDevices();
    if (!devices_.empty()) SelectDevice(0);
    return true;
}

void App::Shutdown() {
    CloseDevice();
    di_.Shutdown();
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

void App::RefreshDevices() {
    // Remember what was selected by identity, not by position: enumeration
    // order shifts when a device is unplugged and a stale index would silently
    // select a different pad.
    GUID keep = GUID_NULL;
    if (selected_ >= 0 && selected_ < (int)devices_.size()) keep = devices_[selected_].instanceGuid;

    devices_.clear();
    di_.Enumerate(devices_);
    lastEnumTick_ = GetTickCount();

    if (keep != GUID_NULL) {
        for (size_t i = 0; i < devices_.size(); ++i) {
            if (devices_[i].instanceGuid == keep) { selected_ = (int)i; return; }
        }
        // The selected device is gone.
        CloseDevice();
        selected_ = -1;
    }
    if (selected_ >= (int)devices_.size()) selected_ = -1;
}

void App::CloseDevice() {
    if (dev_) { delete dev_; dev_ = nullptr; }
    view_.deviceOpen = false;
    CancelCapture();
}

void App::SelectDevice(int index) {
    if (index < 0 || index >= (int)devices_.size()) return;

    CloseDevice();
    selected_ = index;

    // Shared, non-exclusive: the configurator has no business locking a pad
    // away from anything else just to look at it. Force feedback is the one
    // thing that needs exclusivity, and it asks for it only when tested.
    dev_ = di_.Open(devices_[index], false);
    if (!dev_) {
        status_     = "could not open " + Narrow(devices_[index].productName);
        statusTick_ = GetTickCount();
        return;
    }

    profile_ = DeviceProfile();
    BuildAutoProfile(dev_->Caps(), profile_);
    view_.deviceOpen = true;

    status_     = "opened " + Narrow(devices_[index].productName);
    statusTick_ = GetTickCount();
}

void App::PollDevice() {
    polled_ = false;
    if (!dev_) return;

    polled_ = dev_->Poll(raw_);
    MapState(profile_, raw_, view_.out);
    ReadAxesRaw(profile_, raw_, view_.rawAxis);
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

void App::QueueCapture(PadControl kind, int index) {
    CaptureTarget t;
    t.kind  = kind;
    t.index = index;
    queue_.push_back(t);
}

void App::QueueAll() {
    queue_.clear();
    for (int i = 0; i < XA_Count; ++i)          QueueCapture(PadControl::Axis,   kAxisOrder[i]);
    for (int i = 0; i < kButtonOrderCount; ++i) QueueCapture(PadControl::Button, kButtonOrder[i]);
    detector_.Cancel();
}

void App::ClearBinding(PadControl kind, int index) {
    if (kind == PadControl::Axis && index >= 0 && index < XA_Count) {
        profile_.axes[index] = AxisMapping();
        // An explicit clear must survive: without this the next auto-detect
        // pass would helpfully put the binding straight back.
        profile_.autoAxes = false;
        status_ = std::string("cleared ") + XAxisName(index);
    } else if (kind == PadControl::Button && index >= 0 && index < XB_Count) {
        profile_.buttons[index] = ButtonMapping();
        profile_.autoButtons = false;
        profile_.autoDpad    = false;
        status_ = std::string("cleared ") + XButtonName(index);
    }
    statusTick_ = GetTickCount();
}

void App::CancelCapture() {
    queue_.clear();
    detector_.Cancel();
}

void App::SkipCapture() {
    if (!queue_.empty()) queue_.erase(queue_.begin());
    detector_.Cancel();
}

void App::AdvanceCapture() {
    if (queue_.empty() || !dev_) return;

    if (!detector_.IsActive() && !detector_.IsDone()) {
        detector_.Begin(dev_->Caps());
        return;
    }

    detector_.Feed(raw_, polled_);
    if (!detector_.IsDone()) return;

    const CaptureTarget t = queue_.front();
    const Detection&    d = detector_.Result();

    if (t.kind == PadControl::Axis) {
        const bool isTrigger = (t.index == XA_LeftTrigger || t.index == XA_RightTrigger);
        profile_.axes[t.index] = isTrigger ? TriggerFromDetection(d) : AxisFromDetection(d);
        profile_.autoAxes = false;
        status_ = std::string(XAxisName(t.index)) + "  ->  " + AxisSpecString(profile_.axes[t.index]);
    } else if (t.kind == PadControl::Button) {
        profile_.buttons[t.index] = ButtonFromDetection(d);
        profile_.autoButtons = false;
        profile_.autoDpad    = false;
        status_ = std::string(XButtonName(t.index)) + "  ->  " + ButtonSpecString(profile_.buttons[t.index]);
    }
    statusTick_ = GetTickCount();

    queue_.erase(queue_.begin());
    detector_.Cancel();
}

const char* App::CurrentPrompt() const {
    if (queue_.empty()) return nullptr;
    const CaptureTarget& t = queue_.front();
    if (t.kind == PadControl::Axis) return XAxisPrompt(t.index);

    const char* p = XButtonPrompt(t.index);
    if (p) return p;

    static char buf[64];
    snprintf(buf, sizeof(buf), "Press %s", XButtonName(t.index));
    return buf;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void App::Frame() {
    if (GetTickCount() - lastEnumTick_ > kEnumIntervalMs) RefreshDevices();

    PollDevice();
    AdvanceCapture();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !queue_.empty()) CancelCapture();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    DrawDeviceBar();
    ImGui::Separator();

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Pad")) {
            DrawPadTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Raw input")) {
            DrawRawTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Device")) {
            DrawDeviceTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Status line, pinned to the bottom.
    if (!status_.empty()) {
        if (GetTickCount() - statusTick_ > kStatusHoldMs) {
            status_.clear();
        } else {
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - 6.0f);
            ImGui::Separator();
            ImGui::TextDisabled("%s", status_.c_str());
        }
    }

    ImGui::End();
}

void App::DrawDeviceBar() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Controller");
    ImGui::SameLine();

    const std::string current =
        (selected_ >= 0 && selected_ < (int)devices_.size())
            ? Narrow(devices_[selected_].productName)
            : (devices_.empty() ? "no controllers found" : "select a controller");

    ImGui::SetNextItemWidth(360.0f);
    if (ImGui::BeginCombo("##device", current.c_str())) {
        for (int i = 0; i < (int)devices_.size(); ++i) {
            const bool sel = (i == selected_);
            if (ImGui::Selectable(Narrow(devices_[i].productName).c_str(), sel)) SelectDevice(i);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Rescan")) RefreshDevices();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (dev_ && !polled_) {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f), "device not responding");
    } else if (dev_) {
        const DeviceCaps& c = dev_->Caps();
        ImGui::TextDisabled("%d buttons, %d hat%s%s", c.buttonCount, c.povCount,
                            c.povCount == 1 ? "" : "s",
                            dev_->ForceFeedbackCapable() ? ", force feedback" : "");
    } else {
        ImGui::TextDisabled("nothing open");
    }
}

void App::DrawCaptureBanner() {
    // Always the same height, whether or not a capture is running. If this grew
    // when a capture started, clicking a control would shift every other
    // control out from under the cursor.
    const bool  busy = !queue_.empty();
    const float h    = ImGui::GetFrameHeight() * 2.4f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, busy ? ImVec4(0.16f, 0.20f, 0.28f, 1.0f)
                                                 : ImVec4(0.13f, 0.13f, 0.15f, 1.0f));
    ImGui::BeginChild("##capture", ImVec2(0, h), ImGuiChildFlags_Borders);

    if (!busy) {
        ImGui::TextDisabled("Click a control on the pad to bind it.");
        ImGui::TextDisabled("Right-click one to clear it.");
    } else {
        const InputDetector::Phase ph = detector_.CurrentPhase();
        if (ph == InputDetector::Phase::Sampling) {
            ImGui::TextDisabled("Hands off the controller - reading its resting position");
            ImGui::ProgressBar(detector_.SampleProgress(), ImVec2(-FLT_MIN, 6.0f), "");
        } else if (ph == InputDetector::Phase::Releasing) {
            ImGui::TextUnformatted("Got it - let go to continue");
        } else {
            ImGui::TextUnformatted(CurrentPrompt());
            ImGui::SameLine();
            if (queue_.size() > 1) ImGui::TextDisabled("(%d more)", (int)queue_.size() - 1);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::BeginDisabled(!busy);
    if (ImGui::Button("Skip")) SkipCapture();
    ImGui::SameLine();
    if (ImGui::Button("Cancel  (Esc)")) CancelCapture();
    ImGui::EndDisabled();

    ImGui::SameLine();
    HelpMarker("The resting position is sampled before each prompt. That is how a "
               "trigger that is a full axis resting at its minimum is told apart "
               "from one sharing an axis with the other trigger - which is why the "
               "controller must be left alone while the bar fills.");
}

void App::DrawPadTab() {
    if (!dev_) {
        ImGui::Spacing();
        ImGui::TextDisabled(devices_.empty()
            ? "No DirectInput controllers found. Plug one in; the list refreshes on its own."
            : "Select a controller above.");
        return;
    }

    const float padWidth = ImGui::GetContentRegionAvail().x * 0.58f;

    ImGui::BeginChild("##padcol", ImVec2(padWidth, 0));
    DrawCaptureBanner();

    const PadHit hit = DrawVirtualPad(
        view_, profile_.dz,
        queue_.empty() ? PadControl::None : queue_.front().kind,
        queue_.empty() ? -1 : queue_.front().index,
        ImGui::GetContentRegionAvail().x);

    if (hit.Any()) {
        if (hit.clear) {
            ClearBinding(hit.kind, hit.index);
        } else if (hit.kind == PadControl::Axis &&
                   (hit.index == XA_LeftX || hit.index == XA_RightX)) {
            // A stick is two destinations. Clicking it asks for both in turn
            // rather than making the user find two separate hit targets.
            queue_.clear();
            detector_.Cancel();
            QueueCapture(PadControl::Axis, hit.index);
            QueueCapture(PadControl::Axis, hit.index == XA_LeftX ? XA_LeftY : XA_RightY);
        } else {
            queue_.clear();
            detector_.Cancel();
            QueueCapture(hit.kind, hit.index);
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Rebind everything")) QueueAll();
    ImGui::SameLine();
    if (ImGui::Button("Reset to auto-detect")) {
        profile_ = DeviceProfile();
        BuildAutoProfile(dev_->Caps(), profile_);
        CancelCapture();
        status_     = "mapping reset to auto-detect";
        statusTick_ = GetTickCount();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##sidecol", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    DrawDeadzones();
    ImGui::Spacing();
    DrawMappingTable();
    ImGui::EndChild();
}

void App::DrawDeadzones() {
    ImGui::SeparatorText("Deadzones");
    ImGui::SameLine();
    HelpMarker("Stick deadzones are radial: they measure total deflection in any "
               "direction, which is what actually stops a worn stick from drifting. "
               "The shaded disc on each stick is the deadzone and the outer ring is "
               "the saturation point; the dim dot is the raw reading, the bright one "
               "is what the game receives.");

    ImGui::PushItemWidth(-110.0f);
    ImGui::SliderFloat("Left stick",  &profile_.dz.leftStick,  0.0f, 0.60f, "%.2f");
    ImGui::SliderFloat("Right stick", &profile_.dz.rightStick, 0.0f, 0.60f, "%.2f");
    ImGui::SliderFloat("Trigger",     &profile_.dz.trigger,    0.0f, 0.60f, "%.2f");
    ImGui::Spacing();
    ImGui::SliderFloat("Left max",    &profile_.dz.leftStickMax,  0.40f, 1.0f, "%.2f");
    ImGui::SliderFloat("Right max",   &profile_.dz.rightStickMax, 0.40f, 1.0f, "%.2f");
    ImGui::SliderFloat("Trigger max", &profile_.dz.triggerMax,    0.40f, 1.0f, "%.2f");
    ImGui::PopItemWidth();

    // A saturation point below the deadzone divides by zero in the rescale and
    // makes the stick behave as a switch. Keep them apart.
    if (profile_.dz.leftStickMax  <= profile_.dz.leftStick  + 0.05f)
        profile_.dz.leftStickMax  = profile_.dz.leftStick  + 0.05f;
    if (profile_.dz.rightStickMax <= profile_.dz.rightStick + 0.05f)
        profile_.dz.rightStickMax = profile_.dz.rightStick + 0.05f;
    if (profile_.dz.triggerMax    <= profile_.dz.trigger    + 0.05f)
        profile_.dz.triggerMax    = profile_.dz.trigger    + 0.05f;
}

void App::DrawMappingTable() {
    ImGui::SeparatorText("Mapping");
    ImGui::TextDisabled("Click a row to rebind, right-click to clear.");
    ImGui::Spacing();

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##map", 2, flags)) return;

    ImGui::TableSetupColumn("XInput", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableHeadersRow();

    auto row = [&](const char* name, const std::string& spec, PadControl kind, int index) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        ImGui::PushID(kind == PadControl::Axis ? index : (XA_Count + index));
        const bool active = !queue_.empty() && queue_.front().kind == kind &&
                            queue_.front().index == index;
        if (ImGui::Selectable(name, active, ImGuiSelectableFlags_SpanAllColumns)) {
            queue_.clear();
            detector_.Cancel();
            QueueCapture(kind, index);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ClearBinding(kind, index);
        ImGui::PopID();

        ImGui::TableNextColumn();
        const bool unset = (spec == "none");
        if (unset) ImGui::TextDisabled("%s", spec.c_str());
        else       ImGui::TextUnformatted(spec.c_str());
    };

    for (int i = 0; i < XA_Count; ++i) {
        const int slot = kAxisOrder[i];
        row(XAxisName(slot), AxisSpecString(profile_.axes[slot]), PadControl::Axis, slot);
    }
    for (int i = 0; i < kButtonOrderCount; ++i) {
        const int slot = kButtonOrder[i];
        row(XButtonName(slot), ButtonSpecString(profile_.buttons[slot]), PadControl::Button, slot);
    }

    ImGui::EndTable();
}

void App::DrawRawTab() {
    if (!dev_) {
        ImGui::Spacing();
        ImGui::TextDisabled("Select a controller above.");
        return;
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Exactly what the device reports, before any mapping. "
                        "When something is wrong this is the view that tells the truth.");
    ImGui::Spacing();

    const DeviceCaps& caps = dev_->Caps();

    ImGui::BeginChild("##axes", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0));
    ImGui::SeparatorText("Axes");
    for (int i = 0; i < kAxisCount; ++i) {
        if (!caps.axisPresent[i]) continue;
        ImGui::Text("%-8s", DiAxisName((DiAxis)i));
        ImGui::SameLine(90.0f);
        BipolarBar(raw_.axis[i], ImGui::GetContentRegionAvail().x - 70.0f);
        ImGui::SameLine();
        ImGui::Text("%+.3f", raw_.axis[i]);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Hats");
    if (caps.povCount == 0) {
        ImGui::TextDisabled("none");
    } else {
        for (int i = 0; i < caps.povCount; ++i) {
            if (raw_.pov[i] < 0) ImGui::Text("pov%d    centred", i);
            else                 ImGui::Text("pov%d    %d deg", i, raw_.pov[i] / 100);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##buttons", ImVec2(0, 0));
    ImGui::SeparatorText("Buttons");

    const int   perRow = 8;
    const float cell   = ImGui::GetFrameHeight() * 1.05f;
    ImDrawList* dl     = ImGui::GetWindowDrawList();

    for (int i = 0; i < caps.buttonCount; ++i) {
        if (i % perRow != 0) ImGui::SameLine();

        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const bool   on = raw_.button[i];
        dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell),
                          on ? IM_COL32(92, 170, 255, 255) : ImGui::GetColorU32(ImGuiCol_FrameBg),
                          3.0f);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", i);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(p.x + (cell - ts.x) * 0.5f, p.y + (cell - ts.y) * 0.5f),
                    on ? IM_COL32(16, 20, 26, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    buf);
        ImGui::Dummy(ImVec2(cell, cell));
    }
    if (caps.buttonCount == 0) ImGui::TextDisabled("none");
    ImGui::EndChild();
}

void App::DrawDeviceTab() {
    if (!dev_) {
        ImGui::Spacing();
        ImGui::TextDisabled("Select a controller above.");
        return;
    }

    const DeviceInfo& info = dev_->Info();
    const DeviceCaps& caps = dev_->Caps();

    ImGui::Spacing();
    ImGui::SeparatorText("Identity");
    ImGui::Text("Product   %s", Narrow(info.productName).c_str());
    ImGui::Text("Instance  %s", Narrow(info.instanceName).c_str());
    ImGui::Text("GUID      %s", GuidToString(info.instanceGuid).c_str());

    ImGui::Spacing();
    ImGui::SeparatorText("Reports");
    std::string axes;
    for (int i = 0; i < kAxisCount; ++i) {
        if (!caps.axisPresent[i]) continue;
        if (!axes.empty()) axes += " ";
        axes += DiAxisName((DiAxis)i);
    }
    ImGui::Text("Axes      %s", axes.empty() ? "none" : axes.c_str());
    ImGui::Text("Buttons   %d", caps.buttonCount);
    ImGui::Text("Hats      %d", caps.povCount);

    ImGui::Spacing();
    ImGui::SeparatorText("Force feedback");
    if (!dev_->ForceFeedbackCapable()) {
        ImGui::TextDisabled("Not supported by this device.");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextDisabled(
            "Many pads only rumble through their own vendor driver, which DirectInput "
            "cannot reach - a DualShock 4 on the stock Windows driver reports none at "
            "all. Wheels and older gamepads with real DirectInput force feedback work.");
        ImGui::PopTextWrapPos();
    } else {
        ImGui::Text("Supported: %s", dev_->ForceFeedbackKind());
        ImGui::TextDisabled("Testing it needs an exclusive claim on the device, which "
                            "is not taken while merely viewing input.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Summary");
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 44.0f);
    ImGui::TextUnformatted(DescribeProfile(profile_).c_str());
    ImGui::PopTextWrapPos();
}

} // namespace gui
} // namespace vx
