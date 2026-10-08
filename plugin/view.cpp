#include "controller.hpp"
#include "settings.hpp"
#include "host_bridge.h"
#include "view_state.hpp"
#include <algorithm>
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
void refresh(View &v)
{
    if (!v.active || (v.paused && !v.forceRefresh) || v.pending || v.failed)
        return;
    if (v.selectedDistro.empty())
    {
        queue(v, {{"op", "discover"}}, DiscoverTag);
        return;
    }
    // Keep graphs live in every inner view, then refresh that view's rows.
    v.forceRefresh = false;
    queue(v, {{"op", "snapshot"}}, SnapshotTag);
}
namespace
{
View *mainView = nullptr;
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
    const int optionWidth = checkWidth(v.page == 1 ? v.listeners : v.tree);
    const int combo = std::max(s(110), std::min(s(300), width - s(224) - optionWidth - gap));
    place(v.distro, 0, 0, combo, s(300));
    // A dropdown's requested height includes its popup. Measure the collapsed
    // control so adjacent buttons have exactly the same visual height.
    RECT comboRect{};
    GetWindowRect(v.distro, &comboRect);
    const int line = std::max(s(20), static_cast<int>(comboRect.bottom - comboRect.top));
    place(v.settings, combo + gap, 0, s(88), line);
    place(v.exportButton, combo + s(88) + 2 * gap, 0, s(114), line);
    place(v.tree, width - checkWidth(v.tree), 0, checkWidth(v.tree), line);
    place(v.listeners, width - checkWidth(v.listeners), 0, checkWidth(v.listeners), line);
    const int footerHeight = s(18);
    const int footerY = height - gap - footerHeight;
    place(v.status, s(2), footerY, width - s(4), footerHeight);

    const bool content = !v.componentMissing;
    for (HWND child : {v.graph, v.memoryGraph, v.tabs, v.exportButton})
        ShowWindow(child, content ? SW_SHOW : SW_HIDE);
    ShowWindow(v.processes.window, content && v.page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.connections.window, content && v.page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.services.window, content && v.page == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.tree, content && v.page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.listeners, content && v.page == 1 ? SW_SHOW : SW_HIDE);
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
        place(table->window, 0, tableTop, width, std::max(0, footerY - gap - tableTop));
}
void switchPage(View &v)
{
    const int tab = TabCtrl_GetCurSel(v.tabs);
    v.page = tab == 1 ? 2 : tab == 2 ? 1 : 0;
    ShowWindow(v.processes.window, v.page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.connections.window, v.page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.services.window, v.page == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.listeners, v.page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.tree, v.page == 0 ? SW_SHOW : SW_HIDE);
    SendMessageW(v.search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(v.page == 0   ? L"Filter name, PID, user or command…"
                                          : v.page == 1 ? L"Find port, address, PID or process…"
                                                        : L"Filter service, state or description…"));
    layout(v);
    render(v);
    refresh(v);
}
LRESULT CALLBACK graphProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message != WM_PAINT)
        return DefWindowProcW(window, message, wparam, lparam);
    auto v = reinterpret_cast<View *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window, &ps);
    RECT r{};
    GetClientRect(window, &r);
    const bool dark = WslIsDarkTheme() != FALSE;
    HBRUSH background = CreateSolidBrush(dark ? RGB(40, 40, 40) : GetSysColor(COLOR_WINDOW));
    FillRect(dc, &r, background);
    DeleteObject(background);
    HPEN grid = CreatePen(PS_SOLID, 1, dark ? RGB(60, 60, 60) : GetSysColor(COLOR_3DFACE));
    auto old = SelectObject(dc, grid);
    for (int i = 1; i < 4; ++i)
    {
        MoveToEx(dc, 0, r.bottom * i / 4, nullptr);
        LineTo(dc, r.right, r.bottom * i / 4);
    }
    SelectObject(dc, old);
    DeleteObject(grid);
    const bool memory = v && window == v->memoryGraph;
    static const std::deque<double> emptyHistory;
    const auto &samples = !v ? emptyHistory : memory ? v->memoryHistory : v->history;
    if (v && samples.size() > 1)
    {
        HPEN line = CreatePen(PS_SOLID, 2, (memory ? RGB(150, 90, 185) : RGB(35, 155, 195)));
        old = SelectObject(dc, line);
        for (size_t i = 0; i < samples.size(); ++i)
        {
            int x = (static_cast<int>(i) + 120 - static_cast<int>(samples.size())) * (r.right - 2) / 119;
            int y =
                r.bottom - 2 - static_cast<int>(std::clamp(samples[i], 0.0, 100.0) * (r.bottom - 4) / 100.0);
            if (i)
                LineTo(dc, x, y);
            else
                MoveToEx(dc, x, y, nullptr);
        }
        SelectObject(dc, old);
        DeleteObject(line);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dark ? RGB(190, 190, 190) : GetSysColor(COLOR_GRAYTEXT));
    auto oldFont = SelectObject(dc, font);
    RECT label = r;
    InflateRect(&label, -scale(window, 5), -scale(window, 3));
    std::wstring caption = memory ? L"WSL VM memory used" : L"Distro CPU";
    if (!samples.empty())
        caption += L"  " + number(samples.back()) + L"%";
    DrawTextW(dc, caption.c_str(), -1, &label, DT_LEFT | DT_TOP | DT_SINGLELINE);
    HPEN border = CreatePen(PS_SOLID, 1, dark ? RGB(100, 100, 100) : GetSysColor(COLOR_3DSHADOW));
    auto oldPen = SelectObject(dc, border);
    auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);
    SelectObject(dc, oldFont);
    EndPaint(window, &ps);
    return 0;
}
void manualRefresh(View &v)
{
    if (v.pending)
        return;
    if (v.failed)
        disconnect(v.selectedDistro);
    v.failed = false;
    v.forceRefresh = true;
    queue(v, {{"op", "discover"}}, DiscoverTag);
    status(v, L"Discovering running WSL2 distributions…");
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
    }
    if (!v)
        return DefWindowProcW(window, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE: {
        v->paused = !WslHostRefreshAutomatically();
        v->distro =
            control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, DistroCombo);
        v->settings = control(window, L"BUTTON", L"Settings...", WS_TABSTOP, SettingsButton);
        v->graph = control(window, L"WslTools.Graph", L"CPU history", 0, 0);
        SetWindowLongPtrW(v->graph, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v));
        v->memoryGraph = control(window, L"WslTools.Graph", L"VM memory history", 0, 0);
        SetWindowLongPtrW(v->memoryGraph, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v));
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
        v->tree =
            control(window, L"BUTTON", L"Show process ancestry", BS_AUTOCHECKBOX | WS_TABSTOP, TreeCheck);

        v->processes.kind = Table::Kind::Processes;
        v->connections.kind = Table::Kind::Network;
        v->services.kind = Table::Kind::Services;
        v->processes.create(window, ProcessTable,
                            {{L"Process", 180},
                             {L"PID", 70, true},
                             {L"User", 90},
                             {L"CPU %", 80, true},
                             {L"RSS MiB", 90, true},
                             {L"Read KiB/s", 95, true},
                             {L"Write KiB/s", 95, true},
                             {L"State", 60},
                             {L"Threads", 65, true},
                             {L"PPID", 65, true},
                             {L"Command line", 540}},
                            3, true);

        v->connections.create(window, ConnectionTable,
                              {{L"Protocol", 80},
                               {L"Local address", 200},
                               {L"Port", 75, true},
                               {L"Remote address", 200},
                               {L"Remote port", 90, true},
                               {L"State", 120},
                               {L"PID", 70, true},
                               {L"Process", 150},
                               {L"Socket inode", 110, true}});
        v->services.create(window, ServiceTable,
                           {{L"Service", 255},
                            {L"Active", 95},
                            {L"Substate", 105},
                            {L"Startup", 100},
                            {L"Load", 100},
                            {L"Description", 500}});
        v->exportButton = control(window, L"BUTTON", L"Export view...", WS_TABSTOP, ExportButton);
        v->installNotice =
            control(window, L"STATIC",
                    L"Install the WSL inspection component\r\n\r\n"
                    L"This distribution needs the WSL Tools observer to display processes, services and "
                    L"network connections. "
                    L"Install it as root in /usr/local/lib/system-informer-wsl. "
                    L"It runs only while connected; future component updates are applied automatically.",
                    SS_LEFT, 0);
        v->installButton = control(window, L"BUTTON", L"Install and retry", WS_TABSTOP, InstallButton);
        v->status = control(window, L"STATIC",
                            L"Monitoring starts when this tab is selected. Stopped distros are not "
                            L"started intentionally.",
                            SS_LEFT, 0);
        for (HWND child : {v->distro, v->settings, v->tabs, v->search, v->listeners,
                           v->tree, v->processes.window, v->connections.window, v->services.window,
                           v->exportButton, v->installButton})
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
        tip(v->exportButton, L"Export the visible rows and columns");
        tip(v->settings, L"WSL options: CPU percentage, Inspector capture and Explorer path mapping");
        WslApplyTheme(window);
        switchPage(*v);
        layout(*v);
        SetTimer(window, 1, 500, nullptr);
        startController();
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
    case WM_TIMER: {

        if (v->cpuPercentOfTotal != (readSetting(L"CpuPercentOfTotal", 1) != 0))
            render(*v);
        auto now = GetTickCount64();
        auto interval = std::max(1ul, WslHostRefreshInterval());
        if (now - v->lastRefresh >= interval)
        {
            v->lastRefresh = now;
            refresh(*v);
        }
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
                v->forceRefresh = true;
                layout(*v);
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
            if (v->componentMissing && !v->pending)
            {
                v->forceRefresh = true;
                v->failed = false;
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
        case ListenerCheck:
            render(*v);
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
    case WM_NOTIFY: {
        auto hdr = reinterpret_cast<NMHDR *>(lparam);
        if (hdr->hwndFrom == v->tabs && hdr->code == TCN_SELCHANGE)
        {
            switchPage(*v);
            return 0;
        }
        if (hdr->hwndFrom == v->processes.window && hdr->code == LVN_COLUMNCLICK)
            SendMessageW(v->tree, BM_SETCHECK, BST_UNCHECKED, 0);
        for (auto table : {&v->processes, &v->connections, &v->services})
        {
            if (hdr->hwndFrom == table->window && hdr->code == NM_CUSTOMDRAW)
                return table->customDraw(reinterpret_cast<NMLVCUSTOMDRAW *>(hdr));
            if (table->notify(hdr))
                return 0;
        }
        if (hdr->hwndFrom == v->table().window)
        {
            if (hdr->code == NM_DBLCLK)
            {
                inspect(*v);
                return 0;
            }
            if (hdr->code == LVN_ITEMCHANGED)
            {
                updateButtons(*v);
                return 0;
            }
        }
        break;
    }
    case WM_CONTEXTMENU: {
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
    case ReplyMessage: {
        std::unique_ptr<Reply> reply(reinterpret_cast<Reply *>(lparam));
        if ((reply->tag >> 16) != v->epoch)
            return 0;
        v->pending = false;
        auto tag = reply->tag & 0xffff;
        if (!reply->error.empty())
        {
            if (reply->componentMissing || tag == InstallTag)
            {
                v->componentMissing = true;
                v->failed = true;
                layout(*v);
                if (tag == InstallTag)
                    errorBox(window, L"Could not install the WSL component.\r\n\r\n" + wide(reply->error));
                status(*v, tag == InstallTag ? L"Installation failed: " + wide(reply->error)
                                             : L"Component not installed in " + v->selectedDistro + L".");
                return 0;
            }
            if (tag == ActionTag)
            {
                // A refused signal or service action does not necessarily mean
                // the observer disconnected. Do not retry the action; refresh
                // its state through the existing connection instead.
                errorBox(window, wide(reply->error));
                status(*v, L"Action failed: " + wide(reply->error));
                refresh(*v);
                return 0;
            }
            v->failed = true;
            status(*v, L"Disconnected: " + wide(reply->error) + L"  ·  Use View > Refresh (F5) to reconnect.");
            return 0;
        }
        try
        {
            if (tag == InstallTag)
            {
                v->componentMissing = false;
                v->failed = false;
                status(*v, L"Component installed. Loading processes…");
                switchPage(*v);
                return 0;
            }
            if (tag == DiscoverTag)
            {
                auto old = v->selectedDistro;
                SendMessageW(v->distro, CB_RESETCONTENT, 0, 0);
                int chosen = -1, index = 0;
                for (auto &name : reply->data)
                {
                    auto value = wide(name.get<std::string>());
                    SendMessageW(v->distro, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
                    if (value == old)
                        chosen = index;
                    ++index;
                }
                if (!index)
                {
                    clearDistro(*v);
                    v->selectedDistro.clear();
                    layout(*v);
                    v->failed = true;
                    status(*v, L"No running WSL2 distributions. Start a distro, then use View > Refresh (F5).");

                    updateButtons(*v);
                    return 0;
                }
                if (chosen < 0)
                    chosen = 0;
                SendMessageW(v->distro, CB_SETCURSEL, chosen, 0);
                auto distro = windowText(v->distro);
                if (distro != old)
                {
                    clearDistro(*v);
                    v->selectedDistro = distro;
                }
                refresh(*v);
            }
            else if (tag == SnapshotTag)
            {
                if (v->componentMissing)
                {
                    v->componentMissing = false;
                    layout(*v);
                }
                updateSnapshot(*v, reply->data);
                render(*v);
                if (v->page == 0 && !v->pendingSelection.empty())
                {
                    v->processes.selectKey(v->pendingSelection);
                    int index = ListView_GetNextItem(v->processes.window, -1, LVNI_SELECTED);
                    if (index >= 0)
                        ListView_EnsureVisible(v->processes.window, index, FALSE);
                    else
                        status(*v, L"The socket owner has exited. Refresh the connections view.");
                    v->pendingSelection.clear();
                }
                if (v->page != 0)
                {
                    queue(*v, {{"op", v->page == 1 ? "connections" : "services"}},
                          v->page == 1 ? ConnectionsTag : ServicesTag);
                    return 0;
                }
            }
            else if (tag == ConnectionsTag)
            {
                v->sockets = reply->data;
                render(*v);
            }
            else if (tag == ServicesTag)
            {
                v->units = reply->data;
                render(*v);
                if (!v->units.value("available", true))
                {
                    status(*v, text(v->units, "message", L"systemd is unavailable in this distribution."));
                    return 0;
                }
            }
            else if (tag == ActionTag)
            {
                status(*v, L"Action completed. Refreshing…");
                v->forceRefresh = true;
                refresh(*v);
                return 0;
            }

            if (!v->pending)
            {
                SYSTEMTIME time{};
                GetLocalTime(&time);
                wchar_t stamp[32];
                swprintf_s(stamp, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
                auto statusText = L"Root · " + v->selectedDistro + L" · ";
                if (v->page == 0 && !v->statistics.empty())
                    statusText += v->statistics;
                else
                    statusText += std::to_wstring(v->table().rows.size()) + L" visible rows";
                statusText += L" · " + std::wstring(stamp);
                if (v->page == 1 && v->sockets.value("inaccessible_processes", 0) > 0)
                    statusText += L" · some socket owners were inaccessible";
                if ((v->page == 0 && v->snapshot.value("processes_truncated", false)) ||
                    (v->page == 1 && v->sockets.value("connections_truncated", false)))
                    statusText += L" · collection limit reached (partial results)";
                status(*v, statusText);
            }
        }
        catch (const std::exception &e)
        {
            v->failed = true;
            status(*v, L"Invalid observer response: " + wide(e.what()));
        }
        return 0;
    }
    case WM_DESTROY:
        DestroyWindow(v->tooltips);
        KillTimer(window, 1);
        v->mailbox->detach();
        disconnect(v->selectedDistro);
        drainReplies(window);
        return 0;
    case WM_NCDESTROY:
        if (mainView == v)
            mainView = nullptr;
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
    WNDCLASSW graph{};
    graph.hInstance = instance;
    graph.lpfnWndProc = graphProc;
    graph.lpszClassName = L"WslTools.Graph";
    graph.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&graph);
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
    if (v.active)
    {
        v.failed = false;
        v.paused = !WslHostRefreshAutomatically();
        v.forceRefresh = !v.snapshot.is_object();
        v.previousTime = 0;
        refresh(v);
    }
    else
    {
        disconnect(v.selectedDistro);
        ++v.epoch;
        v.pending = false;
        status(v, L"Collector disconnected while the WSL tab is hidden.");
    }
}
extern "C" void WslShutdown(void)
{
    wsl::stopController();
}

extern "C" void WslFocusContent(BOOL select)
{
    using namespace wsl::ui;
    if (!mainView)
        return;
    if (mainView->componentMissing)
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

extern "C" void WslHostRefreshChanged(BOOL automatic)
{
    using namespace wsl;
    using namespace wsl::ui;
    if (!mainView) return;
    auto &v = *mainView;
    v.paused = !automatic;
    if (v.paused)
    {
        disconnect(v.selectedDistro);
        ++v.epoch;
        v.pending = false;
        v.forceRefresh = false;
        status(v, L"Automatic refresh is off. Use View > Refresh (F5) to update.");
    }
    else
    {
        v.previousTime = 0;
        v.lastRefresh = 0;
        refresh(v);
    }
}
extern "C" void WslHostRefresh(void)
{
    using namespace wsl::ui;
    if (mainView && mainView->active) manualRefresh(*mainView);
}
