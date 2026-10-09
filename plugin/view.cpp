#include "controller.hpp"
#include "capture.hpp"
#include "settings.hpp"
#include "host_bridge.h"
#include "graphs.hpp"
#include "view_state.hpp"
#include "transport.hpp"
#include "resource_tooltips.hpp"
#include "resource_dialog.hpp"
#include "process_rules.hpp"
#include <algorithm>
#include <atomic>
#include <set>
#include <utility>
#include <windowsx.h>

namespace wsl::ui
{
std::wstring windowText(HWND window)
{
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
    value.resize(wcslen(value.c_str()));
    return value;
}
void status(View &v, const std::wstring &value)
{
    if (windowText(v.status) != value)
        SetWindowTextW(v.status, value.c_str());
}
void queue(View &v, Json request, uintptr_t tag)
{
    v.pending = true;
    submit(v.selectedDistro, std::move(request), v.mailbox, (static_cast<uintptr_t>(v.epoch) << 16) | tag);
}
namespace
{
bool contentVisible(const View &v)
{
    HWND host = GetAncestor(v.window, GA_ROOT);
    return v.active && IsWindowVisible(v.window) && IsWindowVisible(host) && !IsIconic(host);
}
Json snapshotRequest(const View &v);
void updateCaptureState(View &v)
{
    setCaptureView(v.window, v.selectedDistro, contentVisible(v), snapshotRequest(v));
}
Json mergeIdentitySnapshot(const Json &previous, const Json &incoming, const char *arrayName)
{
    if (!incoming.value("identities_only", false) || !previous.contains(arrayName))
        return incoming;
    const bool connections = std::string(arrayName) == "connections";
    auto key = [&](const Json &item) { return connections ? connectionKey(item) : item.value("name", ""); };
    std::map<std::string, Json> metadata;
    for (const auto &item : previous[arrayName])
        metadata.emplace(key(item), item);
    Json merged = incoming;
    for (auto &item : merged[arrayName])
    {
        const auto old = metadata.find(key(item));
        if (old != metadata.end())
        {
            Json updated = std::move(old->second);
            updated.update(item);
            item = std::move(updated);
        }
    }
    return merged;
}

Json snapshotRequest(const View &v)
{
    Json request{{"op", "snapshot"}, {"fields", Json::array()}};
    const bool savedScheduling = hasSavedScheduling(v.selectedDistro);
    if (!contentVisible(v) || v.page != 0)
    {
        if (savedScheduling)
            request["fields"].push_back("exe");
        return request;
    }
    std::set<std::string> fields;
    if (savedScheduling)
        fields.insert("exe");
    auto needs = [&](std::initializer_list<int> columns) {
        for (int column : columns)
            if (v.processes.isColumnVisible(column) || v.processes.sortColumn == column)
                return true;
        return false;
    };
    auto enabled = [](PCWSTR name) { return WslHostIntegerSetting(name) != 0; };
    const bool tooltips = enabled(L"EnableTooltipSupport");
    const bool interopFilter =
        readSetting(L"HideWindowsToWslInterop", 1) || readSetting(L"HideWslToWindowsInterop", 1);
    if (needs({ProcessUser}) || tooltips)
        fields.insert("user");
    // Search is intentionally restricted to the data actually collected. The
    // command line remains searchable when its column is visible (the default).
    if (needs({ProcessCommand}) || interopFilter || (tooltips && enabled(L"EnableCommandLineTooltips")))
        fields.insert("command");
    if (needs({ProcessRead, ProcessWrite, ProcessReadTotal, ProcessWriteTotal, ProcessReadChars,
               ProcessWriteChars, ProcessReadCalls, ProcessWriteCalls}))
        fields.insert("io");
    if (needs({ProcessExecutable}) || interopFilter || tooltips)
        fields.insert("exe");
    if (needs({ProcessDirectory}) || tooltips)
        fields.insert("cwd");
    if (needs({ProcessCgroup}) || enabled(L"UseColorServiceProcesses") || enabled(L"EnableTooltipSupport"))
        fields.insert("cgroup");
    if (needs({ProcessUid, ProcessEuid, ProcessGid, ProcessEgid, ProcessTracer, ProcessSwap,
               ProcessVoluntarySwitches, ProcessInvoluntarySwitches, ProcessSeccomp,
               ProcessNoNewPrivileges}) ||
        enabled(L"UseColorDebuggedProcesses") || enabled(L"UseColorOwnProcesses") ||
        enabled(L"UseColorSystemProcesses") || enabled(L"HideOtherUserProcesses") ||
        enabled(L"HideMicrosoftProcesses"))
        fields.insert("status");
    if (enabled(L"UseColorElevatedProcesses") || enabled(L"UseColorSystemProcesses") ||
        enabled(L"HideMicrosoftProcesses") || tooltips)
        fields.insert("sudo");
    if (enabled(L"UseColorSuspended") || enabled(L"UseColorPartiallySuspended"))
        fields.insert("suspension");
    if (readSetting(L"Detect32BitProcesses", 0) &&
        (needs({ProcessArchitecture}) || enabled(L"UseColorWow64Processes")))
    {
        fields.insert("elf32");
        request["detect_32bit"] = true;
    }
    for (const auto &field : fields)
        request["fields"].push_back(field);
    if (v.capture->defaultUid)
        request["default_uid"] = *v.capture->defaultUid;
    return request;
}
} // namespace
namespace
{
bool queueVisibleServices(View &v)
{
    if (!contentVisible(v) || v.page != 2)
        return false;
    const bool metadata = std::exchange(v.refreshServiceMetadata, false);
    queue(v,
          {{"op", "services"},
           {"refresh_metadata", metadata},
           {"include_pids", v.services.isColumnVisible(6) || v.services.sortColumn == 6 ||
                                WslHostIntegerSetting(L"UseColorServiceProcesses") != 0}},
          ServicesTag);
    return true;
}
} // namespace
void refresh(View &v)
{
    updateCaptureState(v);
    if (!v.selectedDistro.empty())
        refreshCapture(v.selectedDistro);
}
namespace
{
View *mainView = nullptr;
std::atomic<HWND> mainViewWindow{};
const wchar_t *viewClass = L"WslTools.View";

void layout(View &v)
{
    RECT rect{};
    GetClientRect(v.window, &rect);
    auto s = [&](int x) { return scale(v.window, x); };
    const int width = rect.right, height = rect.bottom, gap = s(4);
    auto checkWidth = [&](HWND check) {
        HDC dc = GetDC(check);
        HGDIOBJ previous = SelectObject(dc, font);
        SIZE textSize{};
        auto label = windowText(check);
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &textSize);
        SelectObject(dc, previous);
        ReleaseDC(check, dc);
        return textSize.cx + GetSystemMetricsForDpi(SM_CXMENUCHECK, GetDpiForWindow(check)) + s(6);
    };
    const int optionWidth = checkWidth(v.page == 0 ? v.tree : v.page == 1 ? v.listeners : v.inactiveServices);
    const int combo = std::max(s(110), std::min(s(300), width - s(350) - optionWidth - gap));
    place(v.distro, 0, 0, combo, s(300));
    // A dropdown's requested height includes its popup. Measure the collapsed
    // control so adjacent buttons have exactly the same visual height.
    RECT comboRect{};
    GetWindowRect(v.distro, &comboRect);
    const int line = std::max(s(20), static_cast<int>(comboRect.bottom - comboRect.top));
    place(v.settings, combo + gap, 0, s(88), line);
    place(v.exportButton, combo + s(88) + 2 * gap, 0, s(114), line);
    place(v.findHandles, combo + s(202) + 3 * gap, 0, s(118), line);
    place(v.tree, width - checkWidth(v.tree), 0, checkWidth(v.tree), line);
    place(v.listeners, width - checkWidth(v.listeners), 0, checkWidth(v.listeners), line);
    place(v.inactiveServices, width - checkWidth(v.inactiveServices), 0, checkWidth(v.inactiveServices),
          line);
    const int footerHeight = s(18);
    const int footerY = height - s(2) - footerHeight;
    place(v.status, s(2), footerY, width - s(4), footerHeight);

    const bool content = !v.capture->componentMissing;
    for (HWND child : {v.graph, v.memoryGraph, v.tabs, v.exportButton, v.findHandles})
        ShowWindow(child, content ? SW_SHOW : SW_HIDE);
    ShowWindow(v.processes.window, content && v.page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.connections.window, content && v.page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.services.window, content && v.page == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.tree, content && v.page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.listeners, content && v.page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.inactiveServices, content && v.page == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.installNotice, content ? SW_HIDE : SW_SHOW);
    ShowWindow(v.installButton, content ? SW_HIDE : SW_SHOW);
    EnableWindow(v.installButton, !v.pending);
    const bool globalSearch = WslHasGlobalSearch() != FALSE;
    ShowWindow(v.search, content && !globalSearch ? SW_SHOW : SW_HIDE);
    if (!content)
    {
        place(v.installNotice, s(20), line + s(24), std::max(s(200), width - s(40)), s(100));
        place(v.installButton, s(20), line + s(132), s(160), line);
        return;
    }
    const int graphY = line + gap, graphHeight = s(44);
    const int graphWidth = (width - gap) / 2;
    place(v.graph, 0, graphY, graphWidth, graphHeight);
    place(v.memoryGraph, graphWidth + gap, graphY, width - graphWidth - gap, graphHeight);
    const int tabsY = graphY + graphHeight + gap;
    RECT tabItem{};
    TabCtrl_GetItemRect(v.tabs, 0, &tabItem);
    const int tabsHeight = tabItem.bottom + s(2);
    place(v.tabs, 0, tabsY, width, tabsHeight);
    int tableTop = tabsY + tabsHeight;
    if (!globalSearch)
    {
        place(v.search, 0, tableTop + gap, width, editHeight(v.window));
        tableTop += gap + editHeight(v.window) + gap;
    }
    for (auto table : {&v.processes, &v.connections, &v.services})
        place(table->window, 0, tableTop, width, std::max(0, footerY - s(2) - tableTop));
}
void updateStatus(View &v)
{
    if (v.selectedDistro.empty())
    {
        status(v, L"No running WSL2 distributions. Start a distro, then use View > Refresh (F5).");
        return;
    }
    if (v.capture->componentMissing)
    {
        status(v, L"Component not installed in " + v.selectedDistro + L".");
        return;
    }
    if (v.capture->failed)
    {
        status(v, L"Disconnected: " + v.capture->error + L" · Use View > Refresh (F5) to reconnect.");
        return;
    }
    if (!WslHostRefreshAutomatically())
    {
        status(v, L"Automatic refresh is off. Use View > Refresh (F5) to update.");
        return;
    }
    if (v.capture->suspended)
    {
        status(v, L"Background capture is disabled. Capture resumes when this WSL tab is visible.");
        return;
    }
    if (!v.capture->snapshot.is_object())
    {
        status(v, L"Connecting to the selected distribution…");
        return;
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    std::wstring value = v.page == 0 && !v.capture->statistics.empty()
                             ? v.capture->statistics
                             : std::to_wstring(v.table().rows.size()) + L" visible rows";
    value += L" · " + std::wstring(stamp);
    if (v.page == 1 && v.sockets.is_object() && v.sockets.value("inaccessible_processes", 0) > 0)
        value += L" · some socket owners were inaccessible";
    if ((v.page == 0 && v.capture->snapshot.value("processes_truncated", false)) ||
        (v.page == 1 && v.sockets.is_object() && v.sockets.value("connections_truncated", false)))
        value += L" · collection limit reached (partial results)";
    const auto schedulingError = savedSchedulingError(v.selectedDistro);
    if (!schedulingError.empty())
        value += L" · " + schedulingError;
    status(v, value);
}
void refreshUiData(View &v)
{
    if (v.selectedDistro.empty() || v.capture->failed || (v.capture->suspended && !contentVisible(v)))
        return;
    if (v.pending)
    {
        v.refreshAfterPending = true;
        return;
    }
    v.refreshAfterPending = false;
    v.collectConnections = v.page == 1 || v.sockets.is_object();
    v.collectServices = contentVisible(v) && v.page == 2;
    if (v.collectConnections)
        queue(v,
              {{"op", "connections"},
               {"identities_only", !contentVisible(v) || v.page != 1},
               {"resolve_names", contentVisible(v) && v.page == 1 &&
                                     WslHostIntegerSetting(L"EnableNetworkResolve") &&
                                     v.connections.isColumnVisible(9)}},
              ConnectionsTag);
    else if (v.collectServices)
        queueVisibleServices(v);
}
void captureChanged(View &v)
{
    const auto names = captureDistros();
    if (names != v.distroNames)
    {
        v.distroNames = names;
        SendMessageW(v.distro, CB_RESETCONTENT, 0, 0);
        int selected = -1;
        for (size_t i = 0; i < names.size(); ++i)
        {
            SendMessageW(v.distro, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(names[i].c_str()));
            if (names[i] == v.selectedDistro)
                selected = static_cast<int>(i);
        }
        if (selected < 0 && !names.empty())
            selected = 0;
        SendMessageW(v.distro, CB_SETCURSEL, selected, 0);
        const auto chosen = selected >= 0 ? names[selected] : std::wstring{};
        if (chosen != v.selectedDistro)
        {
            clearDistro(v);
            v.selectedDistro = chosen;
            if (!chosen.empty())
                v.capture = captureModel(chosen);
            updateCaptureState(v);
        }
    }
    if (v.renderedRevision == v.capture->revision)
        return;
    const bool first = v.renderedRevision == UINT64_MAX;
    v.renderedRevision = v.capture->revision;
    const bool newSnapshot = v.capture->snapshot.is_object() && v.capture->previousTime > 0 &&
                             v.renderedSnapshotTime != v.capture->previousTime;
    const bool renderSnapshot = v.capture->snapshot.is_object() && (first || newSnapshot);
    v.renderedSnapshotTime = v.capture->previousTime;
    if (!v.renderedSnapshotTime)
    {
        v.refreshAfterPending = false;
        v.collectServices = false;
    }
    layout(v);
    PostMessageW(v.graph, GraphSampleChanged, 0, 0);
    PostMessageW(v.memoryGraph, GraphSampleChanged, 0, 0);
    if (renderSnapshot)
    {
        render(v, SnapshotTag);
        if (contentVisible(v) && v.page == 0 && !v.capture->newProcess.empty() &&
            WslHostIntegerSetting(L"ScrollToNewProcesses"))
        {
            const auto &rows = v.processes.rows;
            const auto added = std::find_if(rows.begin(), rows.end(), [&](const Row &row) {
                return row.key == v.capture->newProcess && !row.removed;
            });
            if (added != rows.end())
                v.processes.ensureVisible(static_cast<int>(added - rows.begin()));
        }
        if (v.page == 0 && !v.pendingSelection.empty())
        {
            v.processes.selectKey(v.pendingSelection);
            const int index = v.processes.selectedIndex();
            if (index >= 0)
                v.processes.ensureVisible(index);
            v.pendingSelection.clear();
        }
        if (newSnapshot)
            refreshUiData(v);
    }
    if (!v.pending)
        updateStatus(v);
}
void switchPage(View &v)
{
    const int tab = TabCtrl_GetCurSel(v.tabs);
    v.page = tab == 1 ? 2 : tab == 2 ? 1 : 0;
    if (v.page == 2)
        v.refreshServiceMetadata = true;
    ShowWindow(v.processes.window, v.page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.connections.window, v.page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.services.window, v.page == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.listeners, v.page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.tree, v.page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.inactiveServices, v.page == 2 ? SW_SHOW : SW_HIDE);
    SendMessageW(v.search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(v.page == 0   ? L"Filter name, PID, user or command…"
                                          : v.page == 1 ? L"Find port, address, PID or process…"
                                                        : L"Filter service, state or description…"));
    layout(v);
    render(v);
    v.refreshAfterPending = v.pending;
    refresh(v);
}
void manualRefresh(View &v)
{
    v.refreshServiceMetadata = true;
    updateCaptureState(v);
    refreshCaptures(true);
}
LRESULT CALLBACK childKeys(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data)
{
    auto v = reinterpret_cast<View *>(data);
    if (message == WM_KEYDOWN)
    {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && wparam == 'F' && !WslHasGlobalSearch())
        {
            SetFocus(v->search);
            SendMessageW(v->search, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wparam == VK_F5)
        {
            manualRefresh(*v);
            return 0;
        }
        if (window == v->table().window)
        {
            if (wparam == VK_RETURN)
            {
                inspect(*v);
                return 0;
            }
            if (ctrl && wparam == 'C')
            {
                action(*v, CopyRow);
                return 0;
            }
            if (wparam == VK_DELETE && v->page == 0)
            {
                action(*v, Terminate);
                return 0;
            }
        }
        if (wparam == VK_ESCAPE && window == v->search)
        {
            SetWindowTextW(v->search, L"");
            return 0;
        }
        if (wparam == VK_TAB)
        {
            HWND next = GetNextDlgTabItem(v->window, window, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            if (next)
                SetFocus(next);
            return 0;
        }
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, childKeys, 1);
    return DefSubclassProc(window, message, wparam, lparam);
}
LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto v = reinterpret_cast<View *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        v = new View;
        v->window = window;
        v->mailbox->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v));
        mainView = v;
        mainViewWindow = window;
    }
    if (!v)
        return DefWindowProcW(window, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE: {
        v->distro =
            control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, DistroCombo);
        v->settings = control(window, L"BUTTON", L"Settings...", WS_TABSTOP, SettingsButton);
        v->graph = createHistoryGraph(window, *v, false);
        v->memoryGraph = createHistoryGraph(window, *v, true);
        v->tabs = control(window, WC_TABCONTROLW, L"Views", WS_TABSTOP, ViewTabs);
        for (auto label : {L"Processes", L"Services", L"Network"})
        {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t *>(label);
            TabCtrl_InsertItem(v->tabs, TabCtrl_GetItemCount(v->tabs), &item);
        }
        v->search = control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, SearchEdit);
        v->listeners = control(window, L"BUTTON", L"Listening / bound ports only",
                               BS_AUTOCHECKBOX | WS_TABSTOP, ListenerCheck);
        v->tree = control(window, L"BUTTON", L"Show process tree", BS_AUTOCHECKBOX | WS_TABSTOP, TreeCheck);
        SendMessageW(v->tree, BM_SETCHECK, readSetting(L"ShowProcessTree", 0) ? BST_CHECKED : BST_UNCHECKED,
                     0);
        v->inactiveServices = control(window, L"BUTTON", L"Show inactive services",
                                      BS_AUTOCHECKBOX | WS_TABSTOP, InactiveServicesCheck);
        SendMessageW(v->inactiveServices, BM_SETCHECK,
                     readSetting(L"ShowInactiveServices", 0) ? BST_CHECKED : BST_UNCHECKED, 0);

        v->processes.kind = Table::Kind::Processes;
        v->connections.kind = Table::Kind::Network;
        v->services.kind = Table::Kind::Services;
        v->processes.infoTip = processTooltip;
        v->services.infoTip = serviceTooltip;
        v->connections.infoTip = networkTooltip;
        v->processes.create(window, ProcessTable,
                            {{L"Name", 180},
                             {L"PID", 70, true},
                             {L"User name", 110},
                             {L"CPU", 80, true},
                             {L"Resident set size", 120, true},
                             {L"I/O read rate", 115, true},
                             {L"I/O write rate", 115, true},
                             {L"State", 60},
                             {L"Threads", 65, true},
                             {L"Parent PID", 90, true},
                             {L"Command line", 540},
                             {L"UID", 75, true, false},
                             {L"Effective UID", 95, true, false},
                             {L"GID", 75, true, false},
                             {L"Effective GID", 95, true, false},
                             {L"TTY", 120, false, false},
                             {L"Nice", 70, true, false},
                             {L"Priority", 75, true, false},
                             {L"Relative start time", 135, true, false},
                             {L"Virtual size", 115, true, false},
                             {L"Session ID", 90, true, false},
                             {L"Process group", 105, true, false},
                             {L"Last CPU", 80, true, false},
                             {L"Minor faults", 100, true, false},
                             {L"Major faults", 100, true, false},
                             {L"File name", 360, false, false},
                             {L"Working directory", 300, false, false},
                             {L"Control group", 360, false, false},
                             {L"Tracer PID", 90, true, false},
                             {L"Swap", 95, true, false},
                             {L"I/O read bytes", 125, true, false},
                             {L"I/O write bytes", 125, true, false},
                             {L"Read characters", 140, true, false},
                             {L"Write characters", 140, true, false},
                             {L"I/O reads", 100, true, false},
                             {L"I/O writes", 100, true, false},
                             {L"Voluntary switches", 140, true, false},
                             {L"Involuntary switches", 150, true, false},
                             {L"Seccomp", 85, false, false},
                             {L"No new privileges", 130, false, false},
                             {L"Architecture", 95, false, false},
                             {L"User CPU time", 120, true, false},
                             {L"Kernel CPU time", 120, true, false},
                             {L"Scheduling policy", 130, false, false},
                             {L"CPU (average)", 115, true, false}},
                            3, true);

        v->connections.create(window, ConnectionTable,
                              {{L"Protocol", 80},
                               {L"Local address", 200},
                               {L"Local port", 90, true},
                               {L"Remote address", 200},
                               {L"Remote port", 90, true},
                               {L"State", 120},
                               {L"PID", 70, true},
                               {L"Name", 150},
                               {L"Socket inode", 110, true},
                               {L"Remote hostname", 220}});
        v->services.create(window, ServiceTable,
                           {{L"Name", 255},
                            {L"Status", 95},
                            {L"Substate", 105},
                            {L"Startup", 100},
                            {L"Load", 100},
                            {L"Description", 500},
                            {L"PID", 75, true}});
        v->exportButton = control(window, L"BUTTON", L"Export view...", WS_TABSTOP, ExportButton);
        v->findHandles = control(window, L"BUTTON", L"Find handles...", WS_TABSTOP, FindHandlesButton);
        v->installNotice =
            control(window, L"STATIC",
                    L"Install the WSL inspection component\r\n\r\n"
                    L"This distribution needs the WSL Tools observer to display processes, services and "
                    L"network connections. "
                    L"Install it as root in /usr/local/lib/system-informer-wsl. "
                    L"It runs only while connected; future component updates are applied automatically.",
                    SS_LEFT, 0);
        v->installButton = control(window, L"BUTTON", L"Install and retry", WS_TABSTOP, InstallButton);
        v->status = control(window, L"STATIC", L"Loading capture history…", SS_LEFT, 0);
        for (HWND child : {v->distro, v->settings, v->tabs, v->search, v->listeners, v->tree,
                           v->processes.window, v->connections.window, v->services.window, v->exportButton,
                           v->installButton, v->findHandles, v->inactiveServices})
            SetWindowSubclass(child, childKeys, 1, reinterpret_cast<DWORD_PTR>(v));
        v->tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                                      CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
        auto tip = [&](HWND child, const wchar_t *label) {
            TOOLINFOW info{sizeof(info)};
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = window;
            info.uId = reinterpret_cast<UINT_PTR>(child);
            info.lpszText = const_cast<wchar_t *>(label);
            SendMessageW(v->tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        };
        tip(v->findHandles, L"Find open files and mapped modules across processes in this distribution");
        tip(v->exportButton, L"Export the visible rows and columns");
        tip(v->settings, L"WSL options: CPU percentage, Inspector capture and Explorer path mapping");
        WslApplyTheme(window);
        switchPage(*v);
        layout(*v);
        SetTimer(window, 1, 500, nullptr);
        updateCaptureState(*v);
        captureChanged(*v);
        return 0;
    }
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(font);
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(window, &r);
        SetDCBrushColor(reinterpret_cast<HDC>(wparam), WslDialogBackground());
        FillRect(reinterpret_cast<HDC>(wparam), &r, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, WslDialogText());
        SetBkColor(dc, WslDialogBackground());
        SetDCBrushColor(dc, WslDialogBackground());
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    case WM_SIZE:
        layout(*v);
        return 0;
    case ResourceActionCompleted:
        v->refreshAfterPending = v->pending;
        refresh(*v);
        return 0;
    case WM_TIMER: {
        updateCaptureState(*v);
        const bool foreground = contentVisible(*v);
        if (foreground != v->foreground)
        {
            v->foreground = foreground;
            if (foreground)
            {
                v->refreshServiceMetadata = true;
                v->refreshAfterPending = v->pending;
                refresh(*v);
            }
        }
        if (v->cpuPercentOfTotal != (readSetting(L"CpuPercentOfTotal", 1) != 0))
            render(*v);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wparam);
        if (id == DistroCombo && HIWORD(wparam) == CBN_SELCHANGE)
        {
            auto chosen = windowText(v->distro);
            if (chosen != v->selectedDistro)
            {
                clearDistro(*v);
                v->selectedDistro = chosen;
                v->capture = captureModel(chosen);
                captureChanged(*v);
                refresh(*v);
            }
            return 0;
        }
        if (id == SearchEdit && HIWORD(wparam) == EN_CHANGE)
        {
            render(*v);
            return 0;
        }
        switch (id)
        {
        case InstallButton:
            if (v->capture->componentMissing && !v->pending)
            {
                queue(*v, {{"op", "install_component"}}, InstallTag);
                EnableWindow(v->installButton, FALSE);
                status(*v, L"Installing the WSL component as root…");
            }
            break;
        case RefreshButton:
            manualRefresh(*v);
            break;
        case SettingsButton:
            showSettings(window, v->selectedDistro);
            break;
        case TreeCheck:
            writeSetting(L"ShowProcessTree", SendMessageW(v->tree, BM_GETCHECK, 0, 0) == BST_CHECKED);
            render(*v);
            v->processes.centerSelection();
            break;
        case InactiveServicesCheck:
            writeSetting(L"ShowInactiveServices",
                         SendMessageW(v->inactiveServices, BM_GETCHECK, 0, 0) == BST_CHECKED);
            render(*v, ServicesTag);
            break;
        case ListenerCheck:
            render(*v);
            break;
        case FindHandlesButton:
            if (!v->selectedDistro.empty())
                openHandleSearch(window, v->selectedDistro);
            break;
        case ExportButton:
            saveText(window, v->table().exportText(),
                     v->page == 0   ? L"wsl-processes.tsv"
                     : v->page == 1 ? L"wsl-connections.tsv"
                                    : L"wsl-services.tsv");
            break;
        }
        return 0;
    }
    case SortResetMessage:
        if (reinterpret_cast<HWND>(lparam) == v->processes.window)
        {
            SendMessageW(v->tree, BM_SETCHECK, BST_UNCHECKED, 0);
            writeSetting(L"ShowProcessTree", 0);
            render(*v);
        }
        return 0;
    case ColumnsChangedMessage:
        v->refreshAfterPending = v->pending;
        refresh(*v);
        return 0;
    case WM_NOTIFY: {
        auto hdr = reinterpret_cast<NMHDR *>(lparam);
        if (hdr->hwndFrom == v->tabs && hdr->code == TCN_SELCHANGE)
        {
            switchPage(*v);
            return 0;
        }
        if (hdr->hwndFrom == v->processes.window && hdr->code == TableSortChanged)
        {
            if (!WslHostIntegerSetting(L"SortChildProcesses"))
            {
                SendMessageW(v->tree, BM_SETCHECK, BST_UNCHECKED, 0);
                writeSetting(L"ShowProcessTree", 0);
            }
            render(*v, SnapshotTag);
            return 0;
        }
        if (hdr->hwndFrom == v->table().window)
        {
            if (hdr->code == TableDoubleClick)
            {
                inspect(*v);
                return 0;
            }
            if (hdr->code == TableSelectionChanged)
            {
                updateButtons(*v);
                return 0;
            }
        }
        break;
    }
    case WM_CONTEXTMENU: {
        // Header menus belong to Table. A header right-click also generates a
        // context-menu message; do not turn that into a second resource menu.
        if (reinterpret_cast<HWND>(wparam) != v->table().window)
            return 0;
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (p.x == -1)
        {
            RECT r{};
            GetWindowRect(v->table().window, &r);
            p = {r.left + 30, r.top + 50};
        }
        menu(*v, p);
        return 0;
    }
    case WSL_VIEW_SETTINGS_CHANGED:
        WslHostViewSettingsChanged();
        return 0;
    case CaptureChangedMessage:
        captureChanged(*v);
        return 0;
    case ReplyMessage: {
        std::unique_ptr<Reply> reply(reinterpret_cast<Reply *>(lparam));
        updateCaptureState(*v);
        if ((reply->tag >> 16) != v->epoch)
            return 0;
        v->pending = false;
        auto tag = reply->tag & 0xffff;
        if (!reply->error.empty())
        {
            if (reply->componentMissing || tag == InstallTag)
            {
                layout(*v);
                if (tag == InstallTag)
                    errorBox(window, L"Could not install the WSL component.\r\n\r\n" + wide(reply->error));
                status(*v, tag == InstallTag ? L"Installation failed: " + wide(reply->error)
                                             : L"Component not installed in " + v->selectedDistro + L".");
                return 0;
            }
            if (tag == ActionTag || tag == ExecutableTag || tag == ServiceProcessTag)
            {
                // A refused signal or service action does not necessarily mean
                // the observer disconnected. Do not retry the action; refresh
                // its state through the existing connection instead.
                errorBox(window, wide(reply->error));
                status(*v, L"Action failed: " + wide(reply->error));
                refresh(*v);
                return 0;
            }
            status(*v,
                   L"Disconnected: " + wide(reply->error) + L"  ·  Use View > Refresh (F5) to reconnect.");
            return 0;
        }
        try
        {
            if (tag == ServiceProcessTag)
            {
                const auto name = std::exchange(v->pendingService, {});
                bool found = false;
                for (const auto &service : reply->data.value("services", Json::array()))
                    if (service.value("name", "") == name)
                    {
                        goToProcess(*v, service);
                        found = true;
                        break;
                    }
                if (!found)
                    errorBox(window, L"The service is no longer available.");
                return 0;
            }
            if (tag == ExecutableTag)
            {
                bool found = false;
                for (const auto &process : reply->data.at("processes"))
                    if (process.at("pid").dump() + ":" + process.at("start_ticks").dump() ==
                        v->pendingExecutable)
                    {
                        const auto path = text(process, "exe");
                        if (!path.empty())
                            openLinuxPath(window, v->selectedDistro, path);
                        else
                            errorBox(window, L"The executable is inaccessible or this is a kernel thread.");
                        found = true;
                        break;
                    }
                if (!found)
                    errorBox(window, L"The process has exited. Refresh the process list.");
                v->pendingExecutable.clear();
                return 0;
            }
            if (tag == InstallTag)
            {
                status(*v, L"Component installed. Loading processes…");
                refreshCapture(v->selectedDistro, true);
                return 0;
            }
            if (tag == ConnectionsTag)
            {
                v->sockets = mergeIdentitySnapshot(v->sockets, reply->data, "connections");
                render(*v, ConnectionsTag);
                if (v->collectServices && queueVisibleServices(*v))
                    return 0;
            }
            else if (tag == ServicesTag)
            {
                v->units = mergeIdentitySnapshot(v->units, reply->data, "services");
                render(*v, ServicesTag);
                if (!v->units.value("available", true))
                {
                    status(*v, text(v->units, "message", L"systemd is unavailable in this distribution."));
                    return 0;
                }
            }
            else if (tag == ActionTag)
            {
                status(*v, L"Action completed. Refreshing…");
                refresh(*v);
                return 0;
            }

            if (!v->pending && v->refreshAfterPending)
                refreshUiData(*v);
            if (!v->pending)
                updateStatus(*v);
        }
        catch (const std::exception &e)
        {
            status(*v, L"Invalid observer response: " + wide(e.what()));
        }
        return 0;
    }
    case WM_DESTROY:
        DestroyWindow(v->tooltips);
        KillTimer(window, 1);
        v->mailbox->detach();
        detachCaptureView(window);
        drainReplies(window);
        return 0;
    case WM_NCDESTROY:
        if (mainView == v)
        {
            mainViewWindow = nullptr;
            mainView = nullptr;
        }
        delete v;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace
void selectPage(View &view, int page)
{
    TabCtrl_SetCurSel(view.tabs, page == 1 ? 2 : page == 2 ? 1 : 0);
    switchPage(view);
}
} // namespace wsl::ui

extern "C" HWND WslCreateView(HWND parent, HINSTANCE dll)
{
    using namespace wsl;
    using namespace wsl::ui;
    instance = dll;
    if (!font)
        font = WslCreateUiFont(parent);
    if (!font)
        font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = viewProc;
    cls.lpszClassName = viewClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
    RegisterClassW(&cls);
    return CreateWindowExW(WS_EX_CONTROLPARENT, viewClass, L"WSL Tools",
                           WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr, instance,
                           nullptr);
}
extern "C" void WslSetActive(BOOL active)
{
    using namespace wsl;
    using namespace wsl::ui;
    if (!mainView)
        return;
    auto &v = *mainView;
    v.active = active != FALSE;
    updateCaptureState(v);
    if (v.active)
    {
        v.refreshServiceMetadata = true;
        render(v);
        refresh(v);
    }
}
extern "C" void WslShutdown(void)
{
    wsl::stopCapture();
    wsl::stopController();
}

extern "C" void WslFocusContent(BOOL select)
{
    using namespace wsl::ui;
    if (!mainView)
        return;
    if (mainView->capture->componentMissing)
    {
        SetFocus(mainView->installButton);
        return;
    }
    auto &table = mainView->table();
    SetFocus(table.window);
    if (select && !table.selected() && !table.rows.empty())
        table.selectKey(table.rows.front().key);
}
extern "C" void WslSearchChanged(void)
{
    using namespace wsl::ui;
    if (!mainView)
        return;
    layout(*mainView);
    render(*mainView);
}

extern "C" void WslHostRefreshChanged(BOOL)
{
    using namespace wsl;
    using namespace wsl::ui;
    captureSettingsChanged();
    if (mainView)
    {
        updateCaptureState(*mainView);
        updateStatus(*mainView);
    }
}
extern "C" void WslHostRefresh(void)
{
    using namespace wsl;
    using namespace wsl::ui;
    if (mainView && mainView->active)
        manualRefresh(*mainView);
    else
        refreshCaptures(true);
}

// View-menu settings do not emit the host Options callback. The bridge calls
// this after the host has applied a command, including while updates are paused.
extern "C" void WslHostViewSettingsChanged(void)
{
    using namespace wsl;
    using namespace wsl::ui;
    captureSettingsChanged();
    const HWND window = mainViewWindow.load();
    if (!window)
        return;
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
    {
        PostMessageW(window, WSL_VIEW_SETTINGS_CHANGED, 0, 0);
        return;
    }
    if (!mainView)
        return;
    render(*mainView);
    updateCaptureState(*mainView);
    if (contentVisible(*mainView))
        refresh(*mainView);
}
