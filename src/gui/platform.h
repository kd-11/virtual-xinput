#pragma once

#include <windows.h>

#include <string>

// Window, Direct3D 11 device and ImGui lifecycle. Nothing in here knows what
// the application draws; it exists so app.cpp contains no boilerplate.
namespace vx {
namespace gui {

// Creates the window, the device and the ImGui context. Returns false with a
// human-readable reason on failure.
bool PlatformInit(const wchar_t* title, int width, int height, std::string& err);

void PlatformShutdown();

// Pumps messages and starts an ImGui frame. Returns false when the user has
// asked to close the window, at which point the caller must stop drawing and
// break out of its loop.
//
// When the window is minimised or otherwise occluded this idles rather than
// spinning, and returns true without having started a frame - `skipped` says
// so, and the caller must not draw or call PlatformEndFrame.
bool PlatformBeginFrame(bool& skipped);

void PlatformEndFrame();

HWND PlatformWindow();

} // namespace gui
} // namespace vx
