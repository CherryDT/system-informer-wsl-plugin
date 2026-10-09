#include "common.hpp"
#include "host_bridge.h"

#include <algorithm>
#include <windowsx.h>

namespace wsl
{
namespace
{
constexpr wchar_t SearchClass[] = L"WslTools.HandleSearch";
enum ControlId
{
    Query = 500,
    Search,
    Cancel,
    Results,
    Status,
    Close,
    OpenProcess,
    OpenLocation,
    Copy,
    CopyAll
};
struct SearchWindow
{
    HWND window = nullptr, query = nullptr, search = nullptr, cancel = nullptr, status = nullptr,
         close = nullptr;
    HFONT uiFont = nullptr;
    std::wstring distro;
    Table table;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    uintptr_t generation = 0;
    ULONG_PTR match = 0;
    std::wstring lastQuery;
    bool caseSensitive = false, regex = false;
    bool modesKnown = true;
    bool busy = false;
};

void layout(SearchWindow &state)
{
    RECT bounds{};
    GetClientRect(state.window, &bounds);
    const int gap = scale(state.window, 8), button = scale(state.window, 86);
    const int height = editHeight(state.window), width = bounds.right;
    place(state.query, gap, gap, std::max(1, width - 4 * gap - 2 * button), height);
    place(state.search, width - 2 * (gap + button), gap, button, height);
    place(state.cancel, width - gap - button, gap, button, height);
    const int footer = bounds.bottom - gap - height;
    place(state.table.window, gap, height + 2 * gap, std::max(1, width - 2 * gap),
          std::max(1, footer - height - 3 * gap));
    place(state.status, gap, footer + scale(state.window, 3), std::max(1, width - button - 3 * gap), height);
    place(state.close, width - gap - button, footer, button, height);
}
void setBusy(SearchWindow &state, bool busy)
{
    state.busy = busy;
    EnableWindow(state.search, !busy);
    EnableWindow(state.cancel, busy);
}
void beginSearch(SearchWindow &state)
{
    if (state.busy)
        return;
    const int length = GetWindowTextLengthW(state.query);
    if (!length)
    {
        SetFocus(state.query);
        return;
    }
    std::wstring query(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(state.query, query.data(), static_cast<int>(query.size()));
    query.resize(length);
    if (!state.match)
    {
        SetWindowTextW(state.status, L"Enter a valid search expression.");
        SetFocus(state.query);
        return;
    }
    state.table.clear();
    setBusy(state, true);
    SetWindowTextW(state.status, L"Searching handles and mapped files...");
    const bool nonAscii = std::any_of(query.begin(), query.end(), [](wchar_t c) { return c >= 0x80; });
    // Keep ordinary literal searches cheap. PCRE and Unicode-insensitive
    // matching are performed by the host control, so those searches request
    // candidates without an incompatible Linux-side regular expression engine.
    submit(state.distro,
           {{"op", "find_handles"},
            {"query", utf8(query)},
            {"case_sensitive", state.caseSensitive},
            {"enumerate", !state.modesKnown || state.regex || (!state.caseSensitive && nonAscii)}},
           state.mailbox, ++state.generation);
}
void cancelSearch(SearchWindow &state)
{
    if (!state.busy)
        return;
    // Detach queued work and discard an in-flight reply without interrupting
    // the shared observer used by the main tab and other property windows.
    // An active procfs scan has its own five-second deadline.
    state.mailbox->detach();
    state.mailbox = std::make_shared<Mailbox>();
    state.mailbox->window = state.window;
    ++state.generation;
    setBusy(state, false);
    SetWindowTextW(state.status, L"Search canceled.");
}
void CALLBACK queryChanged(ULONG_PTR match, void *context)
{
    auto &state = *static_cast<SearchWindow *>(context);
    // Match handles belong to the native edit control. Discard an outstanding
    // scan before its query or options change; never apply a new expression to
    // candidates collected with a different literal prefilter.
    cancelSearch(state);
    state.match = match;
    const int length = GetWindowTextLengthW(state.query);
    std::wstring query(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(state.query, query.data(), static_cast<int>(query.size()));
    query.resize(length);
    const bool caseSensitive = WslHostIntegerSetting(L"SearchControlCaseSensitive") != 0;
    const bool regex = WslHostIntegerSetting(L"SearchControlRegex") != 0;
    if (query == state.lastQuery)
    {
        // A callback with unchanged text follows an option change in this
        // control (or the host synchronizing its options). Typing alone must
        // not import preferences changed by a different search box.
        state.caseSensitive = caseSensitive;
        state.regex = regex;
        state.modesKnown = true;
    }
    else if (caseSensitive != state.caseSensitive || regex != state.regex)
    {
        // Host versions differ: some synchronize shared options while typing,
        // others keep their local buttons. Without a public option accessor,
        // neither interpretation can justify discarding Linux candidates.
        state.modesKnown = false;
    }
    state.lastQuery = std::move(query);
    if (state.status)
        SetWindowTextW(state.status, L"Press \"Search\" to find handles and mapped files by name.");
}
void command(SearchWindow &state, int id)
{
    const Row *selected = state.table.selected();
    const Row *actionable = state.table.selectedActionable();
    if (id == Search)
        beginSearch(state);
    else if (id == Cancel)
        cancelSearch(state);
    else if (id == Close || id == IDCANCEL)
        DestroyWindow(state.window);
    else if (id == OpenProcess && actionable)
    {
        Json process = actionable->data;
        process["name"] = process.value("process", "");
        openDetails(state.window, state.distro, process);
    }
    else if (id == OpenLocation && actionable)
    {
        auto path = text(actionable->data, "path");
        if (!path.empty() && path.front() == L'/' && path.find(L" (deleted)") == std::wstring::npos)
            openLinuxPath(state.window, state.distro, path);
    }
    else if (id == Copy && selected)
    {
        copyText(state.window, state.table.selectedText());
    }
    else if (id == CopyAll)
        copyText(state.window, state.table.exportText());
}
void contextMenu(SearchWindow &state, LPARAM position)
{
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    if (point.x == -1 && point.y == -1)
    {
        RECT item{};
        const int index = state.table.selectedIndex();
        if (index >= 0 && state.table.rowRect(index, item))
            point = {item.left + scale(state.window, 24), item.bottom};
        else
            point = {scale(state.window, 24), scale(state.window, 24)};
        ClientToScreen(state.table.window, &point);
    }
    const Row *row = state.table.selectedActionable();
    const auto path = row ? text(row->data, "path") : L"";
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (row ? 0 : MF_GRAYED), OpenProcess, L"Properties");
    SetMenuDefaultItem(menu, OpenProcess, FALSE);
    AppendMenuW(menu,
                MF_STRING |
                    (!path.empty() && path.front() == L'/' && path.find(L" (deleted)") == std::wstring::npos
                         ? 0
                         : MF_GRAYED),
                OpenLocation, L"Open file location");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (state.table.selected() ? 0 : MF_GRAYED), Copy, L"Copy\tCtrl+C");
    AppendMenuW(menu, MF_STRING, CopyAll, L"Copy all");
    const HWND owner = state.window;
    const auto identity = reinterpret_cast<LONG_PTR>(&state);
    const int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, owner, nullptr);
    DestroyMenu(menu);
    if (id && IsWindow(owner) && GetWindowLongPtrW(owner, GWLP_USERDATA) == identity)
        command(state, id);
}
LRESULT CALLBACK queryProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                           DWORD_PTR context)
{
    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;
    if (message == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE))
    {
        PostMessageW(reinterpret_cast<HWND>(context), WM_COMMAND, wparam == VK_RETURN ? Search : IDCANCEL, 0);
        return 0;
    }
    if (message == WM_CHAR && (wparam == VK_RETURN || wparam == VK_ESCAPE))
        return 0;
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, queryProc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}
LRESULT CALLBACK resultsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                             DWORD_PTR context)
{
    if (message == WM_KEYDOWN)
    {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wparam == VK_RETURN || wparam == VK_ESCAPE || (ctrl && wparam == 'C'))
        {
            int commandId = Copy;
            if (wparam == VK_RETURN)
                commandId = OpenProcess;
            else if (wparam == VK_ESCAPE)
                commandId = Close;
            PostMessageW(reinterpret_cast<HWND>(context), WM_COMMAND, commandId, 0);
            return 0;
        }
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, resultsProc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}
void applyFonts(SearchWindow &state)
{
    HFONT previous = state.uiFont;
    if (HFONT next = WslCreateUiFont(state.window))
        state.uiFont = next;
    for (HWND child : {state.query, state.search, state.cancel, state.status, state.close})
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state.uiFont ? state.uiFont : font), TRUE);
    if (previous && previous != state.uiFont)
        DeleteObject(previous);
}
LRESULT CALLBACK searchProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto *state = reinterpret_cast<SearchWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        auto *incoming = static_cast<std::unique_ptr<SearchWindow> *>(
            reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        state = incoming->release();
        state->window = window;
        state->mailbox->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE:
        // The native search control owns its frame. Creating EDIT with WS_BORDER
        // leaves a second cached inner border after that frame is installed.
        state->query = control(window, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, Query);
        state->caseSensitive = WslHostIntegerSetting(L"SearchControlCaseSensitive") != 0;
        state->regex = WslHostIntegerSetting(L"SearchControlRegex") != 0;
        WslCreateSearch(window, state->query, L"Search handle and mapped file names", queryChanged, state);
        // The native search control's expression buffer holds 255 characters.
        SendMessageW(state->query, EM_SETLIMITTEXT, 255, 0);
        SetWindowSubclass(state->query, queryProc, 1, reinterpret_cast<DWORD_PTR>(window));
        state->search = control(window, WC_BUTTONW, L"Search", WS_TABSTOP | BS_DEFPUSHBUTTON, Search);
        state->cancel = control(window, WC_BUTTONW, L"Cancel", WS_TABSTOP, Cancel);
        state->status = control(window, WC_STATICW, L"Search open handles, executables and mapped files.",
                                SS_LEFT, Status);
        state->close = control(window, WC_BUTTONW, L"Close", WS_TABSTOP, Close);
        state->table.create(window, Results,
                            {{L"Process", 160},
                             {L"PID", 65, true},
                             {L"Type", 120},
                             {L"Handle / address", 135},
                             {L"Name", 480}},
                            0);
        SetWindowSubclass(state->table.window, resultsProc, 1, reinterpret_cast<DWORD_PTR>(window));
        SetWindowLongPtrW(state->table.window, GWL_STYLE,
                          GetWindowLongPtrW(state->table.window, GWL_STYLE) | WS_BORDER);
        applyFonts(*state);
        WslApplyTheme(window);
        setBusy(*state, false);
        layout(*state);
        return 0;
    case WM_SIZE:
        layout(*state);
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {scale(window, 600), scale(window, 340)};
        return 0;
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->uiFont ? state->uiFont : font);
    case WM_DPICHANGED: {
        const RECT *rect = reinterpret_cast<RECT *>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        applyFonts(*state);
        layout(*state);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        HBRUSH brush = CreateSolidBrush(WslDialogBackground());
        FillRect(reinterpret_cast<HDC>(wparam), &rect, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, WslDialogText());
        SetBkColor(dc, WslDialogBackground());
        SetDCBrushColor(dc, WslDialogBackground());
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    case WM_COMMAND:
        command(*state, LOWORD(wparam));
        return 0;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wparam) == state->table.window)
            contextMenu(*state, lparam);
        return 0;
    case WM_NOTIFY: {
        auto *header = reinterpret_cast<NMHDR *>(lparam);
        if (header->hwndFrom != state->table.window)
            break;
        if (header->code == TableDoubleClick)
            command(*state, OpenProcess);
        return 0;
    }
    case ReplyMessage: {
        std::unique_ptr<Reply> reply(reinterpret_cast<Reply *>(lparam));
        if (!reply || reply->tag != state->generation)
            return 0;
        setBusy(*state, false);
        if (!reply->error.empty())
        {
            SetWindowTextW(state->status, wide(reply->error).c_str());
            return 0;
        }
        std::vector<Row> rows;
        for (const auto &item : reply->data.value("results", Json::array()))
        {
            if (!state->match || !WslSearchMatches(state->match, text(item, "path").c_str()))
                continue;
            const auto key = item.at("pid").dump() + ':' + item.at("start_ticks").dump() + ':' +
                             item.value("type", "") + ':' + item.value("handle", "") + ':' +
                             item.value("path", "");
            rows.push_back({{text(item, "process"), text(item, "pid"), text(item, "type"),
                             text(item, "handle"), text(item, "path")},
                            item,
                            key});
        }
        state->table.replace(std::move(rows));
        std::wstring status = std::to_wstring(state->table.rows.size()) + L" results in " +
                              text(reply->data, "processes_scanned") + L" processes";
        if (reply->data.value("inaccessible_processes", 0))
            status += L"; " + text(reply->data, "inaccessible_processes") + L" inaccessible";
        if (reply->data.value("truncated", false))
            status += L". Search incomplete: time or candidate limit reached.";
        SetWindowTextW(state->status, status.c_str());
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->mailbox->detach();
        drainReplies(window);
        state->table.saveLayout();
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (state->uiFont)
            DeleteObject(state->uiFont);
        delete state;
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

void openHandleSearch(HWND owner, const std::wstring &distro)
{
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance;
    cls.lpfnWndProc = searchProc;
    cls.lpszClassName = SearchClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        errorBox(owner, L"Could not register the WSL handle search window.");
        return;
    }
    auto state = std::make_unique<SearchWindow>();
    state->distro = distro;
    const auto title = L"Find handles or mapped files (" + distro + L")";
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, SearchClass, title.c_str(),
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  scale(owner, 1000), scale(owner, 560), owner, nullptr, instance, &state);
    if (!window)
    {
        errorBox(owner, L"Could not create the WSL handle search window.");
        return;
    }
    WslPositionDialog(window, owner);
    ShowWindow(window, SW_SHOW);
    SetFocus(GetDlgItem(window, Query));
}
} // namespace wsl
