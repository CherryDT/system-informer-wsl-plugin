#pragma once
#include "common.hpp"
#include "host_bridge.h"
namespace wsl
{
DWORD readSetting(const wchar_t *name, DWORD fallback);
void writeSetting(const wchar_t *name, DWORD value);
std::wstring readStringSetting(WSL_STRING_SETTING setting);
void writeStringSetting(WSL_STRING_SETTING setting, const std::wstring &value);
std::wstring distroPrefix(const std::wstring &distro);
void setDistroPrefix(const std::wstring &distro, const std::wstring &prefix);
std::wstring windowsPath(const std::wstring &distro, const std::wstring &linuxPath);
void showSettings(HWND owner, const std::wstring &distro);
} // namespace wsl
