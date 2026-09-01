#include "pad_manager.h"

#include "di_device.h"
#include "log.h"

#include <cstdio>

namespace vx {
namespace {

// How long the first XInput call will wait for the worker's initial scan.
// Bounded on purpose: if a game calls into us from static initialisation while
// holding the loader lock, our worker cannot load dinput8.dll until that lock
// is released, and an unbounded wait would deadlock. Timing out here simply
// reports "no controller" for a moment; the next call succeeds.
const DWORD kReadyTimeoutMs = 2000;

// Re-enumeration interval, so a pad plugged in after the game started is
// picked up without restarting.
const DWORD kRescanIntervalMs = 2000;

// Upper bound on how long the initial scan waits for a device to yield its
// first sample before giving up and letting the game proceed regardless.
const DWORD kReadySampleTimeoutMs = 500;

bool SameGamepad(const XINPUT_GAMEPAD& a, const XINPUT_GAMEPAD& b) {
    return a.wButtons == b.wButtons &&
           a.bLeftTrigger == b.bLeftTrigger && a.bRightTrigger == b.bRightTrigger &&
           a.sThumbLX == b.sThumbLX && a.sThumbLY == b.sThumbLY &&
           a.sThumbRX == b.sThumbRX && a.sThumbRY == b.sThumbRY;
}

struct OpenPad {
    DiDevice*     device;
    DeviceProfile profile;
    int           slot;
    OpenPad() : device(nullptr), slot(-1) {}
};

// Picks the profile that should drive a device: an exact GUID match wins over a
// name pattern, and a catch-all pattern is the last resort.
const DeviceProfile* SelectProfile(const std::vector<DeviceProfile>& profiles,
                                   const DeviceInfo& info) {
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].hasGuid &&
            IsEqualGUID(profiles[i].guid, info.instanceGuid)) {
            return &profiles[i];
        }
    }
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].hasGuid) continue;
        if (profiles[i].match != "*" && MatchesPattern(info.productName, profiles[i].match)) {
            return &profiles[i];
        }
    }
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].hasGuid) continue;
        if (profiles[i].match == "*") return &profiles[i];
    }
    return nullptr;
}

} // namespace

std::wstring ModuleDirectory(HMODULE module) {
    wchar_t path[MAX_PATH];
    DWORD   n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();

    std::wstring s(path, n);
    size_t       slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return s.substr(0, slash);
}

// ---------------------------------------------------------------------------

PadManager::PadManager()
    : module_(nullptr), thread_(nullptr), stopEvent_(nullptr), readyEvent_(nullptr),
      started_(0), waitedForReady_(false) {
    InitializeCriticalSection(&lock_);
}

PadManager::~PadManager() {
    Shutdown();
    DeleteCriticalSection(&lock_);
}

PadManager& PadManager::Instance() {
    // Deliberately leaked. A static object would register a destructor with
    // atexit, and that destructor joins the polling thread - which, in a DLL,
    // runs during process detach while the loader lock is held, and would
    // deadlock. Letting the OS reclaim everything is the safe choice.
    static PadManager* instance = new PadManager();
    return *instance;
}

DWORD WINAPI PadManager::ThreadEntry(LPVOID param) {
    ((PadManager*)param)->ThreadMain();
    return 0;
}

void PadManager::EnsureStarted(HMODULE hModule) {
    if (InterlockedCompareExchange(&started_, 1, 0) == 0) {
        module_     = hModule;
        stopEvent_  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_     = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
        if (!thread_) {
            VXLOG("failed to create polling thread");
            return;
        }
    }

    // Block the very first query briefly so the game does not see an empty slot
    // on startup and conclude no controller is present.
    if (!waitedForReady_ && readyEvent_) {
        WaitForSingleObject(readyEvent_, kReadyTimeoutMs);
        waitedForReady_ = true;
    }
}

void PadManager::Shutdown() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvent_)  { CloseHandle(stopEvent_);  stopEvent_  = nullptr; }
    if (readyEvent_) { CloseHandle(readyEvent_); readyEvent_ = nullptr; }
}

void PadManager::PublishSlot(int slot, const XINPUT_GAMEPAD& pad) {
    EnterCriticalSection(&lock_);
    SlotState& s = slots_[slot];
    if (!s.connected || !SameGamepad(s.pad, pad)) {
        s.pad = pad;
        s.packet++;
    }
    s.connected = true;
    LeaveCriticalSection(&lock_);
}

void PadManager::ClearSlot(int slot) {
    EnterCriticalSection(&lock_);
    slots_[slot].connected = false;
    ZeroMemory(&slots_[slot].pad, sizeof(XINPUT_GAMEPAD));
    LeaveCriticalSection(&lock_);
}

bool PadManager::GetState(int slot, XINPUT_GAMEPAD& gamepad, DWORD& packet) {
    if (slot < 0 || slot >= VX_USER_MAX_COUNT) return false;

    EnterCriticalSection(&lock_);
    bool connected = slots_[slot].connected;
    if (connected) {
        gamepad = slots_[slot].pad;
        packet  = slots_[slot].packet;
    }
    LeaveCriticalSection(&lock_);
    return connected;
}

void PadManager::SetRumble(int slot, WORD left, WORD right) {
    if (slot < 0 || slot >= VX_USER_MAX_COUNT) return;

    EnterCriticalSection(&lock_);
    SlotState& s = slots_[slot];
    if (s.rumbleLeft != left || s.rumbleRight != right) {
        s.rumbleLeft  = left;
        s.rumbleRight = right;
        s.rumbleDirty = true;
    }
    LeaveCriticalSection(&lock_);
}

bool PadManager::TakeRumble(int slot, WORD& left, WORD& right) {
    if (slot < 0 || slot >= VX_USER_MAX_COUNT) return false;

    EnterCriticalSection(&lock_);
    SlotState& s     = slots_[slot];
    bool       dirty = s.rumbleDirty;
    if (dirty) {
        left          = s.rumbleLeft;
        right         = s.rumbleRight;
        s.rumbleDirty = false;
    }
    LeaveCriticalSection(&lock_);
    return dirty;
}

bool PadManager::IsConnected(int slot) {
    if (slot < 0 || slot >= VX_USER_MAX_COUNT) return false;

    EnterCriticalSection(&lock_);
    bool connected = slots_[slot].connected;
    LeaveCriticalSection(&lock_);
    return connected;
}

void PadManager::ThreadMain() {
    std::wstring moduleDir = ModuleDirectory(module_);
    std::wstring exeDir    = ModuleDirectory(nullptr);

    config_ = ConfigLoad(moduleDir, exeDir);
    if (config_.log) LogOpen(moduleDir.empty() ? exeDir : moduleDir);

    if (config_.fileFound) {
        VXLOG("config: %s", Narrow(config_.path).c_str());
    } else {
        VXLOG("config: none found, using defaults");
    }
    if (!config_.error.empty()) {
        VXLOG("config error (using defaults): %s", config_.error.c_str());
    }

    DiSystem di;
    if (!di.Init()) {
        VXLOG("DirectInput unavailable: %s", di.LastError().c_str());
        SetEvent(readyEvent_);
        return;
    }

    std::vector<OpenPad> pads;
    std::vector<GUID>    openGuids;
    DWORD                lastScan      = 0;
    bool                 firstScan     = true;
    bool                 readySignaled = false;
    const DWORD          startTick     = GetTickCount();

    const DWORD periodMs = (DWORD)(1000 / (config_.pollHz > 0 ? config_.pollHz : 250));

    for (;;) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;

        DWORD now = GetTickCount();
        if (firstScan || (now - lastScan) >= kRescanIntervalMs) {
            lastScan = now;

            std::vector<DeviceInfo> found;
            di.Enumerate(found);

            // Rebuild only when the attached set actually changed, so a steady
            // state costs one enumeration every couple of seconds and nothing
            // more.
            bool changed = (found.size() != openGuids.size());
            if (!changed) {
                for (size_t i = 0; i < found.size(); ++i) {
                    if (!IsEqualGUID(found[i].instanceGuid, openGuids[i])) {
                        changed = true;
                        break;
                    }
                }
            }

            if (changed) {
                for (size_t i = 0; i < pads.size(); ++i) {
                    pads[i].device->StopRumble();
                    delete pads[i].device;
                }
                pads.clear();
                for (int s = 0; s < VX_USER_MAX_COUNT; ++s) ClearSlot(s);

                openGuids.clear();
                for (size_t i = 0; i < found.size(); ++i)
                    openGuids.push_back(found[i].instanceGuid);

                bool slotTaken[VX_USER_MAX_COUNT] = {false, false, false, false};

                for (size_t i = 0; i < found.size(); ++i) {
                    const DeviceProfile* match = SelectProfile(config_.devices, found[i]);
                    if (!match) {
                        VXLOG("no profile matches '%s', ignoring",
                              Narrow(found[i].productName).c_str());
                        continue;
                    }

                    int slot = match->slot;
                    if (slot >= 0) {
                        if (slotTaken[slot]) {
                            VXLOG("slot %d already in use, skipping '%s'", slot,
                                  Narrow(found[i].productName).c_str());
                            continue;
                        }
                    } else {
                        slot = -1;
                        for (int s = 0; s < VX_USER_MAX_COUNT; ++s) {
                            if (!slotTaken[s]) { slot = s; break; }
                        }
                        if (slot < 0) continue;   // more pads than XInput slots
                    }

                    DiDevice* dev = di.Open(found[i], match->rumble);
                    if (!dev) continue;

                    OpenPad pad;
                    pad.device  = dev;
                    pad.profile = *match;
                    pad.slot    = slot;
                    BuildAutoProfile(dev->Caps(), pad.profile);

                    slotTaken[slot] = true;
                    pads.push_back(pad);

                    VXLOG("slot %d <- '%s' [%s]", slot,
                          Narrow(found[i].productName).c_str(),
                          GuidToString(found[i].instanceGuid).c_str());
                    VXLOG("  %s", DescribeProfile(pad.profile).c_str());
                    VXLOG("  rumble: %s", dev->ForceFeedbackReady()
                              ? dev->ForceFeedbackKind()
                              : (!match->rumble ? "disabled by config"
                                                : (dev->ForceFeedbackCapable()
                                                       ? "capable but unavailable"
                                                       : "not supported by device")));
                }

                if (pads.empty()) VXLOG("no usable DirectInput controllers found");
            }

            firstScan = false;
        }

        for (size_t i = 0; i < pads.size(); ++i) {
            WORD rl = 0, rr = 0;
            if (TakeRumble(pads[i].slot, rl, rr)) {
                const float gain = pads[i].profile.rumbleGain;
                pads[i].device->SetRumble((rl / 65535.0f) * gain,
                                          (rr / 65535.0f) * gain);
            }

            RawState raw;
            if (!pads[i].device->Poll(raw)) {
                // Keep the slot connected across a transient acquisition loss;
                // dropping it would make games think the pad was unplugged.
                continue;
            }
            XINPUT_GAMEPAD gp;
            MapState(pads[i].profile, raw, gp);
            PublishSlot(pads[i].slot, gp);
        }

        // Only now is the first XInput call released. Enumerating a device is
        // not enough: its slot stays empty until a poll has actually published
        // a sample, and unblocking any earlier lets the game's first query race
        // that first sample and conclude no controller is present.
        if (!readySignaled) {
            bool allPublished = true;
            for (size_t i = 0; i < pads.size(); ++i) {
                if (!IsConnected(pads[i].slot)) { allPublished = false; break; }
            }
            if (allPublished || (GetTickCount() - startTick) > kReadySampleTimeoutMs) {
                readySignaled = true;
                SetEvent(readyEvent_);
            }
        }

        di.PumpMessages();
        WaitForSingleObject(stopEvent_, periodMs ? periodMs : 4);
    }

    for (size_t i = 0; i < pads.size(); ++i) {
        pads[i].device->StopRumble();
        delete pads[i].device;
    }
    pads.clear();
    di.Shutdown();
    VXLOG("polling thread exiting");
}

} // namespace vx
