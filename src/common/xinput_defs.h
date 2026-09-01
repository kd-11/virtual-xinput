// Self-contained XInput ABI definitions.
//
// The wrapper DLL must never include <xinput.h> or link xinput.lib: it *is*
// xinput1_3.dll, so any XInput import would resolve back to itself. Everything
// the exports need is declared here instead.
#pragma once

#include <windows.h>

#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167
#endif

// XINPUT_GAMEPAD.wButtons
#define VX_GAMEPAD_DPAD_UP          0x0001
#define VX_GAMEPAD_DPAD_DOWN        0x0002
#define VX_GAMEPAD_DPAD_LEFT        0x0004
#define VX_GAMEPAD_DPAD_RIGHT       0x0008
#define VX_GAMEPAD_START            0x0010
#define VX_GAMEPAD_BACK             0x0020
#define VX_GAMEPAD_LEFT_THUMB       0x0040
#define VX_GAMEPAD_RIGHT_THUMB      0x0080
#define VX_GAMEPAD_LEFT_SHOULDER    0x0100
#define VX_GAMEPAD_RIGHT_SHOULDER   0x0200
// 0x0400 is the undocumented Guide bit. It is reported only through the
// ordinal-100 XInputGetStateEx export, never through XInputGetState.
#define VX_GAMEPAD_GUIDE            0x0400
#define VX_GAMEPAD_A                0x1000
#define VX_GAMEPAD_B                0x2000
#define VX_GAMEPAD_X                0x4000
#define VX_GAMEPAD_Y                0x8000

#define VX_DEVTYPE_GAMEPAD          0x01
#define VX_DEVSUBTYPE_GAMEPAD       0x01
#define VX_CAPS_FFB_SUPPORTED       0x0001

#define VX_FLAG_GAMEPAD             0x00000001

#define VX_BATTERY_DEVTYPE_GAMEPAD  0x00
#define VX_BATTERY_TYPE_WIRED       0x01
#define VX_BATTERY_LEVEL_FULL       0x03

#define VX_USER_MAX_COUNT           4

#pragma pack(push, 4)

typedef struct _XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD, *PXINPUT_GAMEPAD;

typedef struct _XINPUT_STATE {
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE, *PXINPUT_STATE;

typedef struct _XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} XINPUT_VIBRATION, *PXINPUT_VIBRATION;

typedef struct _XINPUT_CAPABILITIES {
    BYTE             Type;
    BYTE             SubType;
    WORD             Flags;
    XINPUT_GAMEPAD   Gamepad;
    XINPUT_VIBRATION Vibration;
} XINPUT_CAPABILITIES, *PXINPUT_CAPABILITIES;

typedef struct _XINPUT_BATTERY_INFORMATION {
    BYTE BatteryType;
    BYTE BatteryLevel;
} XINPUT_BATTERY_INFORMATION, *PXINPUT_BATTERY_INFORMATION;

typedef struct _XINPUT_KEYSTROKE {
    WORD  VirtualKey;
    WCHAR Unicode;
    WORD  Flags;
    BYTE  UserIndex;
    BYTE  HidCode;
} XINPUT_KEYSTROKE, *PXINPUT_KEYSTROKE;

#pragma pack(pop)
