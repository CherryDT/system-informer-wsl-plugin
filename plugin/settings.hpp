#pragma once
#include "common.hpp"
namespace wsl {
constexpr const wchar_t* RegistryKey = L"Software\\David Trapp\\System Informer WSL Plugin";
DWORD readSetting(const wchar_t* name, DWORD fallback);
void writeSetting(const wchar_t* name, DWORD value);
std::wstring distroPrefix(const std::wstring& distro);
void setDistroPrefix(const std::wstring& distro, const std::wstring& prefix);
std::wstring windowsPath(const std::wstring& distro, const std::wstring& linuxPath);
void showSettings(HWND owner, const std::wstring& distro);
}
