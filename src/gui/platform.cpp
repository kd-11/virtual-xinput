#include "platform.h"

#include <d3d11.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tchar.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// Declared by imgui_impl_win32.h but not exported into a header we include.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace vx {
namespace gui {

namespace {

const wchar_t* kWindowClass = L"VirtualXInputConfigurator";

ID3D11Device*           g_device        = nullptr;
ID3D11DeviceContext*    g_context       = nullptr;
IDXGISwapChain*         g_swapChain     = nullptr;
ID3D11RenderTargetView* g_renderTarget  = nullptr;
HWND                    g_hwnd          = nullptr;
WNDCLASSEXW             g_wc            = {};
bool                    g_quit          = false;
bool                    g_occluded      = false;

// Set from the message handler, acted on at the top of the next frame. Resizing
// the swap chain from inside WM_SIZE while ImGui holds a render target view is
// how this crashes, so the two are kept apart.
UINT g_resizeWidth  = 0;
UINT g_resizeHeight = 0;

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) return;
    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
    backBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = 0;   // track the window
    sd.BufferDesc.Height                  = 0;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.SampleDesc.Quality                 = 0;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL       got      = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);

    // A machine with no usable GPU - a VM, a remote desktop session, a broken
    // driver - still needs to be able to configure a gamepad, so fall back to
    // the software rasteriser rather than refusing to start.
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    }
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context   = nullptr; }
    if (g_device)    { g_device->Release();    g_device    = nullptr; }
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_resizeWidth  = (UINT)LOWORD(lParam);
        g_resizeHeight = (UINT)HIWORD(lParam);
        return 0;

    case WM_SYSCOMMAND:
        // Swallow the Alt+Space system menu; it steals input mid-configuration.
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;

    case WM_DESTROY:
        g_quit = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// A dark, low-contrast theme. The pad preview is the thing to look at; the
// chrome around it should not compete with it.
void ApplyStyle() {
    ImGui::StyleColorsDark();

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 4.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 3.0f;
    s.ScrollbarRounding = 3.0f;
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(7, 4);
    s.ItemSpacing       = ImVec2(8, 6);
    s.WindowBorderSize  = 0.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.22f, 0.30f, 0.42f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.27f, 0.38f, 0.53f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.20f, 0.24f, 0.30f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.27f, 0.34f, 0.44f, 1.00f);
    c[ImGuiCol_Tab]             = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TabSelected]     = ImVec4(0.22f, 0.30f, 0.42f, 1.00f);
    c[ImGuiCol_TabHovered]      = ImVec4(0.27f, 0.38f, 0.53f, 1.00f);
    c[ImGuiCol_Separator]       = ImVec4(0.26f, 0.26f, 0.30f, 1.00f);
}

} // namespace

bool PlatformInit(const wchar_t* title, int width, int height, std::string& err) {
    ImGui_ImplWin32_EnableDpiAwareness();

    ZeroMemory(&g_wc, sizeof(g_wc));
    g_wc.cbSize        = sizeof(g_wc);
    g_wc.style         = CS_CLASSDC;
    g_wc.lpfnWndProc   = WndProc;
    g_wc.hInstance     = GetModuleHandleW(nullptr);
    g_wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    g_wc.lpszClassName = kWindowClass;
    g_wc.hIcon         = LoadIconW(g_wc.hInstance, MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&g_wc)) {
        err = "RegisterClassEx failed";
        return false;
    }

    // Scale the default window to the monitor, so a 4K display does not get a
    // postage stamp and a 1366x768 laptop does not get a window it cannot fit.
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(GetDesktopWindow());
    const int   w   = (int)(width * dpi);
    const int   h   = (int)(height * dpi);

    g_hwnd = CreateWindowExW(0, kWindowClass, title, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                             nullptr, nullptr, g_wc.hInstance, nullptr);
    if (!g_hwnd) {
        UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
        err = "CreateWindowEx failed";
        return false;
    }

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        DestroyWindow(g_hwnd);
        UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
        g_hwnd = nullptr;
        err = "Direct3D 11 device creation failed. The machine has no usable "
              "Direct3D 11 driver; use virtual-xinput-config.exe instead.";
        return false;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // No imgui.ini. The app has its own layout and writing a stray file into
    // whatever folder it was launched from would be rude for a portable tool.
    io.IniFilename = nullptr;

    ApplyStyle();
    ImGui::GetStyle().ScaleAllSizes(dpi);
    ImGui::GetIO().FontGlobalScale = dpi;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    return true;
}

void PlatformShutdown() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
}

bool PlatformBeginFrame(bool& skipped) {
    skipped = false;

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) g_quit = true;
    }
    if (g_quit) return false;

    // Minimised or fully hidden: Present has told us it is not reaching the
    // screen, so sleep instead of burning a core rendering to nothing.
    if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
        Sleep(16);
        skipped = true;
        return true;
    }
    g_occluded = false;

    if (g_resizeWidth != 0 && g_resizeHeight != 0) {
        CleanupRenderTarget();
        g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
        g_resizeWidth = g_resizeHeight = 0;
        CreateRenderTarget();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    return true;
}

void PlatformEndFrame() {
    ImGui::Render();

    const float clear[4] = { 0.07f, 0.07f, 0.08f, 1.0f };
    g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
    g_context->ClearRenderTargetView(g_renderTarget, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // Vsync on: this is a configuration tool sitting open beside other things,
    // and it has no reason to render faster than the display.
    const HRESULT hr = g_swapChain->Present(1, 0);
    g_occluded = (hr == DXGI_STATUS_OCCLUDED);
}

HWND PlatformWindow() { return g_hwnd; }

// ---------------------------------------------------------------------------
// Shell integration
// ---------------------------------------------------------------------------

bool PickFolder(const wchar_t* title, const std::wstring& start, std::wstring& out) {
    // COM is initialised here rather than at startup because this is the only
    // thing in the app that wants it. RPC_E_CHANGED_MODE means somebody else
    // got there first with a different apartment, which is fine - we just must
    // not uninitialise on the way out in that case.
    const HRESULT init   = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool    ownCom = SUCCEEDED(init);

    bool          picked = false;
    IFileDialog*  dlg    = nullptr;

    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD flags = 0;
        if (SUCCEEDED(dlg->GetOptions(&flags))) {
            dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                            FOS_PATHMUSTEXIST);
        }
        if (title) dlg->SetTitle(title);

        if (!start.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr,
                                                      IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
            }
        }

        if (SUCCEEDED(dlg->Show(g_hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out    = path;
                    picked = true;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }

    if (ownCom) CoUninitialize();
    return picked;
}

void RevealFolder(const std::wstring& path) {
    ShellExecuteW(g_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace gui
} // namespace vx
