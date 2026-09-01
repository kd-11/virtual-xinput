#pragma once

#include <string>

// UTF-8 <-> UTF-16 conversion.
//
// These lived in di_device.h, which meant anything wanting to convert a string
// had to pull in DIRECTINPUT_VERSION and <dinput.h> to get them. They have
// nothing to do with DirectInput.
namespace vx {

std::string  Narrow(const std::wstring& s);
std::wstring Widen(const std::string& s);

} // namespace vx
