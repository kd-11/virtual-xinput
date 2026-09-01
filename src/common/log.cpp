#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace vx {
namespace {

FILE*      g_file = nullptr;
std::mutex g_mutex;

} // namespace

void LogOpen(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;

    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"virtual-xinput.log";

    if (_wfopen_s(&g_file, path.c_str(), L"w") != 0) g_file = nullptr;
    if (!g_file) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "=== virtual-xinput log %04d-%02d-%02d %02d:%02d:%02d (pid %lu, %d-bit) ===\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            GetCurrentProcessId(), (int)(sizeof(void*) * 8));
    fflush(g_file);
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

bool LogEnabled() { return g_file != nullptr; }

void LogWrite(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);

    fputc('\n', g_file);
    fflush(g_file);   // a crashing game must not lose the tail of the log
}

} // namespace vx
