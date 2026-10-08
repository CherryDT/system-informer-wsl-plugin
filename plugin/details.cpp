#include "common.hpp"
#include <algorithm>
#include <array>
#include <cwctype>

namespace wsl
{
namespace
{
constexpr wchar_t InspectorClass[] = L"WslTools.Inspector";
enum ControlId
{
    Banner = 201,
    Tabs,
    Overview,
    Files,
    Modules,
    Environment,
    Threads,
    Refresh,
    CopySelection,
    CopyAll,
    Save,
    OpenLocation,
    CopyPath,
    CopyValue,
    Status,
    FilterLabel,
    Filter,
    ClearFilter
};

// An inspector owns its controls and the mailbox, but never the controller's
// worker. Closing a window therefore never waits for a slow WSL command.
struct Inspector
{
    HWND window = nullptr;
    HWND banner = nullptr, tabs = nullptr, overview = nullptr, status = nullptr;
    HWND refresh = nullptr, copy = nullptr, copyAll = nullptr, save = nullptr;
    HWND open = nullptr, path = nullptr, value = nullptr;
    HWND filterLabel = nullptr, filter = nullptr, clearFilter = nullptr;
    std::array<Table, 4> tables;
    std::array<std::vector<Row>, 4> snapshots;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    std::wstring distro;
    Json process;
    std::string service;
    bool isService = false;
    bool loading = false;
    bool hasData = false;
    uintptr_t requestTag = 0;
    int page = 0;
    std::wstring notice;
    std::array<bool, 4> available{};
};

std::wstring windowText(HWND window)
{
    int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<size_t>(length));
    return result;
}

std::wstring editText(const std::wstring &text)
{
    // Win32 EDIT uses CRLF; procfs and journalctl return Unix line endings.
    std::wstring result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'))
            result += L'\r';
        result += text[i];
    }
    return result;
}

std::wstring folded(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::wstring cell(const Json &object, const char *key)
{
    if (!object.is_object())
        return L"";
    auto value = object.find(key);
    if (value == object.end() || value->is_null())
        return L"";
    if (value->is_string())
        return wide(value->get<std::string>());
    if (value->is_number() || value->is_boolean())
        return wide(value->dump());
    return L"";
}

std::string rowKey(const Json &object, const char *key)
{
    return utf8(cell(object, key));
}

Table *activeTable(Inspector &state)
{
    return state.page > 0 && state.page <= 4 ? &state.tables[state.page - 1] : nullptr;
}

std::wstring selectedPath(Inspector &state)
{
    if (state.page != 1 && state.page != 2)
        return L"";
    const Row *row = state.tables[state.page - 1].selected();
    if (!row)
        return L"";
    return cell(row->data, state.page == 1 ? "target" : "path");
}

bool canOpen(const std::wstring &path)
{
    // procfs also reports socket:[...], pipe:[...], and anon_inode:[...].
    // These are useful identifiers to copy, but are not filesystem paths.
    return !path.empty() && path.front() == L'/';
}

void updateActions(Inspector &state)
{
    bool pathTab = state.page == 1 || state.page == 2;
    ShowWindow(state.open, pathTab ? SW_SHOW : SW_HIDE);
    ShowWindow(state.path, pathTab ? SW_SHOW : SW_HIDE);
    ShowWindow(state.value, state.page == 3 ? SW_SHOW : SW_HIDE);
    std::wstring path = selectedPath(state);
    EnableWindow(state.open, !state.loading && canOpen(path));
    EnableWindow(state.path, !path.empty());
    EnableWindow(state.value, state.page == 3 && state.tables[2].selected());
    EnableWindow(state.refresh, !state.loading);

    std::wstring message = state.notice;
    if (message.empty() && state.page > 0)
    {
        if (!state.available[state.page - 1])
            message = L"This information is unavailable.";
        else
        {
            size_t count = state.tables[state.page - 1].rows.size();
            message = std::to_wstring(count) + (count == 1 ? L" entry" : L" entries");
            size_t total = state.snapshots[state.page - 1].size();
            if (count != total)
                message += L" shown of " + std::to_wstring(total);
            else if (count == 0)
                message += L" — no entries reported";
        }
    }
    if (state.notice.empty() && state.page == 3)
        message += L"  •  Ctrl+Shift+C copies the selected value";
    if (state.notice.empty() && pathTab)
        message += L"  •  Enter opens location  •  Ctrl+Shift+C copies path";
    if (message.empty())
        message = L"Ctrl+R refresh  •  Ctrl+C copy selection  •  Ctrl+S save this view";
    SetWindowTextW(state.status, message.c_str());
}

void showPage(Inspector &state)
{
    ShowWindow(state.overview, state.page == 0 ? SW_SHOW : SW_HIDE);
    for (size_t i = 0; i < state.tables.size(); ++i)
        if (state.tables[i].window)
            ShowWindow(state.tables[i].window, state.page == static_cast<int>(i + 1) ? SW_SHOW : SW_HIDE);
    if (state.filter)
    {
        EnableWindow(state.filter, state.page > 0);
        EnableWindow(state.clearFilter, state.page > 0);
    }
    updateActions(state);
}

void applyFilter(Inspector &state)
{
    std::wstring query = folded(windowText(state.filter));
    for (size_t i = 0; i < state.tables.size(); ++i)
    {
        if (!state.tables[i].window)
            continue;
        std::vector<Row> rows;
        for (const auto &row : state.snapshots[i])
        {
            bool match = query.empty();
            for (const auto &value : row.cells)
                if (!match && folded(value).find(query) != std::wstring::npos)
                    match = true;
            if (match)
                rows.push_back(row);
        }
        state.tables[i].replace(std::move(rows));
    }
    updateActions(state);
}

void layout(Inspector &state)
{
    RECT rect{};
    GetClientRect(state.window, &rect);
    int gap = scale(state.window, 12);
    int buttonHeight = scale(state.window, 28);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    place(state.banner, gap, gap, width - gap * 2, scale(state.window, 24));
    int buttonsY = gap + scale(state.window, 30);
    int x = gap;
    auto button = [&](HWND handle, int logicalWidth) {
        int buttonWidth = scale(state.window, logicalWidth);
        place(handle, x, buttonsY, buttonWidth, buttonHeight);
        x += buttonWidth + scale(state.window, 6);
    };
    button(state.refresh, 78);
    button(state.copy, 110);
    button(state.copyAll, 80);
    button(state.save, 80);
    int contextualX = x;
    button(state.open, 108);
    button(state.path, 88);
    x = contextualX;
    button(state.value, 100);
    int contentY = buttonsY + buttonHeight + gap;
    if (!state.isService)
    {
        int labelWidth = scale(state.window, 43);
        int clearWidth = scale(state.window, 70);
        place(state.filterLabel, gap, contentY + scale(state.window, 4), labelWidth, buttonHeight);
        place(state.filter, gap + labelWidth, contentY, width - gap * 3 - labelWidth - clearWidth,
              buttonHeight);
        place(state.clearFilter, width - gap - clearWidth, contentY, clearWidth, buttonHeight);
        contentY += buttonHeight + gap;
    }
    int statusHeight = scale(state.window, 23);
    place(state.status, gap, height - gap - statusHeight, width - 2 * gap, statusHeight);
    RECT body{gap, contentY, width - gap, height - 2 * gap - statusHeight};
    if (!state.isService)
    {
        place(state.tabs, body.left, body.top, body.right - body.left, body.bottom - body.top);
        RECT tabClient{};
        GetClientRect(state.tabs, &tabClient);
        TabCtrl_AdjustRect(state.tabs, FALSE, &tabClient);
        MapWindowPoints(state.tabs, state.window, reinterpret_cast<POINT *>(&tabClient), 2);
        body = tabClient;
    }
    place(state.overview, body.left, body.top, body.right - body.left, body.bottom - body.top);
    for (auto &table : state.tables)
        if (table.window)
            place(table.window, body.left, body.top, body.right - body.left, body.bottom - body.top);
}

void refresh(Inspector &state)
{
    if (state.loading)
        return;
    Json request;
    if (state.isService)
        request = {{"op", "service_details"}, {"name", state.service}};
    else
    {
        request = {{"op", "details"}};
        for (const char *key : {"pid", "start_ticks"})
        {
            auto value = state.process.find(key);
            if (value == state.process.end() || (!value->is_number_integer() && !value->is_string()))
            {
                state.notice = L"This process has no valid identity. Refresh the process list and try again.";
                SetWindowTextW(state.overview, state.notice.c_str());
                updateActions(state);
                return;
            }
            request[key] = *value;
        }
    }
    state.loading = true;
    state.notice = L"Loading from " + state.distro + L"…";
    updateActions(state);
    submit(state.distro, std::move(request), state.mailbox, ++state.requestTag);
}

std::wstring allText(Inspector &state)
{
    if (auto *table = activeTable(state))
        return table->exportText();
    return windowText(state.overview);
}

std::wstring selectionText(Inspector &state)
{
    if (auto *table = activeTable(state))
    {
        std::wstring result;
        // Preserve the visible order, including the user's chosen sort order.
        for (int item = ListView_GetNextItem(table->window, -1, LVNI_SELECTED); item >= 0;
             item = ListView_GetNextItem(table->window, item, LVNI_SELECTED))
        {
            if (static_cast<size_t>(item) >= table->rows.size())
                continue;
            if (!result.empty())
                result += L"\r\n";
            const auto &cells = table->rows[item].cells;
            for (size_t column = 0; column < cells.size(); ++column)
            {
                if (column)
                    result += L"\t";
                result += cells[column];
            }
        }
        return result.empty() ? allText(state) : result;
    }
    DWORD start = 0, end = 0;
    SendMessageW(state.overview, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    std::wstring result = windowText(state.overview);
    if (end > start && start < result.size())
        return result.substr(start, end - start);
    return result;
}

void command(Inspector &state, int id)
{
    switch (id)
    {
    case Refresh:
        refresh(state);
        break;
    case ClearFilter:
        SetWindowTextW(state.filter, L"");
        SetFocus(state.filter);
        break;
    case CopySelection:
        copyText(state.window, selectionText(state));
        break;
    case CopyAll:
        copyText(state.window, allText(state));
        break;
    case Save:
        saveText(state.window, allText(state), state.isService ? L"wsl-service.txt" : L"wsl-process.txt");
        break;
    case CopyPath: {
        std::wstring path = selectedPath(state);
        if (!path.empty())
            copyText(state.window, path);
        break;
    }
    case CopyValue:
        if (const Row *row = state.tables[2].selected())
            copyText(state.window, cell(row->data, "value"));
        break;
    case OpenLocation: {
        std::wstring path = selectedPath(state);
        if (!state.loading && canOpen(path))
            openLinuxPath(state.window, state.distro, path);
        break;
    }
    case IDCANCEL:
        DestroyWindow(state.window);
        break;
    }
}

void contextMenu(Inspector &state, HWND source, LPARAM position)
{
    Table *table = activeTable(state);
    if (!table || source != table->window)
        return;
    POINT point{static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
    if (point.x == -1 && point.y == -1)
    {
        int selected = ListView_GetNextItem(table->window, -1, LVNI_SELECTED);
        RECT row{};
        if (selected >= 0 && ListView_GetItemRect(table->window, selected, &row, LVIR_BOUNDS))
            point = {row.left + scale(state.window, 30), row.bottom};
        else
            point = {scale(state.window, 20), scale(state.window, 40)};
        ClientToScreen(table->window, &point);
    }
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, CopySelection, L"Copy &selection\tCtrl+C");
    AppendMenuW(menu, MF_STRING, CopyAll, L"Copy &all");
    AppendMenuW(menu, MF_STRING, Save, L"&Save this view…\tCtrl+S");
    if (state.page == 1 || state.page == 2)
    {
        std::wstring path = selectedPath(state);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (state.loading || !canOpen(path) ? MF_GRAYED : 0), OpenLocation,
                    L"&Open location\tEnter");
        AppendMenuW(menu, MF_STRING | (path.empty() ? MF_GRAYED : 0), CopyPath, L"Copy &path\tCtrl+Shift+C");
    }
    else if (state.page == 3)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (table->selected() ? 0 : MF_GRAYED), CopyValue,
                    L"Copy &value\tCtrl+Shift+C");
    }
    int selected =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, state.window, nullptr);
    DestroyMenu(menu);
    if (selected)
        command(state, selected);
}

void loadReply(Inspector &state, const Reply &reply)
{
    if (reply.tag != state.requestTag)
        return;
    state.loading = false;
    if (!reply.error.empty())
    {
        // Keep the previous snapshot visible when a refresh fails; label it
        // explicitly so it cannot be mistaken for current process information.
        state.notice =
            state.hasData ? L"Refresh failed (displayed data may be stale): " : L"Could not load details: ";
        state.notice += wide(reply.error);
        if (!state.hasData)
            SetWindowTextW(state.overview, editText(state.notice).c_str());
        updateActions(state);
        return;
    }
    state.notice.clear();
    state.hasData = true;
    if (state.isService)
    {
        std::wstring content = cell(reply.data, "text");
        SetWindowTextW(state.overview,
                       content.empty() ? L"No service details were returned." : editText(content).c_str());
        updateActions(state);
        return;
    }
    std::wstring summary = cell(reply.data, "summary");
    SetWindowTextW(state.overview,
                   summary.empty() ? L"No process summary was returned." : editText(summary).c_str());
    const char *sections[] = {"files", "modules", "environment", "threads"};
    const std::vector<std::vector<const char *>> fields = {{"fd", "target", "flags"},
                                                           {"path", "start", "end", "permissions"},
                                                           {"name", "value"},
                                                           {"tid", "name", "state", "wchan"}};
    for (size_t section = 0; section < state.tables.size(); ++section)
    {
        std::vector<Row> rows;
        auto values = reply.data.find(sections[section]);
        state.available[section] = reply.data.is_object() && values != reply.data.end() && values->is_array();
        std::string accessibleKey = std::string(sections[section]) + "_accessible";
        auto accessible = reply.data.find(accessibleKey);
        if (accessible != reply.data.end() && accessible->is_boolean() && !accessible->get<bool>())
            state.available[section] = false;
        if (state.available[section])
        {
            for (const auto &value : *values)
            {
                if (!value.is_object())
                    continue;
                Row row;
                row.data = value;
                for (const char *field : fields[section])
                    row.cells.push_back(cell(value, field));
                row.key = rowKey(value, fields[section][0]);
                // A module can have several mapped segments with the same path.
                if (section == 1)
                    row.key += ":" + rowKey(value, "start");
                rows.push_back(std::move(row));
            }
        }
        state.snapshots[section] = std::move(rows);
    }
    applyFilter(state);
}

// The host owns the message loop; subclass child controls so shortcuts also
// work while the list, edit control, or a toolbar button has keyboard focus.
LRESULT CALLBACK shortcutProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR,
                              DWORD_PTR reference)
{
    HWND inspector = reinterpret_cast<HWND>(reference);
    if (message == WM_CONTEXTMENU)
    {
        int id = GetDlgCtrlID(window);
        if (id >= Files && id <= Threads)
        {
            SendMessageW(inspector, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window), lParam);
            return 0;
        }
    }
    if (message == WM_KEYDOWN)
    {
        int id = 0;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        int controlId = GetDlgCtrlID(window);
        if (ctrl && wParam == 'A' && (controlId == Overview || controlId == Filter))
        {
            SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
        if (ctrl && wParam == 'R')
            id = Refresh;
        if (ctrl && wParam == 'S')
            id = Save;
        // Let the filter edit keep its normal text-copy shortcut.
        if (ctrl && wParam == 'C' && controlId != Filter)
        {
            id = CopySelection;
            HWND tabs = GetDlgItem(inspector, Tabs);
            int page = tabs ? TabCtrl_GetCurSel(tabs) : 0;
            if (shift && page == 3)
                id = CopyValue;
            else if (shift && (page == 1 || page == 2))
                id = CopyPath;
        }
        if (ctrl && wParam == 'F')
        {
            HWND filter = GetDlgItem(inspector, Filter);
            if (filter && IsWindowEnabled(filter))
            {
                SetFocus(filter);
                SendMessageW(filter, EM_SETSEL, 0, -1);
            }
            return 0;
        }
        if (wParam == VK_TAB)
        {
            if (ctrl)
            {
                HWND tabs = GetDlgItem(inspector, Tabs);
                if (tabs)
                {
                    int direction = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
                    int page = (TabCtrl_GetCurSel(tabs) + direction + 5) % 5;
                    TabCtrl_SetCurSel(tabs, page);
                    NMHDR notice{tabs, Tabs, TCN_SELCHANGE};
                    SendMessageW(inspector, WM_NOTIFY, Tabs, reinterpret_cast<LPARAM>(&notice));
                    SetFocus(tabs);
                }
            }
            else
            {
                HWND next = GetNextDlgTabItem(inspector, window, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                if (next)
                    SetFocus(next);
            }
            return 0;
        }
        if (wParam == VK_RETURN)
        {
            if (controlId == Files || controlId == Modules)
                id = OpenLocation;
            else if ((controlId >= Refresh && controlId <= CopyValue) || controlId == ClearFilter)
                id = controlId;
        }
        if (wParam == VK_ESCAPE)
            id = IDCANCEL;
        if (id)
        {
            SendMessageW(inspector, WM_COMMAND, id, 0);
            return 0;
        }
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, shortcutProc, 1);
    return DefSubclassProc(window, message, wParam, lParam);
}

BOOL CALLBACK installShortcuts(HWND child, LPARAM parent)
{
    SetWindowSubclass(child, shortcutProc, 1, static_cast<DWORD_PTR>(parent));
    return TRUE;
}

void createControls(Inspector &state)
{
    HWND window = state.window;
    std::wstring label = state.isService ? wide(state.service) : cell(state.process, "name");
    if (!state.isService)
        label += L"  ·  PID " + cell(state.process, "pid");
    label += L"  ·  " + state.distro;
    state.banner = control(window, WC_STATICW, label.c_str(), SS_LEFT | SS_NOPREFIX, Banner);
    state.refresh = control(window, WC_BUTTONW, L"&Refresh", BS_PUSHBUTTON | WS_TABSTOP, Refresh);
    state.copy = control(window, WC_BUTTONW, L"&Copy selection", BS_PUSHBUTTON | WS_TABSTOP, CopySelection);
    state.copyAll = control(window, WC_BUTTONW, L"Copy &all", BS_PUSHBUTTON | WS_TABSTOP, CopyAll);
    state.save = control(window, WC_BUTTONW, L"&Save…", BS_PUSHBUTTON | WS_TABSTOP, Save);
    state.open = control(window, WC_BUTTONW, L"&Open location", BS_PUSHBUTTON | WS_TABSTOP, OpenLocation);
    state.path = control(window, WC_BUTTONW, L"Copy &path", BS_PUSHBUTTON | WS_TABSTOP, CopyPath);
    state.value = control(window, WC_BUTTONW, L"Copy &value", BS_PUSHBUTTON | WS_TABSTOP, CopyValue);
    if (!state.isService)
    {
        state.filterLabel = control(window, WC_STATICW, L"Filter:", SS_LEFT, FilterLabel);
        state.filter = control(window, WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, Filter);
        SendMessageW(state.filter, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"Search all columns in this view (Ctrl+F)"));
        state.clearFilter = control(window, WC_BUTTONW, L"Clear", BS_PUSHBUTTON | WS_TABSTOP, ClearFilter);
        state.tabs =
            control(window, WC_TABCONTROLW, L"Process detail categories", WS_TABSTOP | WS_CLIPSIBLINGS, Tabs);
        const wchar_t *names[] = {L"Overview", L"Open files", L"Modules", L"Environment", L"Threads"};
        for (int i = 0; i < 5; ++i)
        {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t *>(names[i]);
            TabCtrl_InsertItem(state.tabs, i, &item);
        }
    }
    state.overview = control(window, WC_EDITW, L"Loading…",
                             WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                 ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                             Overview);
    SendMessageW(state.overview, EM_SETLIMITTEXT, 16 * 1024 * 1024, 0);
    if (!state.isService)
    {
        state.tables[0].create(window, Files, {{L"FD", 65, true}, {L"Target", 520}, {L"Flags", 170}});
        state.tables[1].create(
            window, Modules, {{L"Mapped file", 460}, {L"Start", 130}, {L"End", 130}, {L"Permissions", 100}});
        state.tables[2].create(window, Environment, {{L"Variable", 230}, {L"Value", 600}});
        state.tables[3].create(window, Threads,
                               {{L"TID", 85, true}, {L"Name", 210}, {L"State", 100}, {L"Wait channel", 390}});
        const wchar_t *labels[] = {L"Open file descriptors", L"Memory mapped modules",
                                   L"Environment variables", L"Process threads"};
        for (size_t i = 0; i < state.tables.size(); ++i)
            SetWindowTextW(state.tables[i].window, labels[i]);
    }
    state.status = control(window, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, Status);
    EnumChildWindows(window, installShortcuts, reinterpret_cast<LPARAM>(window));
    WslApplyTheme(window);
    showPage(state);
    layout(state);
    refresh(state);
}

LRESULT CALLBACK inspectorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *state = reinterpret_cast<Inspector *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        auto *incoming = static_cast<std::unique_ptr<Inspector> *>(
            reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
        state = incoming->release();
        state->window = window;
        state->mailbox->window.store(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, wParam, lParam);
    switch (message)
    {
    case WM_CREATE:
        createControls(*state);
        return 0;
    case WM_SIZE:
        layout(*state);
        return 0;
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
        info->ptMinTrackSize = {scale(window, 700), scale(window, 420)};
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT *rect = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout(*state);
        return 0;
    }
    case WM_CONTEXTMENU:
        contextMenu(*state, reinterpret_cast<HWND>(wParam), lParam);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == Filter && HIWORD(wParam) == EN_CHANGE)
            applyFilter(*state);
        else
            command(*state, LOWORD(wParam));
        return 0;
    case WM_NOTIFY: {
        auto *hdr = reinterpret_cast<NMHDR *>(lParam);
        if (hdr->hwndFrom == state->tabs && hdr->code == TCN_SELCHANGE)
        {
            state->page = TabCtrl_GetCurSel(state->tabs);
            showPage(*state);
            return 0;
        }
        for (auto &table : state->tables)
        {
            if (hdr->hwndFrom != table.window)
                continue;
            table.notify(hdr);
            if (hdr->code == NM_DBLCLK && (state->page == 1 || state->page == 2))
                command(*state, OpenLocation);
            if (hdr->code == LVN_ITEMCHANGED)
                updateActions(*state);
            break;
        }
        return 0;
    }
    case ReplyMessage: {
        std::unique_ptr<Reply> reply(reinterpret_cast<Reply *>(lParam));
        if (reply)
            loadReply(*state, *reply);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->mailbox->detach();
        drainReplies(window);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete state;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void openInspector(HWND owner, std::unique_ptr<Inspector> state)
{
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance;
    cls.lpfnWndProc = inspectorProc;
    cls.lpszClassName = InspectorClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        errorBox(owner, L"Could not register the WSL inspector window.");
        return;
    }
    std::wstring title = state->isService ? wide(state->service) : cell(state->process, "name");
    title += L" — " + state->distro + (state->isService ? L" service" : L" process");
    // WM_NCDESTROY owns the state as soon as WM_NCCREATE has accepted it.
    Inspector *raw = state.get();
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, InspectorClass, title.c_str(),
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  scale(owner, 1000), scale(owner, 690), owner, nullptr, instance, &state);
    if (!window)
    {
        errorBox(owner, L"Could not create the WSL inspector window.");
        return;
    }
    ShowWindow(window, SW_SHOW);
    SetFocus(raw->overview);
}
} // namespace

void openDetails(HWND owner, const std::wstring &distro, const Json &process)
{
    auto state = std::make_unique<Inspector>();
    state->distro = distro;
    state->process = process;
    openInspector(owner, std::move(state));
}

void openServiceDetails(HWND owner, const std::wstring &distro, const std::string &name)
{
    auto state = std::make_unique<Inspector>();
    state->distro = distro;
    state->service = name;
    state->isService = true;
    openInspector(owner, std::move(state));
}
} // namespace wsl
