#pragma once

#include "../common/detect.h"
#include "../common/di_device.h"

#include "draw_pad.h"

#include <string>
#include <vector>

namespace vx {
namespace gui {

// One queued "press the thing you want here" step.
struct CaptureTarget {
    PadControl kind  = PadControl::None;
    int        index = -1;
};

class App {
public:
    App();
    ~App();

    bool Init(std::string& err);
    void Shutdown();

    // One frame: poll the open device, advance any capture in progress, draw.
    void Frame();

private:
    void PollDevice();
    void AdvanceCapture();
    void RefreshDevices();
    void SelectDevice(int index);
    void CloseDevice();

    void QueueCapture(PadControl kind, int index);
    void QueueAll();
    void ClearBinding(PadControl kind, int index);
    void CancelCapture();
    void SkipCapture();

    void DrawMenuBar();
    void DrawDeviceBar();
    void DrawPadTab();
    void DrawRawTab();
    void DrawDeviceTab();
    void DrawCaptureBanner();
    void DrawMappingTable();
    void DrawDeadzones();

    const char* CurrentPrompt() const;

    DiSystem                di_;
    std::vector<DeviceInfo> devices_;
    int                     selected_;
    DiDevice*               dev_;

    RawState      raw_;
    bool          polled_;
    DeviceProfile profile_;
    PadView       view_;

    InputDetector              detector_;
    std::vector<CaptureTarget> queue_;

    // Devices are re-enumerated periodically so a pad plugged in while the
    // window is open turns up without the user hunting for a refresh button.
    DWORD lastEnumTick_;

    std::string status_;
    DWORD       statusTick_;
};

} // namespace gui
} // namespace vx
