// The exported XInput 1.3 surface.
//
// Every entry point is defensive: a game that passes a bad pointer or index
// must get a clean error rather than a crash inside a dropped-in DLL.

#include "../common/pad_manager.h"
#include "../common/xinput_defs.h"

extern "C" HMODULE g_hModule;

namespace {

// XInputEnable(FALSE) means "report neutral input"; games use it when they lose
// focus. Real XInput keeps returning success with a zeroed gamepad.
volatile LONG g_enabled = 1;

bool ValidIndex(DWORD index) { return index < VX_USER_MAX_COUNT; }

DWORD FetchState(DWORD index, XINPUT_STATE* state, bool includeGuide) {
    if (!ValidIndex(index)) return ERROR_BAD_ARGUMENTS;
    if (!state)             return ERROR_BAD_ARGUMENTS;

    vx::PadManager& pads = vx::PadManager::Instance();
    pads.EnsureStarted(g_hModule);

    XINPUT_GAMEPAD gamepad;
    DWORD          packet = 0;
    if (!pads.GetState((int)index, gamepad, packet)) return ERROR_DEVICE_NOT_CONNECTED;

    if (!InterlockedCompareExchange(&g_enabled, 0, 0)) {
        ZeroMemory(&gamepad, sizeof(gamepad));
    }
    if (!includeGuide) {
        gamepad.wButtons &= ~VX_GAMEPAD_GUIDE;
    }

    state->dwPacketNumber = packet;
    state->Gamepad        = gamepad;
    return ERROR_SUCCESS;
}

} // namespace

extern "C" {

DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
    return FetchState(dwUserIndex, pState, false);
}

// Undocumented ordinal 100. Identical to XInputGetState except that it also
// reports the Guide button.
DWORD WINAPI XInputGetStateEx(DWORD dwUserIndex, XINPUT_STATE* pState) {
    return FetchState(dwUserIndex, pState, true);
}

// Rumble is forwarded to DirectInput force feedback when the pad supports it.
// Devices that do not are still reported as success: a game must not treat a
// pad without motors as absent.
DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration) {
    if (!ValidIndex(dwUserIndex)) return ERROR_BAD_ARGUMENTS;
    if (!pVibration)              return ERROR_BAD_ARGUMENTS;

    vx::PadManager& pads = vx::PadManager::Instance();
    pads.EnsureStarted(g_hModule);

    if (!pads.IsConnected((int)dwUserIndex)) return ERROR_DEVICE_NOT_CONNECTED;

    // While disabled the pad must go quiet, matching real XInput behaviour when
    // a game loses focus.
    if (InterlockedCompareExchange(&g_enabled, 0, 0)) {
        pads.SetRumble((int)dwUserIndex, pVibration->wLeftMotorSpeed,
                       pVibration->wRightMotorSpeed);
    } else {
        pads.SetRumble((int)dwUserIndex, 0, 0);
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
                                   XINPUT_CAPABILITIES* pCapabilities) {
    if (!ValidIndex(dwUserIndex)) return ERROR_BAD_ARGUMENTS;
    if (!pCapabilities)           return ERROR_BAD_ARGUMENTS;
    if (dwFlags != 0 && dwFlags != VX_FLAG_GAMEPAD) return ERROR_BAD_ARGUMENTS;

    vx::PadManager& pads = vx::PadManager::Instance();
    pads.EnsureStarted(g_hModule);

    if (!pads.IsConnected((int)dwUserIndex)) return ERROR_DEVICE_NOT_CONNECTED;

    ZeroMemory(pCapabilities, sizeof(*pCapabilities));
    pCapabilities->Type    = VX_DEVTYPE_GAMEPAD;
    pCapabilities->SubType = VX_DEVSUBTYPE_GAMEPAD;
    pCapabilities->Flags   = 0;

    // Non-zero fields advertise which controls exist. Reporting a full standard
    // pad is what games expect and what the mapping can always produce.
    pCapabilities->Gamepad.wButtons      = 0xF3FF;
    pCapabilities->Gamepad.bLeftTrigger  = 0xFF;
    pCapabilities->Gamepad.bRightTrigger = 0xFF;
    // -64 is 0xFFC0 as a SHORT: the value real XInput reports, meaning the low
    // six bits of a thumb reading are not significant.
    pCapabilities->Gamepad.sThumbLX      = -64;
    pCapabilities->Gamepad.sThumbLY      = -64;
    pCapabilities->Gamepad.sThumbRX      = -64;
    pCapabilities->Gamepad.sThumbRY      = -64;

    // No vibration is reported, matching XInputSetState being a no-op.
    pCapabilities->Vibration.wLeftMotorSpeed  = 0;
    pCapabilities->Vibration.wRightMotorSpeed = 0;
    return ERROR_SUCCESS;
}

void WINAPI XInputEnable(BOOL enable) {
    InterlockedExchange(&g_enabled, enable ? 1 : 0);

    // Going quiet has to include the motors, or a pad keeps buzzing after the
    // game loses focus.
    if (!enable) {
        vx::PadManager& pads = vx::PadManager::Instance();
        for (int i = 0; i < VX_USER_MAX_COUNT; ++i) pads.SetRumble(i, 0, 0);
    }
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID* pDSoundRenderGuid,
                                             GUID* pDSoundCaptureGuid) {
    if (!ValidIndex(dwUserIndex)) return ERROR_BAD_ARGUMENTS;

    // The virtual pad has no headset, so report the null device.
    if (pDSoundRenderGuid)  *pDSoundRenderGuid  = GUID_NULL;
    if (pDSoundCaptureGuid) *pDSoundCaptureGuid = GUID_NULL;

    vx::PadManager& pads = vx::PadManager::Instance();
    pads.EnsureStarted(g_hModule);

    return pads.IsConnected((int)dwUserIndex) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType,
                                         XINPUT_BATTERY_INFORMATION* pBatteryInformation) {
    if (!ValidIndex(dwUserIndex))  return ERROR_BAD_ARGUMENTS;
    if (!pBatteryInformation)      return ERROR_BAD_ARGUMENTS;
    (void)devType;

    vx::PadManager& pads = vx::PadManager::Instance();
    pads.EnsureStarted(g_hModule);

    if (!pads.IsConnected((int)dwUserIndex)) return ERROR_DEVICE_NOT_CONNECTED;

    // A wired pad never shows a low-battery warning.
    pBatteryInformation->BatteryType  = VX_BATTERY_TYPE_WIRED;
    pBatteryInformation->BatteryLevel = VX_BATTERY_LEVEL_FULL;
    return ERROR_SUCCESS;
}

// Chatpad keystrokes: nothing to report, which is a documented, expected result.
DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved,
                                PXINPUT_KEYSTROKE pKeystroke) {
    (void)dwReserved;
    if (dwUserIndex != 0xFF && !ValidIndex(dwUserIndex)) return ERROR_BAD_ARGUMENTS;
    if (!pKeystroke) return ERROR_BAD_ARGUMENTS;

    vx::PadManager& pads = vx::PadManager::Instance();
    pads.EnsureStarted(g_hModule);

    if (dwUserIndex != 0xFF && !pads.IsConnected((int)dwUserIndex)) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return ERROR_EMPTY;
}

// XInput 1.4 replaced XInputGetDSoundAudioDeviceGuids with this. The virtual
// pad has no headset, so no device ids are reported.
DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId,
                                     UINT* pRenderCount, LPWSTR pCaptureDeviceId,
                                     UINT* pCaptureCount) {
    (void)pRenderDeviceId;
    (void)pCaptureDeviceId;
    if (!ValidIndex(dwUserIndex)) return ERROR_BAD_ARGUMENTS;

    if (pRenderCount)  *pRenderCount  = 0;
    if (pCaptureCount) *pCaptureCount = 0;

    vx::PadManager& pads = vx::PadManager::Instance();
    pads.EnsureStarted(g_hModule);

    return pads.IsConnected((int)dwUserIndex) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

// Ordinals 101-104. Present so anything resolving them by ordinal finds a valid
// function rather than a null pointer.
DWORD WINAPI XInputWaitForGuideButton(DWORD dwUserIndex, DWORD dwFlag, LPVOID pVoid) {
    (void)dwUserIndex; (void)dwFlag; (void)pVoid;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI XInputCancelGuideButtonWait(DWORD dwUserIndex) {
    (void)dwUserIndex;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI XInputPowerOffController(DWORD dwUserIndex) {
    (void)dwUserIndex;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI XInputGetBaseBusInformation(DWORD dwUserIndex, void* pBusinfo) {
    (void)dwUserIndex; (void)pBusinfo;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

} // extern "C"
