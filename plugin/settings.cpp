#include "settings.hpp"
#include "host_bridge.h"
#include "options.h"
#include "transport.hpp"
#include <algorithm>
#include <stdexcept>

namespace wsl
{
namespace
{
bool validPrefix(const std::wstring &value)
{
    if (value.empty())
        return true;
    size_t begin = 0;
    bool unc = value.rfind(L"\\\\", 0) == 0;
    if (unc)
        begin = 2;
    else
    {
        bool drive = value.size() >= 3 &&
                     ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) &&
                     value[1] == L':' && value[2] == L'\\';
        if (!drive)
            return false;
        begin = 3;
    }
    size_t components = 0;
    while (begin < value.size())
    {
        size_t end = value.find(L'\\', begin);
        std::wstring component = value.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (component.empty() || component == L"." || component == L".." || component.back() == L'.' ||
            component.back() == L' ' || component.find_first_of(L"/\":*?<>|") != std::wstring::npos)
            return false;
        for (wchar_t character : component)
            if (character < L' ')
                return false;
        ++components;
        if (end == std::wstring::npos)
            break;
        begin = end + 1;
    }
    // A UNC prefix needs both a server and a share. In particular, reject the
    // device namespaces \\.\ and \\?\, which are not Explorer locations.
    return !unc || components >= 2;
}
} // namespace
DWORD readSetting(const wchar_t *name, DWORD fallback)
{
    DWORD value = fallback, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, RegistryKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value;
}
void writeSetting(const wchar_t *name, DWORD value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Unable to open the settings registry key.");
    LSTATUS result =
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        throw std::runtime_error("Unable to save the WSL setting.");
}
std::wstring distroPrefix(const std::wstring &distro)
{
    auto key = std::wstring(RegistryKey) + L"\\PathOverrides";
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), distro.c_str(), RRF_RT_REG_SZ, nullptr, nullptr,
                     &size) != ERROR_SUCCESS)
        return L"";
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), distro.c_str(), RRF_RT_REG_SZ, nullptr, value.data(),
                     &size) != ERROR_SUCCESS)
        return L"";
    while (!value.empty() && value.back() == L'\0')
        value.pop_back();
    return value;
}
void setDistroPrefix(const std::wstring &distro, const std::wstring &prefix)
{
    if (!validPrefix(prefix))
        throw std::runtime_error("Use an absolute drive path or a UNC path containing a server and share.");
    HKEY key;
    auto path = std::wstring(RegistryKey) + L"\\PathOverrides";
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                     &key, nullptr);
    if (result != ERROR_SUCCESS)
        throw std::runtime_error("Unable to open the path override registry key.");
    if (prefix.empty())
        result = RegDeleteValueW(key, distro.c_str());
    else
        result =
            RegSetValueExW(key, distro.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE *>(prefix.c_str()),
                           static_cast<DWORD>((prefix.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
        throw std::runtime_error("Unable to save the path override.");
}
std::wstring windowsPath(const std::wstring &distro, const std::wstring &linuxPath)
{
    if (linuxPath.empty() || linuxPath.front() != L'/')
        throw std::runtime_error("This descriptor is not a filesystem path.");
    auto path = linuxPath;
    const std::wstring deleted = L" (deleted)";
    if (path.size() >= deleted.size() &&
        path.compare(path.size() - deleted.size(), deleted.size(), deleted) == 0)
        throw std::runtime_error(
            "This file has been deleted. Its descriptor may remain open, but Explorer cannot locate it.");
    std::wstring prefix;
    size_t begin = 1;
    // Standard WSL drive mounts refer to Windows volumes directly, even when
    // ordinary Linux paths use a custom per-distro Explorer prefix.
    const bool mountedDrive = path.size() >= 6 && path.compare(0, 5, L"/mnt/") == 0 &&
        ((path[5] >= L'a' && path[5] <= L'z') || (path[5] >= L'A' && path[5] <= L'Z')) &&
        (path.size() == 6 || path[6] == L'/');
    if (mountedDrive)
    {
        wchar_t drive = path[5] >= L'a' ? path[5] - (L'a' - L'A') : path[5];
        prefix = std::wstring(1, drive) + L":\\";
        begin = path.size() == 6 ? 6 : 7;
    }
    else
    {
        prefix = distroPrefix(distro);
        if (!validPrefix(prefix))
            throw std::runtime_error("The registry path override is invalid. Correct it in WSL Tools settings.");
        if (prefix.empty()) prefix = L"\\\\wsl.localhost\\" + distro + L"\\";
        if (prefix.back() != L'\\') prefix += L'\\';
    }
    std::wstring tail;
    while (begin <= path.size())
    {
        auto end = path.find(L'/', begin);
        auto component = path.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (component == L".." || component.find_first_of(L"\\\":*?<>|") != std::wstring::npos)
            throw std::runtime_error("This Linux path cannot be represented safely as a Windows path.");
        if (!component.empty() && component != L".")
        {
            if (!tail.empty())
                tail += L'\\';
            tail += component;
        }
        if (end == std::wstring::npos)
            break;
        begin = end + 1;
    }
    return prefix + tail;
}

namespace
{
std::wstring preferredOptionsDistro;

struct OptionsState
{
    std::vector<std::wstring> distros;
    int selected = -1;
    bool loading = false;
};

std::wstring optionText(HWND window, int controlId)
{
    HWND control = GetDlgItem(window, controlId);
    std::wstring result(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    GetWindowTextW(control, result.data(), static_cast<int>(result.size()));
    result.resize(wcslen(result.c_str()));
    return result;
}

void optionsStatus(HWND window, const std::wstring &message)
{
    SetDlgItemTextW(window, IDC_WSL_OPTIONS_STATUS, message.c_str());
}

void loadPrefix(HWND window, OptionsState &state)
{
    state.loading = true;
    const bool available = state.selected >= 0 && static_cast<size_t>(state.selected) < state.distros.size();
    auto prefix = available ? distroPrefix(state.distros[state.selected]) : std::wstring{};
    SetDlgItemTextW(window, IDC_WSL_PREFIX, prefix.c_str());
    EnableWindow(GetDlgItem(window, IDC_WSL_PREFIX), available);
    EnableWindow(GetDlgItem(window, IDC_WSL_APPLY_PREFIX), FALSE);
    state.loading = false;
}

bool applyPrefix(HWND window, OptionsState &state)
{
    if (state.selected < 0 || static_cast<size_t>(state.selected) >= state.distros.size())
        return true;
    try
    {
        setDistroPrefix(state.distros[state.selected], optionText(window, IDC_WSL_PREFIX));
        EnableWindow(GetDlgItem(window, IDC_WSL_APPLY_PREFIX), FALSE);
        optionsStatus(window, L"Explorer path prefix saved.");
        return true;
    }
    catch (const std::exception &error)
    {
        optionsStatus(window, wide(error.what()));
        SetFocus(GetDlgItem(window, IDC_WSL_PREFIX));
        return false;
    }
}

void layoutOptions(HWND window)
{
    RECT bounds{};
    GetClientRect(window, &bounds);
    auto position = [&](int id, int x, int y, int rightMargin, int height) {
        RECT units{x, y, rightMargin, height};
        MapDialogRect(window, &units);
        place(GetDlgItem(window, id), units.left, units.top, bounds.right - units.left - units.right,
              units.bottom);
    };
    position(IDC_WSL_GENERAL_GROUP, 7, 7, 7, 110);
    position(IDC_WSL_CPU_LABEL, 14, 20, 14, 10);
    position(IDC_WSL_CPU_MODE, 14, 33, 14, 70);
    position(IDC_WSL_NODE_INSPECTOR, 14, 53, 14, 20);
    position(IDC_WSL_DETECT_32BIT, 14, 76, 14, 14);
    position(IDC_WSL_REFRESH_NOTE, 14, 95, 14, 16);
    position(IDC_WSL_PATH_GROUP, 7, 124, 7, 99);
    position(IDC_WSL_DISTRO_LABEL, 14, 137, 14, 10);
    position(IDC_WSL_DISTRO, 14, 150, 14, 70);
    position(IDC_WSL_PREFIX_LABEL, 14, 170, 14, 10);
    position(IDC_WSL_PREFIX, 14, 183, 89, 14);
    RECT button{0, 182, 70, 16};
    MapDialogRect(window, &button);
    RECT margin{14, 0, 0, 0};
    MapDialogRect(window, &margin);
    place(GetDlgItem(window, IDC_WSL_APPLY_PREFIX), bounds.right - margin.left - button.right, button.top,
          button.right, button.bottom);
    position(IDC_WSL_PREFIX_HINT, 14, 203, 14, 14);
    position(IDC_WSL_OPTIONS_STATUS, 7, 229, 7, 18);
}

INT_PTR CALLBACK optionsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    UNREFERENCED_PARAMETER(lparam);
    auto state = reinterpret_cast<OptionsState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_INITDIALOG)
    {
        state = new OptionsState;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->loading = true;
        for (auto label : {L"100% = one vCPU (Linux convention)", L"100% = all WSL vCPUs"})
            SendDlgItemMessageW(window, IDC_WSL_CPU_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendDlgItemMessageW(window, IDC_WSL_CPU_MODE, CB_SETCURSEL,
                            readSetting(L"CpuPercentOfTotal", 1) ? 1 : 0, 0);
        CheckDlgButton(window, IDC_WSL_NODE_INSPECTOR,
                       readSetting(L"UseNodeInspectorWithoutAsking", 0) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, IDC_WSL_DETECT_32BIT,
                       readSetting(L"Detect32BitProcesses", 0) ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessageW(window, IDC_WSL_PREFIX, EM_SETLIMITTEXT, 32760, 0);
        try
        {
            // Registration discovery only reads HKCU. Opening Options must not
            // start a stopped distro or launch a Linux process.
            state->distros = registeredWsl2Distros();
            std::sort(state->distros.begin(), state->distros.end());
            for (size_t i = 0; i < state->distros.size(); ++i)
            {
                SendDlgItemMessageW(window, IDC_WSL_DISTRO, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(state->distros[i].c_str()));
                if (state->distros[i] == preferredOptionsDistro)
                    state->selected = static_cast<int>(i);
            }
            if (!state->distros.empty() && state->selected < 0)
                state->selected = 0;
            SendDlgItemMessageW(window, IDC_WSL_DISTRO, CB_SETCURSEL, state->selected, 0);
            if (state->distros.empty())
                optionsStatus(window, L"No registered WSL 2 distributions were found.");
        }
        catch (const std::exception &error)
        {
            optionsStatus(window, wide(error.what()));
        }
        EnableWindow(GetDlgItem(window, IDC_WSL_DISTRO), !state->distros.empty());
        loadPrefix(window, *state);
        layoutOptions(window);
        return TRUE;
    }
    if (!state)
        return FALSE;
    switch (message)
    {
    case WM_SIZE:
        layoutOptions(window);
        return TRUE;
    case WM_COMMAND:
        if (state->loading)
            return FALSE;
        try
        {
            int id = LOWORD(wparam), event = HIWORD(wparam);
            if (id == IDC_WSL_CPU_MODE && event == CBN_SELCHANGE)
            {
                writeSetting(L"CpuPercentOfTotal",
                             SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0);
                optionsStatus(window, L"CPU percentage display saved.");
                return TRUE;
            }
            if (id == IDC_WSL_NODE_INSPECTOR && event == BN_CLICKED)
            {
                writeSetting(L"UseNodeInspectorWithoutAsking",
                             IsDlgButtonChecked(window, id) == BST_CHECKED ? 1 : 0);
                optionsStatus(window, L"Node Inspector preference saved.");
                return TRUE;
            }
            if (id == IDC_WSL_DETECT_32BIT && event == BN_CLICKED)
            {
                writeSetting(L"Detect32BitProcesses", IsDlgButtonChecked(window, id) == BST_CHECKED ? 1 : 0);
                optionsStatus(window, L"32-bit detection preference saved.");
                return TRUE;
            }
            if (id == IDC_WSL_PREFIX && event == EN_CHANGE)
            {
                EnableWindow(GetDlgItem(window, IDC_WSL_APPLY_PREFIX), state->selected >= 0);
                optionsStatus(window, L"Choose Apply prefix to save this path.");
                return TRUE;
            }
            if (id == IDC_WSL_APPLY_PREFIX && event == BN_CLICKED)
            {
                applyPrefix(window, *state);
                return TRUE;
            }
            if (id == IDC_WSL_DISTRO && event == CBN_SELCHANGE)
            {
                int next = static_cast<int>(SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0));
                if (IsWindowEnabled(GetDlgItem(window, IDC_WSL_APPLY_PREFIX)) && !applyPrefix(window, *state))
                {
                    SendDlgItemMessageW(window, id, CB_SETCURSEL, state->selected, 0);
                    return TRUE;
                }
                state->selected = next;
                loadPrefix(window, *state);
                optionsStatus(window, L"");
                return TRUE;
            }
        }
        catch (const std::exception &error)
        {
            optionsStatus(window, wide(error.what()));
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, WslDialogText());
        SetBkColor(dc, WslDialogBackground());
        SetDCBrushColor(dc, WslDialogBackground());
        return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
    }
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return FALSE;
}
} // namespace

void showSettings(HWND owner, const std::wstring &distro)
{
    preferredOptionsDistro = distro;
    WslOpenHostOptions(owner);
}
} // namespace wsl

extern "C" INT_PTR CALLBACK WslOptionsDialogProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return wsl::optionsProc(window, message, wparam, lparam);
}
