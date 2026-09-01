#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "config.h"
#include "mapping.h"
#include "xinput_defs.h"

namespace vx {

// Owns the polling thread and publishes a gamepad state per XInput slot.
//
// All DirectInput work happens on the worker thread; the exported XInput
// functions only ever read a snapshot under a lock, so a slow or disconnected
// device can never stall the game's input loop.
class PadManager {
public:
    static PadManager& Instance();

    // Starts the worker on first call. `hModule` locates the config file. Safe
    // to call from any thread and from every exported function.
    void EnsureStarted(HMODULE hModule);

    void Shutdown();

    // Returns false when nothing is mapped to that slot.
    bool GetState(int slot, XINPUT_GAMEPAD& gamepad, DWORD& packet);
    bool IsConnected(int slot);

    // Requested motor speeds, 0..65535 as XInput reports them. Recorded here
    // and applied by the worker, because every DirectInput call has to happen
    // on the thread that owns the device.
    void SetRumble(int slot, WORD left, WORD right);

private:
    PadManager();
    ~PadManager();
    PadManager(const PadManager&);
    PadManager& operator=(const PadManager&);

    struct SlotState {
        bool           connected;
        DWORD          packet;
        XINPUT_GAMEPAD pad;
        WORD           rumbleLeft;
        WORD           rumbleRight;
        bool           rumbleDirty;
        SlotState()
            : connected(false), packet(0), rumbleLeft(0), rumbleRight(0),
              rumbleDirty(false) {
            ZeroMemory(&pad, sizeof(pad));
        }
    };

    static DWORD WINAPI ThreadEntry(LPVOID param);
    void   ThreadMain();
    void   PublishSlot(int slot, const XINPUT_GAMEPAD& pad);
    void   ClearSlot(int slot);
    // Consumes a pending rumble request; false when nothing changed.
    bool   TakeRumble(int slot, WORD& left, WORD& right);

    HMODULE          module_;
    HANDLE           thread_;
    HANDLE           stopEvent_;
    HANDLE           readyEvent_;
    LONG             started_;
    bool             waitedForReady_;

    CRITICAL_SECTION lock_;
    SlotState        slots_[VX_USER_MAX_COUNT];

    Config           config_;
};

// Directory containing the given module, with no trailing separator. Pass null
// for the running executable.
std::wstring ModuleDirectory(HMODULE module);

} // namespace vx
