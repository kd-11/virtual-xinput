#include "di_device.h"

#include "log.h"

#include <cctype>
#include <cstdio>

namespace vx {
namespace {

typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

const wchar_t* kWindowClass = L"VirtualXInputHiddenWindow";

// Samples thrown away after acquiring, to let the device start reporting real
// values. At the default 250 Hz poll rate this is about 30 ms.
const int kPrimeSamples = 8;

// Period of the fallback periodic effect, in microseconds. 20 ms is fast enough
// to feel like a motor rather than a pulse.
const DWORD kPeriodicPeriodUs = 20000;

// DIJOYSTATE2 offset -> our axis index. Valid only once c_dfDIJoystick2 has
// been set as the data format, which is when DirectInput rewrites dwOfs.
int AxisIndexFromOffset(DWORD ofs) {
    if (ofs == FIELD_OFFSET(DIJOYSTATE2, lX))  return (int)DiAxis::X;
    if (ofs == FIELD_OFFSET(DIJOYSTATE2, lY))  return (int)DiAxis::Y;
    if (ofs == FIELD_OFFSET(DIJOYSTATE2, lZ))  return (int)DiAxis::Z;
    if (ofs == FIELD_OFFSET(DIJOYSTATE2, lRx)) return (int)DiAxis::Rx;
    if (ofs == FIELD_OFFSET(DIJOYSTATE2, lRy)) return (int)DiAxis::Ry;
    if (ofs == FIELD_OFFSET(DIJOYSTATE2, lRz)) return (int)DiAxis::Rz;
    if (ofs == (DWORD)FIELD_OFFSET(DIJOYSTATE2, rglSlider[0])) return (int)DiAxis::Slider0;
    if (ofs == (DWORD)FIELD_OFFSET(DIJOYSTATE2, rglSlider[1])) return (int)DiAxis::Slider1;
    return -1;
}

LONG RawAxisValue(const DIJOYSTATE2& js, int idx) {
    switch ((DiAxis)idx) {
        case DiAxis::X:       return js.lX;
        case DiAxis::Y:       return js.lY;
        case DiAxis::Z:       return js.lZ;
        case DiAxis::Rx:      return js.lRx;
        case DiAxis::Ry:      return js.lRy;
        case DiAxis::Rz:      return js.lRz;
        case DiAxis::Slider0: return js.rglSlider[0];
        case DiAxis::Slider1: return js.rglSlider[1];
        default:              return 0;
    }
}

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

LRESULT CALLBACK HiddenWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, msg, w, l);
}

} // namespace

// ---------------------------------------------------------------------------

bool MatchesPattern(const std::wstring& name, const std::string& pattern) {
    if (pattern.empty() || pattern == "*") return true;

    std::string hay = Narrow(name);
    for (size_t i = 0; i < hay.size(); ++i) hay[i] = (char)tolower((unsigned char)hay[i]);

    std::string needle = pattern;
    for (size_t i = 0; i < needle.size(); ++i)
        needle[i] = (char)tolower((unsigned char)needle[i]);

    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// DiDevice
// ---------------------------------------------------------------------------

DiDevice::DiDevice()
    : device_(nullptr), acquired_(false), discardSamples_(0),
      ffCapable_(false), ffWanted_(false), ffPeriodic_(false), ffEffectCount_(0),
      ffAxisFound_(0) {
    for (int i = 0; i < kAxisCount; ++i) {
        axisMin_[i] = -32768;
        axisMax_[i] = 32767;
    }
    for (int i = 0; i < 2; ++i) {
        ffEffects_[i]     = nullptr;
        ffAxisOffsets_[i] = 0;
        ffLast_[i]        = -1.0f;   // forces the first update to be sent
    }
}

DiDevice::~DiDevice() { Close(); }

void DiDevice::ReleaseEffects() {
    for (int i = 0; i < 2; ++i) {
        if (ffEffects_[i]) {
            ffEffects_[i]->Stop();
            ffEffects_[i]->Release();
            ffEffects_[i] = nullptr;
        }
        ffLast_[i] = -1.0f;
    }
    ffEffectCount_ = 0;
}

void DiDevice::Close() {
    ReleaseEffects();
    if (device_) {
        device_->Unacquire();
        device_->Release();
        device_ = nullptr;
    }
    acquired_ = false;
}

const char* DiDevice::ForceFeedbackKind() const {
    if (ffEffectCount_ == 0) return "none";
    return ffPeriodic_ ? "periodic (sine)" : "constant force";
}

BOOL CALLBACK DiDevice::EnumObjectsCb(LPCDIDEVICEOBJECTINSTANCEW obj, LPVOID ctx) {
    DiDevice* self = (DiDevice*)ctx;

    int idx = AxisIndexFromOffset(obj->dwOfs);
    if (idx < 0) return DIENUM_CONTINUE;

    self->caps_.axisPresent[idx] = true;

    // Ask for a symmetric range so normalisation is uniform across devices.
    DIPROPRANGE range;
    range.diph.dwSize       = sizeof(DIPROPRANGE);
    range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    range.diph.dwObj        = obj->dwType;
    range.diph.dwHow        = DIPH_BYID;
    range.lMin              = -32768;
    range.lMax              = 32767;
    self->device_->SetProperty(DIPROP_RANGE, &range.diph);

    // Deadzone and saturation are ours to apply, so neutralise the driver's.
    DIPROPDWORD dw;
    dw.diph.dwSize       = sizeof(DIPROPDWORD);
    dw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dw.diph.dwObj        = obj->dwType;
    dw.diph.dwHow        = DIPH_BYID;
    dw.dwData            = 0;
    self->device_->SetProperty(DIPROP_DEADZONE, &dw.diph);
    dw.dwData = 10000;
    self->device_->SetProperty(DIPROP_SATURATION, &dw.diph);

    // Read back what the device actually settled on; not every driver honours
    // the requested range, and normalising against a wrong range would skew
    // every reading from that axis.
    DIPROPRANGE actual;
    actual.diph.dwSize       = sizeof(DIPROPRANGE);
    actual.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    actual.diph.dwObj        = obj->dwType;
    actual.diph.dwHow        = DIPH_BYID;
    if (SUCCEEDED(self->device_->GetProperty(DIPROP_RANGE, &actual.diph)) &&
        actual.lMax > actual.lMin) {
        self->axisMin_[idx] = actual.lMin;
        self->axisMax_[idx] = actual.lMax;
    }

    return DIENUM_CONTINUE;
}

BOOL CALLBACK DiDevice::EnumFfAxesCb(LPCDIDEVICEOBJECTINSTANCEW obj, LPVOID ctx) {
    DiDevice* self = (DiDevice*)ctx;
    if (self->ffAxisFound_ >= 2) return DIENUM_STOP;

    self->ffAxisOffsets_[self->ffAxisFound_++] = obj->dwOfs;
    return DIENUM_CONTINUE;
}

void DiDevice::DiscoverCaps() {
    device_->EnumObjects(EnumObjectsCb, this, DIDFT_ABSAXIS);
}

bool DiDevice::CreateEffects() {
    ReleaseEffects();
    if (ffAxisFound_ == 0) return false;

    // Two actuators are driven independently as the strong and weak motor. With
    // only one, a single effect carries whichever motor is asking for more.
    int wanted = (ffAxisFound_ >= 2) ? 2 : 1;

    for (int i = 0; i < wanted; ++i) {
        DWORD axis = ffAxisOffsets_[i];
        LONG  dir  = 0;

        DIEFFECT eff;
        ZeroMemory(&eff, sizeof(eff));
        eff.dwSize                  = sizeof(DIEFFECT);
        eff.dwFlags                 = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        eff.dwDuration              = INFINITE;
        eff.dwSamplePeriod          = 0;
        eff.dwGain                  = DI_FFNOMINALMAX;
        eff.dwTriggerButton         = DIEB_NOTRIGGER;
        eff.dwTriggerRepeatInterval = 0;
        eff.cAxes                   = 1;
        eff.rgdwAxes                = &axis;
        eff.rglDirection            = &dir;
        eff.lpEnvelope              = nullptr;
        eff.dwStartDelay            = 0;

        DICONSTANTFORCE cf;
        cf.lMagnitude = 0;
        DIPERIODIC per;
        per.dwMagnitude = 0;
        per.lOffset     = 0;
        per.dwPhase     = 0;
        per.dwPeriod    = kPeriodicPeriodUs;

        HRESULT hr = E_FAIL;
        if (!ffPeriodic_) {
            eff.cbTypeSpecificParams  = sizeof(cf);
            eff.lpvTypeSpecificParams = &cf;
            hr = device_->CreateEffect(GUID_ConstantForce, &eff, &ffEffects_[i], nullptr);
        }

        // Plenty of pads implement only periodic effects, so fall back rather
        // than reporting no rumble at all.
        if (FAILED(hr)) {
            eff.cbTypeSpecificParams  = sizeof(per);
            eff.lpvTypeSpecificParams = &per;
            hr = device_->CreateEffect(GUID_Sine, &eff, &ffEffects_[i], nullptr);
            if (SUCCEEDED(hr)) ffPeriodic_ = true;
        }

        if (FAILED(hr) || !ffEffects_[i]) {
            ffEffects_[i] = nullptr;
            VXLOG("force feedback: effect %d could not be created (hr=0x%08lX)",
                  i, (unsigned long)hr);
            break;
        }
        ++ffEffectCount_;
    }

    if (ffEffectCount_ > 0) {
        VXLOG("force feedback: %d actuator(s), %s", ffEffectCount_, ForceFeedbackKind());
    }
    return ffEffectCount_ > 0;
}

bool DiDevice::Open(IDirectInput8W* di, const DeviceInfo& info, HWND hwnd,
                    bool wantForceFeedback) {
    Close();
    info_     = info;
    ffWanted_ = wantForceFeedback;

    HRESULT hr = di->CreateDevice(info.instanceGuid, &device_, nullptr);
    if (FAILED(hr) || !device_) {
        VXLOG("CreateDevice failed for '%s' (hr=0x%08lX)",
              Narrow(info.productName).c_str(), (unsigned long)hr);
        device_ = nullptr;
        return false;
    }

    hr = device_->SetDataFormat(&c_dfDIJoystick2);
    if (FAILED(hr)) {
        VXLOG("SetDataFormat failed (hr=0x%08lX)", (unsigned long)hr);
        Close();
        return false;
    }

    DIDEVCAPS dc;
    ZeroMemory(&dc, sizeof(dc));
    dc.dwSize = sizeof(dc);
    if (SUCCEEDED(device_->GetCapabilities(&dc))) {
        caps_.buttonCount = (int)dc.dwButtons;
        caps_.povCount    = (int)dc.dwPOVs;
        if (caps_.buttonCount > kMaxButtons) caps_.buttonCount = kMaxButtons;
        if (caps_.povCount > kMaxPovs)       caps_.povCount    = kMaxPovs;
        ffCapable_ = (dc.dwFlags & DIDC_FORCEFEEDBACK) != 0;
    }

    // Force feedback requires exclusive acquisition. Devices without it stay
    // non-exclusive, so enabling rumble costs nothing on pads that lack it.
    bool exclusive = ffWanted_ && ffCapable_;

    hr = device_->SetCooperativeLevel(
        hwnd, DISCL_BACKGROUND | (exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE));
    if (FAILED(hr) && exclusive) {
        // Something else already holds the device exclusively. Reading is more
        // important than rumble, so drop force feedback and carry on.
        VXLOG("exclusive mode refused (hr=0x%08lX), continuing without rumble",
              (unsigned long)hr);
        exclusive = false;
        hr = device_->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    }
    if (FAILED(hr)) {
        VXLOG("SetCooperativeLevel failed (hr=0x%08lX), continuing anyway",
              (unsigned long)hr);
    }

    DiscoverCaps();

    if (exclusive) {
        // Autocentring fights every effect we play, and must be changed before
        // the device is acquired.
        DIPROPDWORD ac;
        ac.diph.dwSize       = sizeof(DIPROPDWORD);
        ac.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        ac.diph.dwObj        = 0;
        ac.diph.dwHow        = DIPH_DEVICE;
        ac.dwData            = DIPROPAUTOCENTER_OFF;
        device_->SetProperty(DIPROP_AUTOCENTER, &ac.diph);

        ffAxisFound_ = 0;
        device_->EnumObjects(EnumFfAxesCb, this, DIDFT_AXIS | DIDFT_FFACTUATOR);
    }

    hr = device_->Acquire();
    acquired_ = SUCCEEDED(hr);
    if (acquired_) discardSamples_ = kPrimeSamples;

    if (exclusive && acquired_) CreateEffects();

    VXLOG("opened '%s' buttons=%d povs=%d acquired=%d ff=%s",
          Narrow(info.productName).c_str(), caps_.buttonCount, caps_.povCount,
          acquired_ ? 1 : 0,
          ForceFeedbackReady() ? ForceFeedbackKind()
                               : (ffCapable_ ? "capable, not enabled" : "unsupported"));
    return true;
}

bool DiDevice::ApplyMagnitude(int index, float value) {
    if (index < 0 || index >= 2 || !ffEffects_[index]) return false;

    LONG mag = (LONG)(Clamp01(value) * DI_FFNOMINALMAX);

    if (mag <= 0) {
        ffEffects_[index]->Stop();
        return true;
    }

    DIEFFECT eff;
    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);

    DICONSTANTFORCE cf;
    DIPERIODIC      per;

    if (ffPeriodic_) {
        per.dwMagnitude           = (DWORD)mag;
        per.lOffset               = 0;
        per.dwPhase               = 0;
        per.dwPeriod              = kPeriodicPeriodUs;
        eff.cbTypeSpecificParams  = sizeof(per);
        eff.lpvTypeSpecificParams = &per;
    } else {
        cf.lMagnitude             = mag;
        eff.cbTypeSpecificParams  = sizeof(cf);
        eff.lpvTypeSpecificParams = &cf;
    }

    HRESULT hr = ffEffects_[index]->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
    return SUCCEEDED(hr);
}

void DiDevice::SetRumble(float left, float right) {
    if (ffEffectCount_ <= 0) return;

    float a = Clamp01(left);
    float b = Clamp01(right);

    if (ffEffectCount_ == 1) {
        // One actuator: the louder motor wins, so a game asking for either
        // still produces a rumble.
        float combined = (a > b) ? a : b;
        if (combined != ffLast_[0]) {
            ApplyMagnitude(0, combined);
            ffLast_[0] = combined;
        }
        return;
    }

    if (a != ffLast_[0]) { ApplyMagnitude(0, a); ffLast_[0] = a; }
    if (b != ffLast_[1]) { ApplyMagnitude(1, b); ffLast_[1] = b; }
}

void DiDevice::StopRumble() {
    for (int i = 0; i < ffEffectCount_; ++i) {
        if (ffEffects_[i]) ffEffects_[i]->Stop();
        ffLast_[i] = 0.0f;
    }
}

bool DiDevice::Poll(RawState& out) {
    if (!device_) return false;

    if (!acquired_) {
        if (FAILED(device_->Acquire())) return false;
        acquired_       = true;
        discardSamples_ = kPrimeSamples;
    }

    HRESULT hr = device_->Poll();
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        acquired_ = false;
        if (FAILED(device_->Acquire())) return false;
        acquired_       = true;
        discardSamples_ = kPrimeSamples;
        device_->Poll();

        // Effects do not survive losing the device, so rebuild them.
        if (ffWanted_ && ffCapable_ && ffAxisFound_ > 0) CreateEffects();
    }

    DIJOYSTATE2 js;
    ZeroMemory(&js, sizeof(js));
    hr = device_->GetDeviceState(sizeof(js), &js);
    if (FAILED(hr)) {
        acquired_ = false;
        return false;
    }

    // Report no fresh data rather than a spurious neutral-looking sample.
    if (discardSamples_ > 0) {
        --discardSamples_;
        return false;
    }

    for (int i = 0; i < kAxisCount; ++i) {
        if (!caps_.axisPresent[i]) { out.axis[i] = 0.0f; continue; }

        double lo   = (double)axisMin_[i];
        double hi   = (double)axisMax_[i];
        double half = (hi - lo) * 0.5;
        if (half <= 0.0) { out.axis[i] = 0.0f; continue; }

        double v = ((double)RawAxisValue(js, i) - (lo + half)) / half;
        if (v < -1.0) v = -1.0;
        if (v >  1.0) v =  1.0;
        out.axis[i] = (float)v;
    }

    for (int i = 0; i < kMaxPovs; ++i) {
        DWORD p = js.rgdwPOV[i];
        out.pov[i] = (LOWORD(p) == 0xFFFF) ? -1 : (int)p;
    }

    for (int i = 0; i < kMaxButtons; ++i) {
        out.button[i] = (js.rgbButtons[i] & 0x80) != 0;
    }

    return true;
}

// ---------------------------------------------------------------------------
// DiSystem
// ---------------------------------------------------------------------------

DiSystem::DiSystem()
    : di_(nullptr), hwnd_(nullptr), dinputModule_(nullptr), wndClass_(0) {}

DiSystem::~DiSystem() { Shutdown(); }

bool DiSystem::CreateHiddenWindow() {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = HiddenWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;

    wndClass_ = RegisterClassExW(&wc);
    if (!wndClass_ && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        lastError_ = "RegisterClassEx failed";
        return false;
    }

    // DirectInput's cooperative level needs a top-level window owned by this
    // process. It is never shown, so the game is unaffected.
    hwnd_ = CreateWindowExW(0, kWindowClass, L"", WS_POPUP, 0, 0, 1, 1,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        lastError_ = "CreateWindowEx failed";
        return false;
    }
    return true;
}

bool DiSystem::Init() {
    if (di_) return true;

    if (!CreateHiddenWindow()) {
        VXLOG("hidden window creation failed: %s", lastError_.c_str());
        return false;
    }

    // Load dinput8 explicitly from the system directory. A plain import would
    // resolve against the game folder first, where a dinput8 wrapper may live.
    wchar_t sysDir[MAX_PATH];
    UINT    n = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        lastError_ = "GetSystemDirectory failed";
        return false;
    }
    std::wstring path = std::wstring(sysDir) + L"\\dinput8.dll";

    dinputModule_ = LoadLibraryW(path.c_str());
    if (!dinputModule_) {
        lastError_ = "cannot load dinput8.dll";
        VXLOG("%s", lastError_.c_str());
        return false;
    }

    PFN_DirectInput8Create create =
        (PFN_DirectInput8Create)GetProcAddress(dinputModule_, "DirectInput8Create");
    if (!create) {
        lastError_ = "DirectInput8Create not found";
        return false;
    }

    HRESULT hr = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                        IID_IDirectInput8W, (void**)&di_, nullptr);
    if (FAILED(hr) || !di_) {
        char buf[64];
        snprintf(buf, sizeof(buf), "DirectInput8Create failed (0x%08lX)", (unsigned long)hr);
        lastError_ = buf;
        VXLOG("%s", lastError_.c_str());
        di_ = nullptr;
        return false;
    }

    return true;
}

void DiSystem::Shutdown() {
    if (di_) { di_->Release(); di_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (wndClass_) {
        UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
        wndClass_ = 0;
    }
    // dinput8.dll is intentionally left loaded: unloading it during process
    // teardown has historically been a source of crashes.
    dinputModule_ = nullptr;
}

BOOL CALLBACK DiSystem::EnumDevicesCb(LPCDIDEVICEINSTANCEW inst, LPVOID ctx) {
    std::vector<DeviceInfo>* out = (std::vector<DeviceInfo>*)ctx;

    DeviceInfo info;
    info.instanceGuid = inst->guidInstance;
    info.productGuid  = inst->guidProduct;
    info.productName  = inst->tszProductName;
    info.instanceName = inst->tszInstanceName;
    out->push_back(info);

    return DIENUM_CONTINUE;
}

bool DiSystem::Enumerate(std::vector<DeviceInfo>& out) {
    out.clear();
    if (!di_) return false;

    HRESULT hr = di_->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevicesCb, &out,
                                  DIEDFL_ATTACHEDONLY);
    return SUCCEEDED(hr);
}

DiDevice* DiSystem::Open(const DeviceInfo& info, bool wantForceFeedback) {
    if (!di_) return nullptr;

    DiDevice* dev = new DiDevice();
    if (!dev->Open(di_, info, hwnd_, wantForceFeedback)) {
        delete dev;
        return nullptr;
    }
    return dev;
}

void DiSystem::PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace vx
