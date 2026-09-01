// Smoke test for the built DLL: loads it exactly as a game would, checks the
// export ordinals against the genuine Microsoft DLL, and reports what the
// virtual pads look like.
//
// The ordinal check exists because games built with the DirectX SDK import
// XInput BY ORDINAL. If our numbering disagrees with Microsoft's, such a game
// calls the wrong function with a mismatched __stdcall signature and crashes.
// That is not theoretical: it is the bug this test was written to catch.
//
// Deliberately does not link xinput; it uses its own copies of the structures,
// so nothing here can accidentally pull in the real XInput.

#include "../src/common/xinput_defs.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef DWORD(WINAPI* PFN_GetState)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* PFN_SetState)(DWORD, XINPUT_VIBRATION*);
typedef DWORD(WINAPI* PFN_GetCaps)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef void(WINAPI* PFN_Enable)(BOOL);
typedef DWORD(WINAPI* PFN_GetBattery)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);

namespace {

int g_failures = 0;

void Fail(const char* fmt, ...) {
    ++g_failures;
    va_list args;
    va_start(args, fmt);
    printf("  FAIL: ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

// The export table of XInput1_3.dll, as shipped by Microsoft. Ordinal 1 really
// is DllMain; everything else is numbered from 2 because of it.
struct Expected {
    const char* name;
    WORD        ordinal;
    bool        named;   // ordinals 100+ are NONAME in the real DLL
};

const Expected kExpected[] = {
    {"DllMain",                         1,   true},
    {"XInputGetState",                  2,   true},
    {"XInputSetState",                  3,   true},
    {"XInputGetCapabilities",           4,   true},
    {"XInputEnable",                    5,   true},
    {"XInputGetDSoundAudioDeviceGuids", 6,   true},
    {"XInputGetBatteryInformation",     7,   true},
    {"XInputGetKeystroke",              8,   true},
    {"XInputGetStateEx",                100, false},
    {"XInputWaitForGuideButton",        101, false},
    {"XInputCancelGuideButtonWait",     102, false},
    {"XInputPowerOffController",        103, false},
};

// ---------------------------------------------------------------------------
// Minimal PE export-table reader.
//
// The file is parsed rather than loaded: LoadLibrary on a second DLL with the
// same base name would not reliably give a distinct module, and we need to
// inspect Microsoft's copy while ours is loaded.
// ---------------------------------------------------------------------------

struct ExportEntry {
    std::string name;
    WORD        ordinal;
};

bool ReadWholeFile(const std::wstring& path, std::vector<char>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64 << 20)) {
        CloseHandle(h);
        return false;
    }

    data.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL  ok   = ReadFile(h, &data[0], (DWORD)data.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    data.resize(read);
    return true;
}

template <typename T>
const T* At(const std::vector<char>& d, size_t offset) {
    if (offset + sizeof(T) > d.size()) return nullptr;
    return reinterpret_cast<const T*>(&d[offset]);
}

bool ReadExports(const std::wstring& path, std::vector<ExportEntry>& out) {
    out.clear();

    std::vector<char> d;
    if (!ReadWholeFile(path, d)) return false;

    const IMAGE_DOS_HEADER* dos = At<IMAGE_DOS_HEADER>(d, 0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;

    size_t ntOff = (size_t)dos->e_lfanew;
    const DWORD* sig = At<DWORD>(d, ntOff);
    if (!sig || *sig != IMAGE_NT_SIGNATURE) return false;

    size_t fileHdrOff = ntOff + sizeof(DWORD);
    const IMAGE_FILE_HEADER* fh = At<IMAGE_FILE_HEADER>(d, fileHdrOff);
    if (!fh) return false;

    size_t optOff = fileHdrOff + sizeof(IMAGE_FILE_HEADER);
    const WORD* magic = At<WORD>(d, optOff);
    if (!magic) return false;

    DWORD exportRva = 0;
    if (*magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        const IMAGE_OPTIONAL_HEADER32* oh = At<IMAGE_OPTIONAL_HEADER32>(d, optOff);
        if (!oh || oh->NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
        exportRva = oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    } else if (*magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const IMAGE_OPTIONAL_HEADER64* oh = At<IMAGE_OPTIONAL_HEADER64>(d, optOff);
        if (!oh || oh->NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
        exportRva = oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    } else {
        return false;
    }
    if (exportRva == 0) return false;

    // RVA -> file offset, using the section table.
    size_t secOff = optOff + fh->SizeOfOptionalHeader;
    const IMAGE_SECTION_HEADER* sections = At<IMAGE_SECTION_HEADER>(d, secOff);
    if (!sections) return false;

    struct Mapper {
        const IMAGE_SECTION_HEADER* s;
        int                         count;

        size_t operator()(DWORD rva) const {
            for (int i = 0; i < count; ++i) {
                DWORD va   = s[i].VirtualAddress;
                DWORD size = s[i].Misc.VirtualSize ? s[i].Misc.VirtualSize : s[i].SizeOfRawData;
                if (rva >= va && rva < va + size) {
                    return (size_t)s[i].PointerToRawData + (rva - va);
                }
            }
            return (size_t)-1;
        }
    };
    Mapper toOffset{sections, (int)fh->NumberOfSections};

    size_t expOff = toOffset(exportRva);
    if (expOff == (size_t)-1) return false;

    const IMAGE_EXPORT_DIRECTORY* ed = At<IMAGE_EXPORT_DIRECTORY>(d, expOff);
    if (!ed) return false;

    size_t namesOff = toOffset(ed->AddressOfNames);
    size_t ordsOff  = toOffset(ed->AddressOfNameOrdinals);
    if (namesOff == (size_t)-1 || ordsOff == (size_t)-1) return false;

    for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
        const DWORD* nameRva = At<DWORD>(d, namesOff + i * sizeof(DWORD));
        const WORD*  ordIdx  = At<WORD>(d, ordsOff + i * sizeof(WORD));
        if (!nameRva || !ordIdx) return false;

        size_t strOff = toOffset(*nameRva);
        if (strOff == (size_t)-1 || strOff >= d.size()) return false;

        ExportEntry e;
        e.name    = std::string(&d[strOff]);
        e.ordinal = (WORD)(ed->Base + *ordIdx);
        out.push_back(e);
    }
    return true;
}

int OrdinalOf(const std::vector<ExportEntry>& exports, const char* name) {
    for (size_t i = 0; i < exports.size(); ++i) {
        if (_stricmp(exports[i].name.c_str(), name) == 0) return exports[i].ordinal;
    }
    return -1;
}

// Confirms our expectations really do match Microsoft's DLL, so this test can
// never drift into asserting our own mistake.
void CheckAgainstGenuine() {
    wchar_t sysDir[MAX_PATH];
    UINT    n = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    // A 32-bit process is redirected to SysWOW64, so this is the matching
    // bitness in both builds.
    std::wstring path = std::wstring(sysDir) + L"\\XInput1_3.dll";

    std::vector<ExportEntry> genuine;
    if (!ReadExports(path, genuine)) {
        printf("\nGenuine XInput1_3.dll not available - skipping the cross-check.\n");
        printf("(Looked in %S)\n", path.c_str());
        return;
    }

    printf("\nCross-check against %S:\n", path.c_str());
    for (int i = 0; i < (int)(sizeof(kExpected) / sizeof(kExpected[0])); ++i) {
        if (!kExpected[i].named) continue;   // NONAME entries carry no name to match

        int got = OrdinalOf(genuine, kExpected[i].name);
        if (got < 0) {
            printf("  %-34s not exported by name in the genuine DLL (skipped)\n",
                   kExpected[i].name);
            continue;
        }
        if (got != (int)kExpected[i].ordinal) {
            Fail("%s is ordinal %d in the genuine DLL, but we expect %d",
                 kExpected[i].name, got, (int)kExpected[i].ordinal);
        } else {
            printf("  %-34s @%-3d matches\n", kExpected[i].name, got);
        }
    }
}

void CheckOurExports(const std::wstring& dllPath) {
    std::vector<ExportEntry> ours;
    if (!ReadExports(dllPath, ours)) {
        Fail("could not read our own export table from %S", dllPath.c_str());
        return;
    }

    printf("\nOur export table:\n");
    for (int i = 0; i < (int)(sizeof(kExpected) / sizeof(kExpected[0])); ++i) {
        if (!kExpected[i].named) continue;

        int got = OrdinalOf(ours, kExpected[i].name);
        if (got < 0) {
            Fail("%s is not exported by name", kExpected[i].name);
        } else if (got != (int)kExpected[i].ordinal) {
            Fail("%s is at ordinal %d, should be %d", kExpected[i].name, got,
                 (int)kExpected[i].ordinal);
        } else {
            printf("  %-34s @%-3d ok\n", kExpected[i].name, got);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* dllPathA = (argc > 1) ? argv[1] : "xinput1_3.dll";

    printf("Loading %s (%d-bit host)\n", dllPathA, (int)(sizeof(void*) * 8));

    wchar_t wide[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dllPathA, -1, wide, MAX_PATH);
    std::wstring dllPath(wide);

    CheckAgainstGenuine();
    CheckOurExports(dllPath);

    HMODULE dll = LoadLibraryW(dllPath.c_str());
    if (!dll) {
        printf("\nFAIL: LoadLibrary failed, error %lu\n", GetLastError());
        return 1;
    }

    // Every ordinal must resolve, and where the export is named, the name and
    // the ordinal must reach the same function.
    printf("\nRuntime resolution:\n");
    for (int i = 0; i < (int)(sizeof(kExpected) / sizeof(kExpected[0])); ++i) {
        FARPROC byOrdinal = GetProcAddress(dll, (LPCSTR)(uintptr_t)kExpected[i].ordinal);
        if (!byOrdinal) {
            Fail("ordinal %d (%s) does not resolve", (int)kExpected[i].ordinal,
                 kExpected[i].name);
            continue;
        }
        if (kExpected[i].named) {
            FARPROC byName = GetProcAddress(dll, kExpected[i].name);
            if (!byName) {
                Fail("%s does not resolve by name", kExpected[i].name);
                continue;
            }
            if (byName != byOrdinal) {
                Fail("%s: name and ordinal %d point at different functions",
                     kExpected[i].name, (int)kExpected[i].ordinal);
                continue;
            }
        }
        printf("  %-34s @%-3d ok\n", kExpected[i].name, (int)kExpected[i].ordinal);
    }

    // Resolve through ORDINALS, the way a DirectX SDK-linked game does. If the
    // numbering were wrong, these would be the wrong functions.
    PFN_GetState   getState   = (PFN_GetState)GetProcAddress(dll, (LPCSTR)2);
    PFN_SetState   setState   = (PFN_SetState)GetProcAddress(dll, (LPCSTR)3);
    PFN_GetCaps    getCaps    = (PFN_GetCaps)GetProcAddress(dll, (LPCSTR)4);
    PFN_Enable     enable     = (PFN_Enable)GetProcAddress(dll, (LPCSTR)5);
    PFN_GetBattery getBattery = (PFN_GetBattery)GetProcAddress(dll, (LPCSTR)7);
    PFN_GetState   getStateEx = (PFN_GetState)GetProcAddress(dll, (LPCSTR)100);

    if (!getState || !getStateEx || !getCaps || !setState || !enable || !getBattery) {
        printf("\nFAIL: could not resolve the functions needed to continue\n");
        return 1;
    }

    printf("\nSlot scan (called through ordinals):\n");
    int connected = 0;
    for (DWORD slot = 0; slot < VX_USER_MAX_COUNT; ++slot) {
        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(state));
        DWORD r = getState(slot, &state);

        if (r == ERROR_SUCCESS) {
            ++connected;
            printf("  slot %lu: CONNECTED  packet=%lu buttons=0x%04X LX=%+6d LY=%+6d "
                   "RX=%+6d RY=%+6d LT=%3d RT=%3d\n",
                   slot, state.dwPacketNumber, state.Gamepad.wButtons,
                   state.Gamepad.sThumbLX, state.Gamepad.sThumbLY,
                   state.Gamepad.sThumbRX, state.Gamepad.sThumbRY,
                   state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger);

            XINPUT_CAPABILITIES caps;
            ZeroMemory(&caps, sizeof(caps));
            DWORD cr = getCaps(slot, VX_FLAG_GAMEPAD, &caps);
            printf("           caps: rc=%lu type=%u subtype=%u\n", cr, caps.Type, caps.SubType);

            XINPUT_BATTERY_INFORMATION bat;
            ZeroMemory(&bat, sizeof(bat));
            DWORD br = getBattery(slot, 0, &bat);
            printf("           battery: rc=%lu type=%u level=%u\n", br,
                   bat.BatteryType, bat.BatteryLevel);

            XINPUT_VIBRATION vib;
            vib.wLeftMotorSpeed = vib.wRightMotorSpeed = 0;
            printf("           XInputSetState rc=%lu\n", setState(slot, &vib));
        } else if (r == ERROR_DEVICE_NOT_CONNECTED) {
            printf("  slot %lu: empty\n", slot);
        } else {
            Fail("slot %lu returned unexpected %lu", slot, r);
        }
    }

    // Out-of-range indices must be rejected, not treated as slot 0.
    XINPUT_STATE dummy;
    if (getState(99, &dummy) != ERROR_BAD_ARGUMENTS) {
        Fail("index 99 should return ERROR_BAD_ARGUMENTS");
    }
    if (getState(0, nullptr) != ERROR_BAD_ARGUMENTS) {
        Fail("null pState should return ERROR_BAD_ARGUMENTS");
    }

    if (connected > 0) {
        // XInputEnable(FALSE) must neutralise input but keep reporting success.
        enable(FALSE);
        XINPUT_STATE off;
        ZeroMemory(&off, sizeof(off));
        DWORD r = getState(0, &off);
        bool neutral = (r == ERROR_SUCCESS) && off.Gamepad.wButtons == 0 &&
                       off.Gamepad.sThumbLX == 0 && off.Gamepad.sThumbLY == 0 &&
                       off.Gamepad.bLeftTrigger == 0 && off.Gamepad.bRightTrigger == 0;
        printf("\nXInputEnable(FALSE) reports neutral state: %s\n", neutral ? "yes" : "NO");
        if (!neutral) Fail("XInputEnable(FALSE) did not neutralise the state");
        enable(TRUE);

        // The Guide bit must never appear through the plain XInputGetState.
        XINPUT_STATE plain, ex;
        ZeroMemory(&plain, sizeof(plain));
        ZeroMemory(&ex, sizeof(ex));
        getState(0, &plain);
        getStateEx(0, &ex);
        bool guideMasked = (plain.Gamepad.wButtons & VX_GAMEPAD_GUIDE) == 0;
        printf("XInputGetState masks the Guide bit: %s\n", guideMasked ? "yes" : "NO");
        if (!guideMasked) Fail("Guide bit leaked through XInputGetState");

        printf("\nLive samples from slot 0 (move the pad to see values change):\n");
        for (int i = 0; i < 5; ++i) {
            XINPUT_STATE s;
            ZeroMemory(&s, sizeof(s));
            getState(0, &s);
            printf("  packet=%-6lu buttons=0x%04X LX=%+6d LY=%+6d RX=%+6d RY=%+6d "
                   "LT=%3d RT=%3d\n",
                   s.dwPacketNumber, s.Gamepad.wButtons,
                   s.Gamepad.sThumbLX, s.Gamepad.sThumbLY,
                   s.Gamepad.sThumbRX, s.Gamepad.sThumbRY,
                   s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger);
            Sleep(300);
        }
    } else {
        printf("\nNo slot reported a controller.\n");
    }

    printf("\n----------------------------------------\n");
    printf("%s (%d connected, %d problems)\n",
           g_failures == 0 ? "PASSED" : "FAILED", connected, g_failures);
    return g_failures == 0 ? 0 : 1;
}
