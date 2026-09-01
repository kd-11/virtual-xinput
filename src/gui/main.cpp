// virtual-xinput windowed configurator.
//
// The console configurator does everything this does and more, but it cannot
// show you a control moving while you decide what to bind it to, which is most
// of what configuring a gamepad actually is.

#include "app.h"
#include "platform.h"

#include <windows.h>

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    std::string err;

    if (!vx::gui::PlatformInit(L"virtual-xinput", 1180, 760, err)) {
        MessageBoxA(nullptr, err.c_str(), "virtual-xinput", MB_ICONERROR | MB_OK);
        return 1;
    }

    vx::gui::App app;
    if (!app.Init(err)) {
        vx::gui::PlatformShutdown();
        MessageBoxA(nullptr, err.c_str(), "virtual-xinput", MB_ICONERROR | MB_OK);
        return 1;
    }

    for (;;) {
        bool skipped = false;
        if (!vx::gui::PlatformBeginFrame(skipped)) break;
        if (skipped) continue;

        app.Frame();
        vx::gui::PlatformEndFrame();
    }

    app.Shutdown();
    vx::gui::PlatformShutdown();
    return 0;
}
