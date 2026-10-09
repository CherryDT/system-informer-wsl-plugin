#include "settings.hpp"
#include "host_bridge.h"
#include "options.h"
#include "transport.hpp"
#include <algorithm>
#include <stdexcept>
#include <limits>
#include <string_view>

namespace wsl
{
namespace
{
// Serialize plugin read/modify/write operations, including callers on worker
// threads. Compare the live host string before using cached JSON so Advanced
// settings edits and Reset never get overwritten by an old cached document.
std::mutex settingsMutex;

class HostString
{
  public:
    explicit HostString(WSL_STRING_SETTING setting)
    {
        if (!WslHostGetStringSetting(setting, &value))
            throw std::runtime_error("The WSL setting exceeds the 1 MiB limit.");
    }
    ~HostString()
    {
        WslHostReleaseStringSetting(&value);
    }
    HostString(const HostString &) = delete;
    HostString &operator=(const HostString &) = delete;
    std::wstring_view text() const
    {
        return {value.Buffer, value.Length};
    }

  private:
    WSL_HOST_STRING value{};
};

struct ObjectSetting
{
    std::wstring source;
    Json document;
    bool loaded = false;
    bool valid = false;
};
ObjectSetting preferences;
ObjectSetting pathOverrides;

ObjectSetting &objectSetting(WSL_STRING_SETTING setting)
{
    auto &cache = setting == WslPreferencesSetting ? preferences : pathOverrides;
    HostString current(setting);
    if (!cache.loaded || current.text() != std::wstring_view(cache.source))
    {
        cache.source = current.text();
        cache.loaded = true;
        cache.valid = false;
        cache.document = nullptr;
        try
        {
            cache.document = Json::parse(utf8(cache.source));
            cache.valid = cache.document.is_object();
        }
        catch (const std::exception &)
        {
            // Reads may use their fallback, but mutations must preserve bad
            // input until the user corrects it in the host's Advanced settings.
        }
    }
    return cache;
}

void requireObject(const ObjectSetting &cache, const char *name)
{
    if (!cache.valid)
        throw std::runtime_error(std::string("Cannot save WSL ") + name +
                                 ": the stored setting is not a valid JSON object. "
                                 "Correct it in System Informer's Advanced settings first.");
}

void setString(WSL_STRING_SETTING setting, const std::wstring &value)
{
    if (!WslHostSetStringSetting(setting, value.data(), value.size()))
        throw std::runtime_error("The WSL setting exceeds the 1 MiB limit.");
}

void storeObject(WSL_STRING_SETTING setting, ObjectSetting &cache, Json replacement)
{
    auto value = wide(replacement.dump());
    setString(setting, value);
    cache.source = std::move(value);
    cache.document = std::move(replacement);
}

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
    std::lock_guard<std::mutex> lock(settingsMutex);
    try
    {
        const auto &cache = objectSetting(WslPreferencesSetting);
        if (!cache.valid)
            return fallback;
        const auto found = cache.document.find(utf8(name));
        if (found == cache.document.end() || !found->is_number_integer() ||
            (!found->is_number_unsigned() && found->get<int64_t>() < 0))
            return fallback;
        const auto value = found->get<uint64_t>();
        return value <= std::numeric_limits<DWORD>::max() ? static_cast<DWORD>(value) : fallback;
    }
    catch (const std::exception &)
    {
        return fallback;
    }
}
void writeSetting(const wchar_t *name, DWORD value)
{
    std::lock_guard<std::mutex> lock(settingsMutex);
    auto &cache = objectSetting(WslPreferencesSetting);
    requireObject(cache, "preferences");
    auto replacement = cache.document;
    replacement[utf8(name)] = value;
    storeObject(WslPreferencesSetting, cache, std::move(replacement));
}
std::wstring readStringSetting(WSL_STRING_SETTING setting)
{
    std::lock_guard<std::mutex> lock(settingsMutex);
    return std::wstring(HostString(setting).text());
}
void writeStringSetting(WSL_STRING_SETTING setting, const std::wstring &value)
{
    std::lock_guard<std::mutex> lock(settingsMutex);
    setString(setting, value);
}
std::wstring distroPrefix(const std::wstring &distro)
{
    std::lock_guard<std::mutex> lock(settingsMutex);
    const auto &cache = objectSetting(WslPathOverridesSetting);
    requireObject(cache, "path overrides");
    const auto found = cache.document.find(utf8(distro));
    if (found == cache.document.end())
        return L"";
    if (!found->is_string())
        throw std::runtime_error("The path override is not text. Correct it in WSL Tools settings.");
    auto value = wide(found->get<std::string>());
    if (!validPrefix(value))
        throw std::runtime_error("The path override is invalid. Correct it in WSL Tools settings.");
    return value;
}
void setDistroPrefix(const std::wstring &distro, const std::wstring &prefix)
{
    if (!validPrefix(prefix))
        throw std::runtime_error("Use an absolute drive path or a UNC path containing a server and share.");
    std::lock_guard<std::mutex> lock(settingsMutex);
    auto &cache = objectSetting(WslPathOverridesSetting);
    requireObject(cache, "path overrides");
    auto replacement = cache.document;
    if (prefix.empty())
        replacement.erase(utf8(distro));
    else
        replacement[utf8(distro)] = utf8(prefix);
    storeObject(WslPathOverridesSetting, cache, std::move(replacement));
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
    const bool mountedDrive =
        path.size() >= 6 && path.compare(0, 5, L"/mnt/") == 0 &&
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
            throw std::runtime_error("The path override is invalid. Correct it in WSL Tools settings.");
        if (prefix.empty())
            prefix = L"\\\\wsl.localhost\\" + distro + L"\\";
        if (prefix.back() != L'\\')
            prefix += L'\\';
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
    std::wstring prefix;
    try
    {
        if (available)
            prefix = distroPrefix(state.distros[state.selected]);
    }
    catch (const std::exception &error)
    {
        optionsStatus(window, wide(error.what()));
    }
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
    position(IDC_WSL_GENERAL_GROUP, 7, 7, 7, 165);
    position(IDC_WSL_CPU_LABEL, 14, 20, 14, 10);
    position(IDC_WSL_CPU_MODE, 14, 33, 14, 70);
    position(IDC_WSL_NODE_INSPECTOR, 14, 53, 14, 20);
    position(IDC_WSL_DETECT_32BIT, 14, 76, 14, 14);
    position(IDC_WSL_BACKGROUND_CAPTURE, 14, 94, 14, 14);
    position(IDC_WSL_HIDE_WINDOWS_TO_WSL, 14, 112, 14, 14);
    position(IDC_WSL_HIDE_WSL_TO_WINDOWS, 14, 130, 14, 14);
    position(IDC_WSL_REFRESH_NOTE, 14, 150, 14, 16);
    position(IDC_WSL_PATH_GROUP, 7, 179, 7, 99);
    position(IDC_WSL_DISTRO_LABEL, 14, 192, 14, 10);
    position(IDC_WSL_DISTRO, 14, 205, 14, 70);
    position(IDC_WSL_PREFIX_LABEL, 14, 225, 14, 10);
    position(IDC_WSL_PREFIX, 14, 238, 89, 14);
    RECT button{0, 237, 70, 16};
    MapDialogRect(window, &button);
    RECT margin{14, 0, 0, 0};
    MapDialogRect(window, &margin);
    place(GetDlgItem(window, IDC_WSL_APPLY_PREFIX), bounds.right - margin.left - button.right, button.top,
          button.right, button.bottom);
    position(IDC_WSL_PREFIX_HINT, 14, 258, 14, 14);
    position(IDC_WSL_OPTIONS_STATUS, 7, 284, 7, 18);
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
                       readSetting(L"UseNodeInspectorWithoutAsking", 1) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, IDC_WSL_DETECT_32BIT,
                       readSetting(L"Detect32BitProcesses", 0) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, IDC_WSL_BACKGROUND_CAPTURE,
                       readSetting(L"EnableBackgroundCapture", 1) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, IDC_WSL_HIDE_WINDOWS_TO_WSL,
                       readSetting(L"HideWindowsToWslInterop", 1) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, IDC_WSL_HIDE_WSL_TO_WINDOWS,
                       readSetting(L"HideWslToWindowsInterop", 1) ? BST_CHECKED : BST_UNCHECKED);
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
            if (id == IDC_WSL_BACKGROUND_CAPTURE && event == BN_CLICKED)
            {
                writeSetting(L"EnableBackgroundCapture",
                             IsDlgButtonChecked(window, id) == BST_CHECKED ? 1 : 0);
                optionsStatus(window, L"Background capture preference saved.");
                return TRUE;
            }
            if ((id == IDC_WSL_HIDE_WINDOWS_TO_WSL || id == IDC_WSL_HIDE_WSL_TO_WINDOWS) &&
                event == BN_CLICKED)
            {
                writeSetting(id == IDC_WSL_HIDE_WINDOWS_TO_WSL ? L"HideWindowsToWslInterop"
                                                               : L"HideWslToWindowsInterop",
                             IsDlgButtonChecked(window, id) == BST_CHECKED ? 1 : 0);
                WslHostViewSettingsChanged();
                optionsStatus(window, L"Interop process filter saved.");
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
