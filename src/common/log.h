#pragma once
#include <string>

namespace vx {

// Opens <dir>/virtual-xinput.log. Safe to call more than once; later calls are
// ignored. Logging stays off until this is called, so a drop-in DLL with no
// config writes nothing to the game folder.
void LogOpen(const std::wstring& dir);
void LogClose();
bool LogEnabled();

void LogWrite(const char* fmt, ...);

#define VXLOG(...) do { if (::vx::LogEnabled()) ::vx::LogWrite(__VA_ARGS__); } while (0)

} // namespace vx
