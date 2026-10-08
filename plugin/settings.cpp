#include "settings.hpp"
#include <algorithm>
#include <cerrno>
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
        throw std::runtime_error("Unable to save the refresh interval.");
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
    std::wstring prefix = distroPrefix(distro);
    if (!validPrefix(prefix))
        throw std::runtime_error("The registry path override is invalid. Correct it in WSL Tools settings.");
    if (prefix.empty())
        prefix = L"\\\\wsl.localhost\\" + distro + L"\\";
    if (prefix.back() != L'\\')
        prefix += L'\\';
    std::wstring tail;
    size_t begin = 1;
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
enum SettingsControl
{
    PrefixEdit = 10,
    IntervalEdit
};
struct SettingsWindow
{
    HWND heading{}, edit{}, hint{}, label{}, interval{}, status{}, save{}, cancel{};
    std::wstring distro;
};

void layoutSettings(HWND window, const SettingsWindow &state)
{
    RECT bounds{};
    GetClientRect(window, &bounds);
    int margin = scale(window, 18);
    int width = bounds.right - 2 * margin;
    place(state.heading, margin, margin, width, scale(window, 24));
    place(state.edit, margin, scale(window, 48), width, scale(window, 28));
    place(state.hint, margin, scale(window, 86), width, scale(window, 50));
    int intervalWidth = scale(window, 138);
    place(state.label, margin, scale(window, 148), width - intervalWidth - margin, scale(window, 24));
    place(state.interval, bounds.right - margin - intervalWidth, scale(window, 142), intervalWidth,
          scale(window, 28));
    place(state.status, margin, scale(window, 188), width, scale(window, 56));
    int buttonWidth = scale(window, 94), buttonHeight = scale(window, 30);
    int bottom = bounds.bottom - margin - buttonHeight;
    place(state.save, bounds.right - margin - 2 * buttonWidth - scale(window, 12), bottom, buttonWidth,
          buttonHeight);
    place(state.cancel, bounds.right - margin - buttonWidth, bottom, buttonWidth, buttonHeight);
}

LRESULT CALLBACK settingsKeys(HWND child, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                              DWORD_PTR reference)
{
    HWND parent = reinterpret_cast<HWND>(reference);
    if (message == WM_KEYDOWN)
    {
        if (wparam == VK_TAB)
        {
            HWND next = GetNextDlgTabItem(parent, child, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            if (next)
                SetFocus(next);
            return 0;
        }
        if (wparam == VK_RETURN || wparam == VK_ESCAPE)
        {
            int command = wparam == VK_ESCAPE || GetDlgCtrlID(child) == IDCANCEL ? IDCANCEL : IDOK;
            SendMessageW(parent, WM_COMMAND, command, 0);
            return 0;
        }
        if (wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) &&
            (GetDlgCtrlID(child) == PrefixEdit || GetDlgCtrlID(child) == IntervalEdit))
        {
            SendMessageW(child, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(child, settingsKeys, 1);
    return DefSubclassProc(child, message, wparam, lparam);
}

LRESULT CALLBACK settingsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto state = reinterpret_cast<SettingsWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        auto incoming = static_cast<std::unique_ptr<SettingsWindow> *>(
            reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        state = incoming->release();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE: {
        state->heading = control(window, L"STATIC", (L"Explorer path prefix for " + state->distro).c_str(),
                                 SS_NOPREFIX, 0);
        state->edit = control(window, L"EDIT", distroPrefix(state->distro).c_str(),
                              WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, PrefixEdit);
        SendMessageW(state->edit, EM_SETLIMITTEXT, 32760, 0);
        SendMessageW(state->edit, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"Default: \\\\wsl.localhost\\<distribution>\\"));
        state->hint = control(window, L"STATIC",
                              L"Example: U:\\ maps /home/user to U:\\home\\user.\r\nLeave blank to use "
                              L"\\\\wsl.localhost\\<distribution>\\.",
                              SS_NOPREFIX, 0);
        state->label =
            control(window, L"STATIC", L"Refresh interval (milliseconds, 500–60000)", SS_NOPREFIX, 0);
        auto refresh = std::clamp(readSetting(L"RefreshInterval", 2000), 500ul, 60000ul);
        state->interval = control(window, L"EDIT", std::to_wstring(refresh).c_str(),
                                  WS_BORDER | WS_TABSTOP | ES_NUMBER, IntervalEdit);
        SendMessageW(state->interval, EM_SETLIMITTEXT, 5, 0);
        state->status =
            control(window, L"STATIC",
                    L"Collectors always run as Linux root. Only the selected running distro is "
                    L"monitored.\r\nSwitching away from WSL or pausing disconnects the collector.\r\nA saved "
                    L"refresh interval applies automatically to ongoing monitoring.",
                    SS_NOPREFIX, 0);
        state->save = control(window, L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
        state->cancel = control(window, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
        for (HWND child : {state->edit, state->interval, state->save, state->cancel})
            SetWindowSubclass(child, settingsKeys, 1, reinterpret_cast<DWORD_PTR>(window));
        layoutSettings(window, *state);
        WslApplyTheme(window);
        return 0;
    }
    case WM_SIZE:
        layoutSettings(window, *state);
        return 0;
    case WM_DPICHANGED: {
        const RECT *bounds = reinterpret_cast<RECT *>(lparam);
        SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                     bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layoutSettings(window, *state);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL)
        {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wparam) == IDOK)
        {
            wchar_t prefix[32768]{}, interval[32]{};
            GetWindowTextW(state->edit, prefix, static_cast<int>(std::size(prefix)));
            GetWindowTextW(state->interval, interval, static_cast<int>(std::size(interval)));
            std::wstring value = prefix;
            if (!validPrefix(value))
            {
                errorBox(window, L"Enter an absolute Windows drive path (for example U:\\), a UNC path with "
                                 L"a server and share (\\\\server\\share\\), or leave the prefix blank.");
                SetFocus(state->edit);
                SendMessageW(state->edit, EM_SETSEL, 0, -1);
                return 0;
            }
            wchar_t *end = nullptr;
            errno = 0;
            unsigned long refresh = wcstoul(interval, &end, 10);
            if (errno == ERANGE || end == interval || *end != L'\0' || refresh < 500 || refresh > 60000)
            {
                errorBox(window, L"Enter a whole-number refresh interval from 500 to 60000 milliseconds.");
                SetFocus(state->interval);
                SendMessageW(state->interval, EM_SETSEL, 0, -1);
                return 0;
            }
            try
            {
                setDistroPrefix(state->distro, value);
                writeSetting(L"RefreshInterval", static_cast<DWORD>(refresh));
                DestroyWindow(window);
            }
            catch (const std::exception &error)
            {
                errorBox(window, wide(error.what()));
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

void showSettings(HWND owner, const std::wstring &distro)
{
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = settingsProc;
    cls.lpszClassName = L"WslTools.Settings";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        errorBox(owner, L"Could not register the settings window.");
        return;
    }
    auto state = std::make_unique<SettingsWindow>();
    state->distro = distro;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT bounds{0, 0, scale(owner, 636), scale(owner, 308)};
    AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_CONTROLPARENT, GetDpiForWindow(owner));
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"WSL Tools settings", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                                  bounds.bottom - bounds.top, owner, nullptr, instance, &state);
    if (!window)
    {
        errorBox(owner, L"Could not create the settings window.");
        return;
    }
    ShowWindow(window, SW_SHOW);
    SetFocus(GetDlgItem(window, PrefixEdit));
}
} // namespace wsl
