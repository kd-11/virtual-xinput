#include <windows.h>

// Recorded so the config file can be looked up next to the DLL rather than next
// to the executable - which matters when the DLL is dropped into a subfolder.
extern "C" HMODULE g_hModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;

    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_hModule = hModule;
            // No DirectInput, no threads, no config I/O here: DllMain runs
            // under the loader lock and any of those can deadlock. Everything
            // is started lazily on the first XInput call instead.
            DisableThreadLibraryCalls(hModule);
            break;

        case DLL_PROCESS_DETACH:
            // Intentionally does nothing. Stopping the polling thread means
            // waiting for it to exit, and a thread cannot finish exiting while
            // we hold the loader lock here. The OS reclaims the thread and its
            // handles at process exit.
            break;
    }
    return TRUE;
}
