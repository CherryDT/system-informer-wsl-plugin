#include "common.hpp"
#include "settings.hpp"
#include <algorithm>
#include <array>
#include <cwctype>
#include <unordered_map>

namespace wsl
{
namespace
{
constexpr wchar_t InspectorClass[] = L"WslTools.Inspector";
constexpr int ConnectionsPage = 5;
constexpr size_t ConnectionsTable = ConnectionsPage - 1;
constexpr int StacksPage = ConnectionsPage + 1;
constexpr int RawPage = StacksPage + 1;
constexpr int RuntimeStacksPage = RawPage + 1;
constexpr int MemoryPage = RuntimeStacksPage + 1;
constexpr size_t MemoryTable = ConnectionsTable + 1;
constexpr size_t TableCount = MemoryTable + 1;
// Keep model indices stable while presenting the available native property pages
// in their familiar order. Linux-only views follow the shared Windows pages.
constexpr std::array<int, 9> ProcessPages = {0, 4, 2, MemoryPage, 3, 1, ConnectionsPage, StacksPage, RawPage};
constexpr wchar_t OverviewClass[] = L"WslTools.Overview";
constexpr int OverviewValueBase = 1000;
enum class Operation
{
    ProcessDetails,
    ServiceDetails,
    Connections,
    Stacks,
    RuntimeStacks
};
enum ControlId
{
    Banner = 201,
    Tabs,
    Overview,
    Files,
    Modules,
    Environment,
    Threads,
    Connections,
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
    ClearFilter,
    RawDetails,
    StackText,
    CaptureStack,
    RuntimeStackText,
    Memory
};

struct OverviewField
{
    const char *key;
    const wchar_t *label;
    bool multiline = false;
    int visibleLines = 3;
    HWND caption = nullptr;
    HWND value = nullptr;
};

// An inspector owns its controls and the mailbox, but never the controller's
// worker. Closing a window therefore never waits for a slow WSL command.
struct Inspector
{
    HWND window = nullptr;
    HWND tabs = nullptr, overview = nullptr, raw = nullptr, status = nullptr;
    HWND tooltips = nullptr;
    std::wstring tooltipText;
    bool statusVisible = false;
    HWND lastOverviewEdit = nullptr;
    HWND stacks = nullptr, runtimeStacks = nullptr, captureStack = nullptr;
    HFONT uiFont = nullptr, rawFont = nullptr;
    int overviewScroll = 0;
    bool overviewLayoutActive = false;
    std::vector<OverviewField> overviewFields;
    Json overviewData;
    HWND refresh = nullptr, copy = nullptr, copyAll = nullptr, save = nullptr;
    HWND open = nullptr, path = nullptr, value = nullptr;
    HWND filterLabel = nullptr, filter = nullptr, clearFilter = nullptr;
    std::array<Table, TableCount> tables;
    std::array<std::vector<Row>, TableCount> snapshots;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    std::wstring distro;
    Json process;
    std::string service;
    std::string runtime;
    std::vector<int> pages;
    bool isService = false;
    bool loading = false;
    bool hasData = false;
    bool connectionsAttempted = false;
    bool connectionsLoaded = false;
    bool stacksLoaded = false;
    bool runtimeStacksLoaded = false;
    std::wstring nativeCapture, runtimeCapture;
    bool nodeChoiceOffered = false;
    Json runtimeRequest;
    Operation pending = Operation::ProcessDetails;
    uintptr_t requestTag = 0;
    int page = 0;
    std::wstring notice;
    std::wstring connectionsNotice;
    std::wstring stacksNotice;
    std::wstring runtimeStacksNotice;
    std::array<bool, TableCount> available{};
    std::array<bool, TableCount> snapshotReady{};
    std::array<bool, TableCount> snapshotComplete{};
    std::array<std::wstring, TableCount> tableNotices;
};

COLORREF inspectorBackground()
{
    return WslDialogBackground();
}

COLORREF pageBackground()
{
    return WslIsDarkTheme() ? WslDialogBackground() : RGB(255, 255, 255);
}

int pageFromTab(const Inspector &state, int index)
{
    if (state.isService)
        return index == 1 ? 1 : 0;
    return index >= 0 && static_cast<size_t>(index) < state.pages.size() ? state.pages[index] : 0;
}

int tabFromPage(const Inspector &state, int page)
{
    if (state.isService)
        return page == 1 ? 1 : 0;
    auto position = std::find(state.pages.begin(), state.pages.end(), page);
    return position == state.pages.end() ? 0 : static_cast<int>(position - state.pages.begin());
}

const wchar_t *runtimeLabel(const Inspector &state)
{
    if (state.runtime == "node")
        return L"JS";
    if (state.runtime == "python")
        return L"Python";
    return L"Java";
}

std::wstring runtimeStackIntro(const Inspector &state)
{
    std::wstring intro = std::wstring(L"Press \"Capture ") + runtimeLabel(state) +
                         L" stacks…\" to collect a stack trace.\r\n\r\n";
    if (state.runtime == "java")
        return intro +
               L"The JVM may briefly pause threads at a safepoint.\r\nRequires jcmd from a matching JDK.";
    if (state.runtime == "python")
        return intro + L"The process briefly pauses during capture.\r\nRequires py-spy in this distribution.";
    return intro + L"The process briefly pauses during capture.\r\nInspector capture requires Python 3; the "
                   L"fallback requires LLDB + llnode.";
}

const wchar_t *nativeStackIntro()
{
    return L"Press \"Capture all stacks…\" to collect native thread stacks.\r\n\r\n"
           L"The process pauses while GDB is attached.\r\nRequires GDB in this distribution.";
}

const wchar_t *runtimeExportName(const Inspector &state)
{
    if (state.runtime == "node")
        return L"wsl-js-stacks.txt";
    if (state.runtime == "python")
        return L"wsl-python-stacks.txt";
    return L"wsl-java-stacks.txt";
}

HWND activeTextControl(const Inspector &state)
{
    if (!state.isService && state.page == RuntimeStacksPage)
        return state.runtimeStacks;
    if (!state.isService && state.page == StacksPage)
        return state.stacks;
    return state.raw;
}

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

void layoutOverview(Inspector &state)
{
    if (state.overviewLayoutActive)
        return;
    RECT bounds{};
    GetClientRect(state.overview, &bounds);
    // Controls start at 0×0 during creation. Updating their scrollbar before
    // the first real layout can synchronously reenter WM_SIZE; so can changing
    // scrollbar visibility during a later resize.
    if (bounds.right <= 0 || bounds.bottom <= 0)
        return;
    state.overviewLayoutActive = true;
    int fieldHeight = editHeight(state.window);
    int rowGap = scale(state.window, 8);
    auto rowHeight = [&](const OverviewField &field) {
        int textHeight = fieldHeight - scale(state.window, 6);
        // Keep whole text lines inside the thin border; the extra six pixels
        // cover its edges and the edit control's vertical inset at this DPI.
        return field.multiline ? field.visibleLines * textHeight + scale(state.window, 6) + rowGap
                               : fieldHeight + rowGap;
    };
    int margin = scale(state.window, 12);
    int captionWidth = scale(state.window, 150);
    int total = margin;
    for (const auto &field : state.overviewFields)
        total += rowHeight(field);
    total += margin;
    state.overviewScroll =
        std::clamp(state.overviewScroll, 0, std::max(0, total - static_cast<int>(bounds.bottom)));
    SCROLLINFO scroll{sizeof(scroll)};
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = std::max(0, total - 1);
    scroll.nPage = static_cast<UINT>(bounds.bottom);
    scroll.nPos = state.overviewScroll;
    SetScrollInfo(state.overview, SB_VERT, &scroll, TRUE);
    // GetClientRect again because showing the scrollbar can reduce the width.
    GetClientRect(state.overview, &bounds);
    int y = margin - state.overviewScroll;
    for (const auto &field : state.overviewFields)
    {
        int height = rowHeight(field);
        int captionHeight = fieldHeight - scale(state.window, 6);
        place(field.caption, margin, y + (fieldHeight - captionHeight) / 2, captionWidth - margin,
              captionHeight);
        place(field.value, margin + captionWidth, y, bounds.right - captionWidth - 2 * margin,
              field.multiline ? height - rowGap : fieldHeight);
        y += height;
    }
    state.overviewLayoutActive = false;
}

LRESULT CALLBACK overviewProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto state = reinterpret_cast<Inspector *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!state)
        return DefWindowProcW(window, message, wparam, lparam);
    switch (message)
    {
    case WM_SIZE:
        layoutOverview(*state);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO scroll{sizeof(scroll)};
        scroll.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &scroll);
        int position = state->overviewScroll;
        switch (LOWORD(wparam))
        {
        case SB_LINEUP:
            position -= scale(state->window, 32);
            break;
        case SB_LINEDOWN:
            position += scale(state->window, 32);
            break;
        case SB_PAGEUP:
            position -= static_cast<int>(scroll.nPage);
            break;
        case SB_PAGEDOWN:
            position += static_cast<int>(scroll.nPage);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            position = scroll.nTrackPos;
            break;
        case SB_TOP:
            position = scroll.nMin;
            break;
        case SB_BOTTOM:
            position = scroll.nMax;
            break;
        }
        state->overviewScroll = position;
        layoutOverview(*state);
        return 0;
    }
    case WM_MOUSEWHEEL:
        state->overviewScroll -= static_cast<short>(HIWORD(wparam)) * scale(state->window, 96) / WHEEL_DELTA;
        layoutOverview(*state);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wparam) == EN_SETFOCUS)
        {
            state->lastOverviewEdit = reinterpret_cast<HWND>(lparam);
            RECT field{}, bounds{};
            GetWindowRect(state->lastOverviewEdit, &field);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&field), 2);
            GetClientRect(window, &bounds);
            if (field.top < 0)
                state->overviewScroll += field.top - scale(state->window, 6);
            else if (field.bottom > bounds.bottom)
                state->overviewScroll += field.bottom - bounds.bottom + scale(state->window, 6);
            layoutOverview(*state);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return SendMessageW(state->window, message, wparam, lparam);
    case WM_ERASEBKGND: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        HBRUSH brush = CreateSolidBrush(pageBackground());
        FillRect(reinterpret_cast<HDC>(wparam), &bounds, brush);
        DeleteObject(brush);
        return 1;
    }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::wstring overviewValue(const Inspector &state, const OverviewField &field)
{
    std::wstring value = cell(state.overviewData, field.key);
    if (value.empty())
    {
        if (state.isService && (std::string(field.key) == "user" || std::string(field.key) == "group"))
            return L"Systemd default";
        return L"Not available";
    }
    std::string key = field.key;
    if (key == "rss_bytes" || key == "virtual_bytes" || key == "read_bytes" || key == "write_bytes" ||
        key == "memory_current")
    {
        if (value.find_first_not_of(L"0123456789") == std::wstring::npos)
        {
            try
            {
                value = bytes(std::stoull(value)) + L" (" + value + L" bytes)";
            }
            catch (const std::exception &)
            { /* Keep values outside the numeric range verbatim. */
            }
        }
    }
    if (!state.isService && key == "state")
    {
        const std::pair<const wchar_t *, const wchar_t *> states[] = {
            {L"R", L"Running"}, {L"S", L"Sleeping"},     {L"D", L"Uninterruptible sleep"},
            {L"T", L"Stopped"}, {L"t", L"Tracing stop"}, {L"Z", L"Zombie"},
            {L"I", L"Idle"}};
        for (const auto &entry : states)
            if (value == entry.first)
                return std::wstring(entry.second) + L" (" + value + L")";
    }
    return editText(value);
}

void updateOverview(Inspector &state, const Json &data)
{
    auto overview = data.find("overview");
    if (overview != data.end() && overview->is_object())
        state.overviewData.update(*overview);
    for (const auto &field : state.overviewFields)
    {
        std::wstring value = overviewValue(state, field);
        SetWindowTextW(field.value, value.c_str());
    }
    layoutOverview(state);
}

void createOverviewFields(Inspector &state)
{
    if (state.isService)
    {
        state.overviewData = {{"name", state.service}};
        state.overviewFields = {{"name", L"Unit"},
                                {"description", L"Description", true},
                                {"active", L"Active state"},
                                {"sub", L"Substate"},
                                {"load", L"Load state"},
                                {"enabled", L"Unit file state"},
                                {"main_pid", L"Main PID"},
                                {"user", L"User"},
                                {"group", L"Group"},
                                {"exec_start", L"Start command", true},
                                {"fragment_path", L"Unit file"},
                                {"active_since", L"Active since"},
                                {"restarts", L"Restarts"},
                                {"result", L"Last result"},
                                {"memory_current", L"Current memory"},
                                {"tasks_current", L"Current tasks"}};
    }
    else
    {
        state.overviewData = state.process.is_object() ? state.process : Json::object();
        state.overviewFields = {{"name", L"Name"},
                                {"pid", L"Process ID"},
                                {"ppid", L"Parent process ID"},
                                {"user", L"User"},
                                {"uid", L"User ID"},
                                {"state", L"State"},
                                {"threads", L"Threads"},
                                {"exe", L"Executable"},
                                {"command", L"Command line", true},
                                {"cwd", L"Working directory"},
                                {"rss_bytes", L"Resident memory"},
                                {"virtual_bytes", L"Virtual memory"},
                                {"read_bytes", L"Storage reads"},
                                {"write_bytes", L"Storage writes"},
                                {"start_ticks", L"Start time (ticks)"},
                                {"cgroup", L"Control groups", true},
                                {"seccomp", L"Seccomp"},
                                {"no_new_privs", L"No new privileges"},
                                {"capabilities", L"Capability masks", true, 6}};
    }
    for (size_t i = 0; i < state.overviewFields.size(); ++i)
    {
        auto &field = state.overviewFields[i];
        field.caption = control(state.overview, WC_STATICW, field.label, SS_LEFT | SS_NOPREFIX, 0);
        DWORD style = WS_TABSTOP | WS_BORDER | ES_READONLY;
        if (field.multiline)
            style |= ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL;
        else
            style |= ES_AUTOHSCROLL;
        field.value = control(state.overview, WC_EDITW, L"", style, OverviewValueBase + static_cast<int>(i));
    }
    updateOverview(state, Json::object());
}

BOOL CALLBACK applyDialogFont(HWND child, LPARAM context)
{
    auto &state = *reinterpret_cast<Inspector *>(context);
    if (child == state.raw || child == state.stacks || child == state.runtimeStacks)
        return TRUE;
    // The host's configurable grid font belongs only to lists and their
    // headers. Buttons, fields, and tab captions use the normal dialog font.
    for (const auto &table : state.tables)
        if (table.window && (child == table.window || IsChild(table.window, child)))
            return TRUE;
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state.uiFont ? state.uiFont : font), FALSE);
    return TRUE;
}

void updateInspectorFonts(Inspector &state)
{
    HFONT previousUi = state.uiFont;
    if (HFONT next = WslCreateUiFont(state.window))
        state.uiFont = next;
    EnumChildWindows(state.window, applyDialogFont, reinterpret_cast<LPARAM>(&state));
    if (previousUi && previousUi != state.uiFont)
        DeleteObject(previousUi);

    HFONT next = WslCreateTextFont(state.window);
    SendMessageW(state.raw, WM_SETFONT,
                 reinterpret_cast<WPARAM>(next ? next : GetStockObject(ANSI_FIXED_FONT)), TRUE);
    if (state.stacks)
        SendMessageW(state.stacks, WM_SETFONT,
                     reinterpret_cast<WPARAM>(next ? next : GetStockObject(ANSI_FIXED_FONT)), TRUE);
    if (state.runtimeStacks)
        SendMessageW(state.runtimeStacks, WM_SETFONT,
                     reinterpret_cast<WPARAM>(next ? next : GetStockObject(ANSI_FIXED_FONT)), TRUE);
    if (state.rawFont)
        DeleteObject(state.rawFont);
    state.rawFont = next;
}

LRESULT dialogControlColor(HDC dc, COLORREF background)
{
    SetTextColor(dc, WslDialogText());
    SetBkColor(dc, background);
    SetBkMode(dc, TRANSPARENT);
    SetDCBrushColor(dc, background);
    return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
}

LRESULT CALLBACK inspectorColorsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                                     DWORD_PTR)
{
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORDLG)
    {
        HWND tabs = GetDlgItem(window, Tabs);
        bool page = tabs && IsChild(tabs, reinterpret_cast<HWND>(lparam));
        return dialogControlColor(reinterpret_cast<HDC>(wparam),
                                  page ? pageBackground() : inspectorBackground());
    }
    if (message == WM_ERASEBKGND)
    {
        RECT bounds{};
        GetClientRect(window, &bounds);
        HBRUSH brush = CreateSolidBrush(inspectorBackground());
        FillRect(reinterpret_cast<HDC>(wparam), &bounds, brush);
        DeleteObject(brush);
        return 1;
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, inspectorColorsProc, 3);
    return DefSubclassProc(window, message, wparam, lparam);
}

void paintControlBorder(HWND window)
{
    HDC dc = GetWindowDC(window);
    if (!dc)
        return;
    RECT bounds{};
    GetWindowRect(window, &bounds);
    OffsetRect(&bounds, -bounds.left, -bounds.top);
    COLORREF color = RGB(192, 192, 192);
    if (WslIsDarkTheme())
    {
        COLORREF background = pageBackground(), foreground = WslDialogText();
        color = RGB((GetRValue(background) * 3 + GetRValue(foreground)) / 4,
                    (GetGValue(background) * 3 + GetGValue(foreground)) / 4,
                    (GetBValue(background) * 3 + GetBValue(foreground)) / 4);
    }
    HBRUSH brush = CreateSolidBrush(color);
    int thickness = GetSystemMetricsForDpi(SM_CXBORDER, GetDpiForWindow(window));
    for (int i = 0; i < std::max(1, thickness); ++i)
    {
        FrameRect(dc, &bounds, brush);
        InflateRect(&bounds, -1, -1);
    }
    DeleteObject(brush);
    ReleaseDC(window, dc);
}

LRESULT CALLBACK controlBorderProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                                   DWORD_PTR)
{
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, controlBorderProc, 4);
    LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_NCPAINT || message == WM_NCACTIVATE || message == WM_THEMECHANGED)
        paintControlBorder(window);
    return result;
}

void styleControlBorder(HWND window)
{
    // Reserve a native one-pixel frame, then paint it in the subtle dialog
    // border color instead of the default black WS_BORDER or a raised edge.
    SetWindowSubclass(window, controlBorderProc, 4, 0);
    SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) | WS_BORDER);
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

BOOL CALLBACK styleInspectorControl(HWND window, LPARAM)
{
    wchar_t name[32]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (lstrcmpiW(name, WC_EDITW) == 0 || lstrcmpiW(name, WC_LISTVIEWW) == 0)
        styleControlBorder(window);
    return TRUE;
}

// Pages are true children of the native tab. Its clipping keeps themed hover
// painting out of the pages; these messages still belong to the inspector.
LRESULT CALLBACK tabPageProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                             DWORD_PTR context)
{
    HWND inspector = reinterpret_cast<HWND>(context);
    switch (message)
    {
    case WM_NOTIFY:
    case WM_COMMAND:
    case WM_CONTEXTMENU:
        return SendMessageW(inspector, message, wparam, lparam);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return dialogControlColor(reinterpret_cast<HDC>(wparam), pageBackground());
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, tabPageProc, 2);
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

std::string rowKey(const Json &object, const char *key)
{
    return utf8(cell(object, key));
}

Table *activeTable(Inspector &state)
{
    if (!state.isService && state.page == MemoryPage)
        return &state.tables[MemoryTable];
    return !state.isService && state.page > 0 && state.page <= ConnectionsPage ? &state.tables[state.page - 1]
                                                                               : nullptr;
}

std::wstring selectedPath(Inspector &state, bool actionable = false)
{
    if (state.page == 0)
        return cell(state.overviewData, state.isService ? "fragment_path" : "exe");
    if (state.isService || (state.page != 1 && state.page != 2 && state.page != MemoryPage))
        return L"";
    Table *table = activeTable(state);
    if (!table)
        return L"";
    const Row *row = actionable ? table->selectedActionable() : table->selected();
    if (!row)
        return L"";
    auto deleted = row->data.find("deleted");
    if (actionable && deleted != row->data.end() && deleted->is_boolean() && deleted->get<bool>())
        return L"";
    return cell(row->data, state.page == 1 ? "target" : "path");
}

bool canOpen(const std::wstring &path)
{
    // procfs also reports socket:[...], pipe:[...], and anon_inode:[...].
    // These are useful identifiers to copy, but are not filesystem paths.
    const std::wstring deleted = L" (deleted)";
    return !path.empty() && path.front() == L'/' &&
           (path.size() < deleted.size() ||
            path.compare(path.size() - deleted.size(), deleted.size(), deleted) != 0);
}

Operation activeOperation(const Inspector &state)
{
    if (state.isService)
        return Operation::ServiceDetails;
    if (state.page == ConnectionsPage)
        return Operation::Connections;
    if (state.page == StacksPage)
        return Operation::Stacks;
    if (state.page == RuntimeStacksPage)
        return Operation::RuntimeStacks;
    return Operation::ProcessDetails;
}

std::wstring &operationNotice(Inspector &state, Operation operation)
{
    if (operation == Operation::Connections)
        return state.connectionsNotice;
    if (operation == Operation::Stacks)
        return state.stacksNotice;
    if (operation == Operation::RuntimeStacks)
        return state.runtimeStacksNotice;
    return state.notice;
}

void layout(Inspector &state);

void updateActions(Inspector &state)
{
    bool pathTab = !state.isService && (state.page == 1 || state.page == 2 || state.page == MemoryPage);
    bool overviewPage = state.page == 0;
    SetWindowTextW(state.open, overviewPage ? (state.isService ? L"Open &unit file" : L"Open &executable")
                                            : L"&Open location");
    SetWindowTextW(state.path, overviewPage ? L"Copy co&mmand" : L"Copy &path");
    ShowWindow(state.open, pathTab || overviewPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.path, pathTab || overviewPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.value, state.page == 3 ? SW_SHOW : SW_HIDE);
    std::wstring path = selectedPath(state);
    EnableWindow(state.open, !state.loading && canOpen(selectedPath(state, true)));
    EnableWindow(state.path,
                 overviewPage ? !cell(state.overviewData, state.isService ? "exec_start" : "command").empty()
                              : !path.empty());
    EnableWindow(state.value, state.page == 3 && state.tables[2].selected());
    EnableWindow(state.refresh, !state.loading);
    bool stackAction =
        !state.isService && (state.page == 4 || state.page == StacksPage || state.page == RuntimeStacksPage);
    ShowWindow(state.captureStack, stackAction ? SW_SHOW : SW_HIDE);
    bool removedThread =
        state.page == 4 && state.tables[3].selected() && !state.tables[3].selectedActionable();
    EnableWindow(state.captureStack, !state.loading && !removedThread);
    std::wstring captureLabel = state.page == RuntimeStacksPage
                                    ? std::wstring(L"Capture ") + runtimeLabel(state) + L" &stacks…"
                                : state.page == 4 && state.tables[3].selected() ? L"View &thread stack…"
                                                                                : L"Capture all &stacks…";
    SetWindowTextW(state.captureStack, captureLabel.c_str());

    // Each snapshot keeps its own notice. Switching tabs while a request is
    // running must neither relabel an old snapshot nor parse the wrong reply.
    bool networkPage = state.page == ConnectionsPage;
    const auto &notice = operationNotice(state, activeOperation(state));
    std::wstring message = notice;
    if (state.loading && activeOperation(state) == state.pending)
        message = state.pending == Operation::Stacks ? L"Capturing stacks with GDB…"
                  : state.pending == Operation::RuntimeStacks
                      ? std::wstring(L"Capturing ") + runtimeLabel(state) + L" stacks…"
                      : L"Loading from " + state.distro + L"…";
    else if (state.loading && networkPage && !state.connectionsAttempted)
        message = L"Connections will load after the current request finishes…";
    if (message.empty() && activeTable(state))
    {
        size_t index = state.page == MemoryPage ? MemoryTable : static_cast<size_t>(state.page - 1);
        if (!state.available[index])
            message = L"This information is unavailable.";
        else
        {
            const auto &rows = state.tables[index].rows;
            size_t removed = static_cast<size_t>(
                std::count_if(rows.begin(), rows.end(), [](const Row &row) { return row.removed; }));
            size_t count = rows.size() - removed;
            message = std::to_wstring(count) + (count == 1 ? L" entry" : L" entries");
            size_t total = state.snapshots[index].size();
            if (state.snapshotComplete[index] && count < total)
                message += L" shown of " + std::to_wstring(total);
            else if (count == 0 && removed == 0)
                message += L" — no entries reported";
            if (removed)
                message += L" · " + std::to_wstring(removed) + L" recently removed";
        }
        if (!state.tableNotices[index].empty())
            message += L" · " + state.tableNotices[index];
    }
    if (state.status)
    {
        SetWindowTextW(state.status, message.c_str());
        bool visible = !message.empty();
        ShowWindow(state.status, visible ? SW_SHOW : SW_HIDE);
        if (visible != state.statusVisible)
        {
            state.statusVisible = visible;
            layout(state);
        }
    }
}

void refresh(Inspector &state);
void layout(Inspector &state);

void ensureConnections(Inspector &state)
{
    // Defer the first network request if the initial process details are still
    // loading. The completed reply will return here; cached rows stay intact.
    if (state.page == ConnectionsPage && !state.connectionsAttempted && !state.loading)
        refresh(state);
}

void showPage(Inspector &state)
{
    ShowWindow(state.overview, state.page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(state.raw, state.page == (state.isService ? 1 : RawPage) ? SW_SHOW : SW_HIDE);
    ShowWindow(state.stacks, !state.isService && state.page == StacksPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.runtimeStacks, !state.isService && state.page == RuntimeStacksPage ? SW_SHOW : SW_HIDE);
    for (size_t i = 0; i < state.tables.size(); ++i)
        if (state.tables[i].window)
            ShowWindow(state.tables[i].window,
                       state.page == (i == MemoryTable ? MemoryPage : static_cast<int>(i + 1)) ? SW_SHOW
                                                                                               : SW_HIDE);
    if (state.filter)
    {
        int visibility = activeTable(state) ? SW_SHOW : SW_HIDE;
        ShowWindow(state.filterLabel, visibility);
        ShowWindow(state.filter, visibility);
        ShowWindow(state.clearFilter, visibility);
        EnableWindow(state.filter, activeTable(state) != nullptr);
        EnableWindow(state.clearFilter, activeTable(state) != nullptr);
    }
    ensureConnections(state);
    updateActions(state);
    layout(state);
}

void applyFilter(Inspector &state)
{
    std::wstring query = folded(windowText(state.filter));
    for (size_t i = 0; i < state.tables.size(); ++i)
    {
        // An empty view before its first reply is not a process snapshot. It
        // must not make every row in that first reply appear newly created.
        if (!state.tables[i].window || !state.snapshotReady[i])
            continue;
        state.tables[i].replace(
            state.snapshots[i],
            [query](const Row &row) {
                if (query.empty())
                    return true;
                for (const auto &value : row.cells)
                    if (folded(value).find(query) != std::wstring::npos)
                        return true;
                return false;
            },
            state.snapshotComplete[i]);
    }
    updateActions(state);
}

void layout(Inspector &state)
{
    RECT rect{};
    GetClientRect(state.window, &rect);
    int gap = scale(state.window, 12);
    int buttonHeight = scale(state.window, 23);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    int buttonsY = gap;
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
    button(state.open, 130);
    button(state.path, 122);
    x = contextualX;
    button(state.value, 100);
    x = contextualX;
    button(state.captureStack, 175);
    int contentY = buttonsY + buttonHeight + gap;
    RECT body{gap, contentY, width - gap, height - gap};
    // The tab frame never moves when changing pages. Search and status belong
    // inside that frame, like native Threads/Modules/Handles property pages.
    place(state.tabs, body.left, body.top, body.right - body.left, body.bottom - body.top);
    GetClientRect(state.tabs, &body);
    TabCtrl_AdjustRect(state.tabs, FALSE, &body);
    InflateRect(&body, -scale(state.window, 4), -scale(state.window, 4));
    if (activeTable(state))
    {
        int labelWidth = scale(state.window, 43);
        int clearWidth = scale(state.window, 60);
        int fieldHeight = editHeight(state.window);
        int rowHeight = std::max(buttonHeight, fieldHeight);
        int captionHeight = fieldHeight - scale(state.window, 6);
        place(state.filterLabel, body.left, body.top + (rowHeight - captionHeight) / 2, labelWidth,
              captionHeight);
        place(state.filter, body.left + labelWidth, body.top + (rowHeight - fieldHeight) / 2,
              body.right - body.left - labelWidth - clearWidth - scale(state.window, 6), fieldHeight);
        place(state.clearFilter, body.right - clearWidth, body.top + (rowHeight - buttonHeight) / 2,
              clearWidth, buttonHeight);
        body.top += rowHeight + scale(state.window, 6);
    }
    if (state.statusVisible)
    {
        int statusHeight = editHeight(state.window) - scale(state.window, 4);
        place(state.status, body.left, body.bottom - statusHeight, body.right - body.left, statusHeight);
        body.bottom -= statusHeight + scale(state.window, 4);
    }
    place(state.overview, body.left, body.top, body.right - body.left, body.bottom - body.top);
    place(state.raw, body.left, body.top, body.right - body.left, body.bottom - body.top);
    if (state.stacks)
        place(state.stacks, body.left, body.top, body.right - body.left, body.bottom - body.top);
    if (state.runtimeStacks)
        place(state.runtimeStacks, body.left, body.top, body.right - body.left, body.bottom - body.top);
    for (auto &table : state.tables)
        if (table.window)
            place(table.window, body.left, body.top, body.right - body.left, body.bottom - body.top);
}

void showCaptureProgress(HWND pane)
{
    SetWindowTextW(pane, L"Capturing...");
    RedrawWindow(pane, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

void queueRuntimeCapture(Inspector &state, Json request)
{
    // Retain the exact identity for a possible Node backend choice. A later
    // explicit capture builds a fresh automatic request instead of reusing it.
    state.runtimeRequest = request;
    state.loading = true;
    state.pending = Operation::RuntimeStacks;
    state.page = RuntimeStacksPage;
    TabCtrl_SetCurSel(state.tabs, tabFromPage(state, RuntimeStacksPage));
    showPage(state);
    showCaptureProgress(state.runtimeStacks);
    submit(state.distro, std::move(request), state.mailbox, ++state.requestTag);
}

void captureRuntimeStacks(Inspector &state)
{
    if (state.loading || state.isService || state.runtime.empty())
        return;
    Json request = {{"op", "script_stacks"}};
    for (const char *key : {"pid", "start_ticks"})
    {
        auto value = state.process.find(key);
        if (value == state.process.end() || (!value->is_number_integer() && !value->is_string()))
        {
            errorBox(
                state.window,
                L"The process identity is unavailable. Refresh the process list and open a new inspector.");
            return;
        }
        request[key] = *value;
    }
    // The runtime hint only controls presentation. The helper revalidates the
    // executable and process identity before choosing a diagnostic tool.
    bool useInspector = state.runtime == "node" && readSetting(L"UseNodeInspectorWithoutAsking", 0) != 0;
    if (useInspector)
    {
        request["backend"] = "inspector";
        request["enable_inspector"] = true;
    }
    state.nodeChoiceOffered = useInspector;
    queueRuntimeCapture(state, std::move(request));
}

void captureStacks(Inspector &state)
{
    if (state.loading || state.isService)
        return;
    Json request = {{"op", "stacks"}};
    for (const char *key : {"pid", "start_ticks"})
    {
        auto value = state.process.find(key);
        if (value == state.process.end() || (!value->is_number_integer() && !value->is_string()))
        {
            errorBox(
                state.window,
                L"The process identity is unavailable. Refresh the process list and open a new inspector.");
            return;
        }
        request[key] = *value;
    }
    if (state.page == 4)
    {
        const Row *thread = state.tables[3].selectedActionable();
        if (!thread && state.tables[3].selected())
            return;
        if (thread)
        {
            auto tid = thread->data.find("tid");
            if (tid == thread->data.end() || !tid->is_number_integer() || *tid <= 0)
            {
                errorBox(state.window,
                         L"The selected thread identity is unavailable. Refresh the thread list.");
                return;
            }
            request["tid"] = *tid;
        }
    }
    // Selecting a stack tab is passive. Capture buttons and Ctrl+R are the
    // explicit actions that attach; their impact is described in the pane.
    state.loading = true;
    state.pending = Operation::Stacks;
    state.page = StacksPage;
    TabCtrl_SetCurSel(state.tabs, tabFromPage(state, StacksPage));
    showPage(state);
    showCaptureProgress(state.stacks);
    submit(state.distro, std::move(request), state.mailbox, ++state.requestTag);
}

void refresh(Inspector &state)
{
    if (state.loading)
        return;
    Operation operation = activeOperation(state);
    if (operation == Operation::Stacks || operation == Operation::RuntimeStacks)
    {
        if (operation == Operation::RuntimeStacks)
            captureRuntimeStacks(state);
        else
            captureStacks(state);
        return;
    }
    Json request;
    auto &notice = operationNotice(state, operation);
    if (state.isService)
        request = {{"op", "service_details"}, {"name", state.service}};
    else
    {
        request = {{"op", operation == Operation::Connections ? "connections" : "details"}};
        for (const char *key : {"pid", "start_ticks"})
        {
            auto value = state.process.find(key);
            if (value == state.process.end() || (!value->is_number_integer() && !value->is_string()))
            {
                notice = L"This process has no valid identity. Refresh the process list and try again.";
                if (operation != Operation::Connections)
                    SetWindowTextW(state.raw, notice.c_str());
                updateActions(state);
                return;
            }
            request[key] = *value;
        }
    }
    state.loading = true;
    state.pending = operation;
    if (operation == Operation::Connections)
        state.connectionsAttempted = true;
    updateActions(state);
    submit(state.distro, std::move(request), state.mailbox, ++state.requestTag);
}

std::wstring allText(Inspector &state)
{
    if (auto *table = activeTable(state))
        return table->exportText();
    if (state.page != 0)
        return windowText(activeTextControl(state));
    std::wstring result;
    for (const auto &field : state.overviewFields)
        result += std::wstring(field.label) + L":\t" + windowText(field.value) + L"\r\n";
    return result;
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
    HWND edit = state.page == 0 ? state.lastOverviewEdit : activeTextControl(state);
    if (!edit)
        return allText(state);
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    std::wstring result = windowText(edit);
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
    case CaptureStack:
        if (state.page == RuntimeStacksPage)
            captureRuntimeStacks(state);
        else
            captureStacks(state);
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
        saveText(state.window, allText(state),
                 state.isService                   ? L"wsl-service.txt"
                 : state.page == RuntimeStacksPage ? runtimeExportName(state)
                 : state.page == StacksPage        ? L"wsl-stacks.txt"
                                                   : L"wsl-process.txt");
        break;
    case CopyPath: {
        std::wstring value = state.page == 0
                                 ? cell(state.overviewData, state.isService ? "exec_start" : "command")
                                 : selectedPath(state);
        if (!value.empty())
            copyText(state.window, value);
        break;
    }
    case CopyValue:
        if (const Row *row = state.tables[2].selected())
            copyText(state.window, cell(row->data, "value"));
        break;
    case OpenLocation: {
        std::wstring path = selectedPath(state, true);
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
    if (state.page == 1 || state.page == 2 || state.page == MemoryPage)
    {
        std::wstring path = selectedPath(state);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (state.loading || !canOpen(selectedPath(state, true)) ? MF_GRAYED : 0),
                    OpenLocation, L"&Open location\tEnter");
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

void syncRuntimeTab(Inspector &state, const Json &data);

std::wstring mappedAddress(const Json &value, const char *key)
{
    std::wstring address = cell(value, key);
    if (address.empty())
        return address;
    if (address.size() < 16)
        address.insert(0, 16 - address.size(), L'0');
    return L"0x" + address;
}

void loadProcessDetails(Inspector &state, const Json &data)
{
    updateOverview(state, data);
    syncRuntimeTab(state, data);
    std::wstring summary = cell(data, "summary");
    SetWindowTextW(state.raw,
                   summary.empty() ? L"No process summary was returned." : editText(summary).c_str());
    struct Section
    {
        size_t index;
        const char *name;
    };
    const Section sections[] = {
        {0, "files"}, {1, "modules"}, {2, "environment"}, {3, "threads"}, {MemoryTable, "memory"}};
    for (const auto &section : sections)
    {
        size_t index = section.index;
        std::vector<Row> rows;
        auto values = data.find(section.name);
        state.available[index] = values != data.end() && values->is_array();
        auto accessible = data.find(std::string(section.name) + "_accessible");
        if (accessible != data.end() && accessible->is_boolean() && !accessible->get<bool>())
            state.available[index] = false;
        state.tableNotices[index].clear();
        auto truncated = data.find(std::string(section.name) + "_truncated");
        if (truncated != data.end() && truncated->is_boolean() && truncated->get<bool>())
            state.tableNotices[index] = L"Collection limit reached; results are incomplete.";
        if (index == 1)
        {
            auto unverified = data.find("modules_unverified");
            if (unverified != data.end() && unverified->is_number_integer() && *unverified > 0)
            {
                if (!state.tableNotices[index].empty())
                    state.tableNotices[index] += L" ";
                std::wstring note = cell(data, "module_classification_note");
                state.tableNotices[index] +=
                    note.empty() ? L"Some mapped files could not be verified as ELF modules." : note;
            }
        }
        if (state.available[index])
        {
            std::unordered_map<std::string, size_t> nameOccurrences;
            for (const auto &value : *values)
            {
                if (!value.is_object())
                    continue;
                Row row;
                row.data = value;
                auto deleted = value.find("deleted");
                std::wstring deletedText =
                    deleted != value.end() && deleted->is_boolean() && deleted->get<bool>() ? L"Yes" : L"No";
                switch (index)
                {
                case 0:
                    row.cells = {cell(value, "fd"), cell(value, "target"), cell(value, "flags")};
                    row.key = rowKey(value, "fd") + ":" + rowKey(value, "target");
                    break;
                case 1:
                    row.cells = {cell(value, "path"),
                                 mappedAddress(value, "base"),
                                 mappedAddress(value, "end"),
                                 cell(value, "size_bytes"),
                                 cell(value, "device"),
                                 cell(value, "inode"),
                                 deletedText};
                    row.key = rowKey(value, "identity");
                    if (row.key.empty())
                        row.key = rowKey(value, "device") + ":" + rowKey(value, "inode");
                    break;
                case 2:
                    row.cells = {cell(value, "name"), cell(value, "value")};
                    row.key = rowKey(value, "name");
                    row.key += ":" + std::to_string(nameOccurrences[row.key]++);
                    break;
                case 3:
                    row.cells = {cell(value, "tid"), cell(value, "name"), cell(value, "state"),
                                 cell(value, "wchan")};
                    row.key = rowKey(value, "tid");
                    break;
                case MemoryTable: {
                    std::wstring backing = cell(value, "path");
                    row.cells = {mappedAddress(value, "start"),
                                 mappedAddress(value, "end"),
                                 cell(value, "size_bytes"),
                                 cell(value, "permissions"),
                                 mappedAddress(value, "offset"),
                                 backing.empty() ? L"[anonymous]" : backing,
                                 cell(value, "device"),
                                 cell(value, "inode"),
                                 deletedText};
                    // A protection change updates a VMA; a split or a different
                    // backing object at the same address creates a different VMA.
                    row.key = rowKey(value, "start") + ":" + rowKey(value, "end") + ":" +
                              rowKey(value, "device") + ":" + rowKey(value, "inode") + ":" +
                              rowKey(value, "offset");
                    break;
                }
                }
                rows.push_back(std::move(row));
            }
        }
        state.snapshots[index] = std::move(rows);
        state.snapshotReady[index] = state.snapshotReady[index] || state.available[index];
        state.snapshotComplete[index] =
            state.available[index] &&
            !(truncated != data.end() && truncated->is_boolean() && truncated->get<bool>());
        if (index == 1)
        {
            auto unverified = data.find("modules_unverified");
            if (unverified != data.end() && unverified->is_number_integer() && *unverified > 0)
                state.snapshotComplete[index] = false;
        }
    }
}

void loadConnections(Inspector &state, const Json &data)
{
    auto connections = data.find("connections");
    state.available[ConnectionsTable] = connections != data.end() && connections->is_array();
    if (!state.available[ConnectionsTable])
    {
        state.connectionsNotice =
            state.connectionsLoaded ? L"No connection information was returned (displayed data may be stale)."
                                    : L"No connection information was returned.";
        return;
    }
    std::vector<Row> rows;
    for (const auto &connection : *connections)
    {
        if (!connection.is_object())
            continue;
        Row row;
        row.data = connection;
        for (const char *field :
             {"protocol", "local_address", "local_port", "remote_address", "remote_port", "state", "inode"})
            row.cells.push_back(cell(connection, field));
        // Inode is the socket's identity; protocol distinguishes its display
        // entry without using an endpoint that can change during connect().
        row.key = rowKey(connection, "protocol") + ":" + rowKey(connection, "inode");
        rows.push_back(std::move(row));
    }
    state.snapshots[ConnectionsTable] = std::move(rows);
    state.connectionsLoaded = true;
    auto inaccessible = data.find("inaccessible_processes");
    if (inaccessible != data.end() && inaccessible->is_number_integer() && *inaccessible > 0)
        state.connectionsNotice =
            L"Some process descriptors were inaccessible; connection ownership may be incomplete.";
    auto tablesRead = data.find("tables_read");
    if (tablesRead != data.end() && tablesRead->is_number_integer() && *tablesRead == 0)
    {
        state.available[ConnectionsTable] = false;
        state.connectionsNotice = L"Network tables are unavailable in the collector's namespace.";
    }
    auto truncated = data.find("connections_truncated");
    if (truncated != data.end() && truncated->is_boolean() && truncated->get<bool>())
    {
        if (!state.connectionsNotice.empty())
            state.connectionsNotice += L" ";
        state.connectionsNotice += L"Connection results reached the collection limit and are incomplete.";
    }
    state.snapshotReady[ConnectionsTable] =
        state.snapshotReady[ConnectionsTable] || state.available[ConnectionsTable];
    state.snapshotComplete[ConnectionsTable] =
        state.available[ConnectionsTable] &&
        !(truncated != data.end() && truncated->is_boolean() && truncated->get<bool>()) &&
        !(inaccessible != data.end() && inaccessible->is_number_integer() && *inaccessible > 0);
}

void loadStacks(Inspector &state, const Json &data)
{
    std::wstring output = cell(data, "text");
    std::wstring message = cell(data, "message");
    auto available = data.find("available");
    bool supported = available != data.end() && available->is_boolean() && available->get<bool>();
    auto timedOut = data.find("timed_out");
    bool timeout = timedOut != data.end() && timedOut->is_boolean() && timedOut->get<bool>();
    auto exitCode = data.find("exit_code");
    bool failed = exitCode != data.end() && exitCode->is_number_integer() && *exitCode != 0;
    if (!supported || timeout || failed)
    {
        state.stacksNotice = !message.empty() ? message
                             : !supported     ? L"GDB stack capture is unavailable."
                             : timeout ? L"Stack capture timed out; any output shown may be incomplete."
                                       : L"GDB could not complete the capture.";
    }
    else
        state.stacksNotice = message.empty() ? L"Stack capture complete." : message;
    if ((!supported || timeout || failed) && state.stacksLoaded)
    {
        state.stacksNotice += L" The previous capture is still displayed.";
        SetWindowTextW(state.stacks, state.nativeCapture.c_str());
        return;
    }
    if (output.empty())
    {
        if (state.stacksLoaded)
        {
            state.stacksNotice += L" The previous capture is still displayed.";
            SetWindowTextW(state.stacks, state.nativeCapture.c_str());
        }
        else
            SetWindowTextW(state.stacks, editText(state.stacksNotice).c_str());
        return;
    }
    if (timeout || failed)
        output = state.stacksNotice + L"\r\n\r\n" + output;
    SetWindowTextW(state.stacks, editText(output).c_str());
    if (supported && !timeout && !failed)
    {
        state.nativeCapture = editText(output);
        state.stacksLoaded = true;
    }
}

void loadRuntimeStacks(Inspector &state, const Json &data)
{
    std::wstring message = cell(data, "message");
    std::wstring output = cell(data, "text");
    auto supported = data.find("supported");
    auto success = data.find("success");
    bool captured = supported != data.end() && supported->is_boolean() && supported->get<bool>() &&
                    success != data.end() && success->is_boolean() && success->get<bool>();
    if (!captured)
    {
        state.runtimeStacksNotice =
            !message.empty() ? message : L"The runtime stack capture could not be completed.";
        // Keep a successful capture intact when tooling becomes unavailable or
        // attachment fails. Initial failures show the diagnostic in the page.
        if (state.runtimeStacksLoaded)
        {
            state.runtimeStacksNotice += L" The previous capture is still displayed.";
            SetWindowTextW(state.runtimeStacks, state.runtimeCapture.c_str());
        }
        else
        {
            std::wstring diagnostic = state.runtimeStacksNotice;
            if (!output.empty())
                diagnostic += L"\r\n\r\n" + output;
            SetWindowTextW(state.runtimeStacks, editText(diagnostic).c_str());
        }
        return;
    }
    if (output.empty())
    {
        state.runtimeStacksNotice = L"The diagnostic tool returned no stack output.";
        if (state.runtimeStacksLoaded)
        {
            state.runtimeStacksNotice += L" The previous capture is still displayed.";
            SetWindowTextW(state.runtimeStacks, state.runtimeCapture.c_str());
        }
        else
            SetWindowTextW(state.runtimeStacks, state.runtimeStacksNotice.c_str());
        return;
    }
    state.runtimeStacksNotice = message.empty() ? L"Runtime stack capture complete." : message;
    state.runtimeCapture = editText(output);
    SetWindowTextW(state.runtimeStacks, state.runtimeCapture.c_str());
    state.runtimeStacksLoaded = true;
}

bool chooseNodeBackend(Inspector &state, const Json &data)
{
    if (state.nodeChoiceOffered && !data.value("inspector_unavailable", false))
    {
        // An explicit backend must not open the same chooser again if its
        // retry fails. Keep diagnostics in the page and preserve any capture.
        loadRuntimeStacks(state, data);
        return false;
    }
    state.nodeChoiceOffered = true;
    constexpr int EnableInspector = 1001;
    constexpr int UseLlnode = 1002;
    const TASKDIALOG_BUTTON buttons[] = {{EnableInspector, L"Temporarily Enable Inspector"},
                                         {UseLlnode, L"Use llnode\nMay not work with every node version"}};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = state.window;
    config.hInstance = instance;
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"Capture JavaScript stacks";
    const bool unavailable = data.value("inspector_unavailable", false);
    config.pszMainInstruction = unavailable ? L"Inspector requires Python 3" : L"Node Inspector is not enabled for this process";
    config.pszContent = unavailable ? L"Install Python 3 in this distribution, or use llnode." : nullptr;
    config.pszVerificationText = unavailable ? nullptr : L"Don't show again";
    config.cButtons = unavailable ? 1 : static_cast<UINT>(std::size(buttons));
    config.pButtons = unavailable ? buttons + 1 : buttons;
    config.nDefaultButton = IDCANCEL;
    int selected = IDCANCEL;
    const HWND owner = state.window;
    const Inspector *expectedState = &state;
    BOOL remember = FALSE;
    HRESULT result = TaskDialogIndirect(&config, &selected, nullptr, &remember);
    // The task dialog pumps messages. Host shutdown can destroy this inspector
    // while it is open; do not touch its state again after that nested teardown.
    if (!IsWindow(owner) ||
        reinterpret_cast<Inspector *>(GetWindowLongPtrW(owner, GWLP_USERDATA)) != expectedState)
        return true;
    if (FAILED(result) || (selected != EnableInspector && selected != UseLlnode))
    {
        state.runtimeStacksNotice =
            FAILED(result) ? L"Could not open the capture method chooser." : L"Capture canceled.";
        if (state.runtimeStacksLoaded)
        {
            state.runtimeStacksNotice += L" The previous capture is still displayed.";
            SetWindowTextW(state.runtimeStacks, state.runtimeCapture.c_str());
        }
        else
        {
            std::wstring text = state.runtimeStacksNotice + L"\r\n\r\n" + runtimeStackIntro(state);
            SetWindowTextW(state.runtimeStacks, text.c_str());
        }
        return false;
    }
    Json request = state.runtimeRequest;
    if (selected == EnableInspector)
    {
        if (remember)
        {
            try
            {
                writeSetting(L"UseNodeInspectorWithoutAsking", 1);
            }
            catch (const std::exception &error)
            {
                errorBox(state.window, wide(error.what()));
            }
        }
        request["backend"] = "inspector";
        request["enable_inspector"] = true;
    }
    else
    {
        request["backend"] = "llnode";
        request.erase("enable_inspector");
    }
    queueRuntimeCapture(state, std::move(request));
    return true;
}

void loadReply(Inspector &state, const Reply &reply)
{
    if (reply.tag != state.requestTag)
        return;
    const Operation completed = state.pending;
    const bool network = completed == Operation::Connections;
    const bool stacks = completed == Operation::Stacks;
    const bool runtimeStacks = completed == Operation::RuntimeStacks;
    auto &notice = operationNotice(state, completed);
    state.loading = false;
    auto choice = reply.data.find("choice_required");
    if (runtimeStacks && state.runtime == "node" && reply.error.empty() && choice != reply.data.end() &&
        choice->is_boolean() && choice->get<bool>())
    {
        bool queued = chooseNodeBackend(state, reply.data);
        if (!queued)
        {
            ensureConnections(state);
            updateActions(state);
        }
        // A chosen backend owns a new tag and pending operation. This reply
        // must not overwrite that state or process its choice request as output.
        return;
    }
    if (!reply.error.empty())
    {
        // A failed refresh leaves the last successful snapshot available for
        // inspection and copying, with a notice scoped to that snapshot.
        bool hadData = runtimeStacks ? state.runtimeStacksLoaded
                       : stacks      ? state.stacksLoaded
                       : network     ? state.connectionsLoaded
                                     : state.hasData;
        notice = hadData ? L"Refresh failed (displayed data may be stale): " : L"Could not load details: ";
        notice += wide(reply.error);
        if (runtimeStacks && hadData)
            SetWindowTextW(state.runtimeStacks, state.runtimeCapture.c_str());
        else if (stacks && hadData)
            SetWindowTextW(state.stacks, state.nativeCapture.c_str());
        if (!network && !hadData)
            SetWindowTextW(runtimeStacks ? state.runtimeStacks
                           : stacks      ? state.stacks
                                         : state.raw,
                           editText(notice).c_str());
    }
    else
    {
        notice.clear();
        if (runtimeStacks)
            loadRuntimeStacks(state, reply.data);
        else if (stacks)
            loadStacks(state, reply.data);
        else if (network)
            loadConnections(state, reply.data);
        else if (completed == Operation::ServiceDetails)
        {
            updateOverview(state, reply.data);
            std::wstring content = cell(reply.data, "text");
            SetWindowTextW(state.raw, content.empty() ? L"No service details were returned."
                                                      : editText(content).c_str());
            state.hasData = true;
        }
        else
        {
            loadProcessDetails(state, reply.data);
            state.hasData = true;
        }
        if (!state.isService && !stacks && !runtimeStacks)
            applyFilter(state);
    }
    ensureConnections(state);
    updateActions(state);
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
        if ((id >= Files && id <= Connections) || id == Memory)
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
        if (ctrl && wParam == 'A' &&
            (controlId == RawDetails || controlId == StackText || controlId == RuntimeStackText ||
             controlId == Filter || controlId >= OverviewValueBase))
        {
            SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
        if (ctrl && wParam == 'R')
            id = Refresh;
        if (ctrl && wParam == 'S')
            id = Save;
        // Let the filter edit keep its normal text-copy shortcut.
        if (ctrl && wParam == 'C' && controlId != Filter && controlId < OverviewValueBase)
        {
            id = CopySelection;
            HWND tabs = GetDlgItem(inspector, Tabs);
            auto state = reinterpret_cast<Inspector *>(GetWindowLongPtrW(inspector, GWLP_USERDATA));
            int page = tabs && state ? pageFromTab(*state, TabCtrl_GetCurSel(tabs)) : 0;
            if (shift && page == 3)
                id = CopyValue;
            else if (shift && (page == 1 || page == 2 || page == MemoryPage))
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
                    int count = TabCtrl_GetItemCount(tabs);
                    int page = (TabCtrl_GetCurSel(tabs) + direction + count) % count;
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
            if (controlId == Files || controlId == Modules || controlId == Memory)
                id = OpenLocation;
            else if ((controlId >= Refresh && controlId <= CopyValue) || controlId == ClearFilter ||
                     controlId == CaptureStack)
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

void createRuntimeStackControl(Inspector &state)
{
    std::wstring placeholder = runtimeStackIntro(state);
    state.runtimeStacks = control(state.tabs, WC_EDITW, placeholder.c_str(),
                                  WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS |
                                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                  RuntimeStackText);
    SendMessageW(state.runtimeStacks, EM_SETLIMITTEXT, 16 * 1024 * 1024, 0);
    // Initial controls are configured together below. A tab discovered by the
    // details reply must receive those same behaviors when it is added later.
    if (state.status)
    {
        SetWindowSubclass(state.runtimeStacks, shortcutProc, 1, reinterpret_cast<DWORD_PTR>(state.window));
        WslApplyTheme(state.runtimeStacks);
        styleControlBorder(state.runtimeStacks);
        SendMessageW(
            state.runtimeStacks, WM_SETFONT,
            reinterpret_cast<WPARAM>(state.rawFont ? state.rawFont : GetStockObject(ANSI_FIXED_FONT)), TRUE);
    }
}

void syncRuntimeTab(Inspector &state, const Json &data)
{
    auto overview = data.find("overview");
    if (overview == data.end() || !overview->is_object())
        return;
    std::string runtime;
    auto reported = overview->find("runtime");
    if (reported != overview->end() && reported->is_string())
    {
        const std::string &value = reported->get_ref<const std::string &>();
        if (value == "node" || value == "python" || value == "java")
            runtime = value;
    }
    if (runtime == state.runtime)
        return;

    bool hadRuntime = !state.runtime.empty();
    int index = hadRuntime ? tabFromPage(state, RuntimeStacksPage) : static_cast<int>(state.pages.size()) - 1;
    // exec() can change a runtime without changing the PID/start-time identity.
    // A capture from the previous interpreter is not a valid cache for the new one.
    state.runtime = std::move(runtime);
    state.runtimeStacksLoaded = false;
    state.runtimeCapture.clear();
    state.runtimeStacksNotice.clear();
    if (state.runtimeStacks)
    {
        DestroyWindow(state.runtimeStacks);
        state.runtimeStacks = nullptr;
    }
    if (state.runtime.empty())
    {
        TabCtrl_DeleteItem(state.tabs, index);
        auto position = std::find(state.pages.begin(), state.pages.end(), RuntimeStacksPage);
        if (position != state.pages.end())
            state.pages.erase(position);
        if (state.page == RuntimeStacksPage)
            state.page = 0;
    }
    else
    {
        std::wstring title = std::wstring(runtimeLabel(state)) + L" Stacks";
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = title.data();
        if (hadRuntime)
            TabCtrl_SetItem(state.tabs, index, &item);
        else
        {
            state.pages.insert(state.pages.end() - 1, RuntimeStacksPage);
            TabCtrl_InsertItem(state.tabs, index, &item);
        }
        createRuntimeStackControl(state);
    }
    TabCtrl_SetCurSel(state.tabs, tabFromPage(state, state.page));
    showPage(state);
}

std::wstring tooltipText(const Inspector &state, int id)
{
    switch (id)
    {
    case Refresh:
        if (state.page == RuntimeStacksPage && !state.isService)
            return L"Capture runtime stacks (Ctrl+R)";
        return state.page == StacksPage && !state.isService ? L"Capture native thread stacks (Ctrl+R)"
                                                            : L"Refresh (Ctrl+R)";
    case CopySelection:
        return L"Copy the selected text or row (Ctrl+C)";
    case CopyAll:
        return L"Copy all fields or all visible rows";
    case Save:
        return L"Save this view (Ctrl+S)";
    case OpenLocation:
        return state.page == 0 ? (state.isService ? L"Open the service unit file location"
                                                  : L"Open the executable location")
                               : L"Open the selected location (Enter)";
    case CopyPath:
        return state.page == 0 ? L"Copy the command line" : L"Copy the selected path (Ctrl+Shift+C)";
    case CopyValue:
        return L"Copy the selected environment value (Ctrl+Shift+C)";
    case CaptureStack:
        if (state.page == RuntimeStacksPage)
            return L"Capture runtime stacks. The diagnostic tool may briefly pause the process.";
        return L"Capture thread stacks with GDB. The process pauses while GDB is attached.";
    case ClearFilter:
        return L"Clear the filter";
    case Filter:
        return L"Filter all columns in this page (Ctrl+F)";
    case Status:
        return windowText(state.status);
    }
    return L"";
}

void createTooltips(Inspector &state)
{
    state.tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                     WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                                     CW_USEDEFAULT, CW_USEDEFAULT, state.window, nullptr, instance, nullptr);
    if (!state.tooltips)
        return;
    SendMessageW(state.tooltips, TTM_SETMAXTIPWIDTH, 0, scale(state.window, 380));
    for (HWND child : {state.refresh, state.copy, state.copyAll, state.save, state.open, state.path,
                       state.value, state.captureStack, state.clearFilter, state.filter, state.status})
    {
        if (!child)
            continue;
        TTTOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = state.window;
        tool.uId = reinterpret_cast<UINT_PTR>(child);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(state.tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void migrateModuleLayout(Table &table, HWND owner)
{
    if (readSetting(L"ModulesLayoutVersion", 0) >= 2)
        return;
    // The old tab showed mappings. Path/base/end keep their useful widths;
    // permissions and its sort order must not become a mapped-size preference.
    std::vector<int> order(table.columns.size());
    for (size_t i = 0; i < table.columns.size(); ++i)
    {
        order[i] = static_cast<int>(i);
        if (i >= 3)
            ListView_SetColumnWidth(table.window, static_cast<int>(i), scale(owner, table.columns[i].width));
    }
    ListView_SetColumnOrderArray(table.window, static_cast<int>(order.size()), order.data());
    if (table.sortColumn >= 3)
    {
        table.sortColumn = -1;
        table.descending = false;
        HWND header = ListView_GetHeader(table.window);
        for (size_t i = 0; i < table.columns.size(); ++i)
        {
            HDITEMW item{};
            item.mask = HDI_FORMAT;
            Header_GetItem(header, static_cast<int>(i), &item);
            item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            Header_SetItem(header, static_cast<int>(i), &item);
        }
    }
    try
    {
        table.saveLayout();
        writeSetting(L"ModulesLayoutVersion", 2);
    }
    catch (const std::exception &)
    {
        // The corrected in-memory layout remains usable. Without the version
        // marker a later window will retry saving it, rather than losing data.
    }
}

void createControls(Inspector &state)
{
    HWND window = state.window;
    state.pages.assign(ProcessPages.begin(), ProcessPages.end());
    auto runtime = state.process.find("runtime");
    if (!state.isService && runtime != state.process.end() && runtime->is_string())
    {
        std::string detected = runtime->get<std::string>();
        if (detected == "node" || detected == "python" || detected == "java")
        {
            state.runtime = std::move(detected);
            state.pages.insert(state.pages.end() - 1, RuntimeStacksPage);
        }
    }
    state.refresh = control(window, WC_BUTTONW, L"&Refresh", BS_PUSHBUTTON | WS_TABSTOP, Refresh);
    state.copy = control(window, WC_BUTTONW, L"&Copy selection", BS_PUSHBUTTON | WS_TABSTOP, CopySelection);
    state.copyAll = control(window, WC_BUTTONW, L"Copy &all", BS_PUSHBUTTON | WS_TABSTOP, CopyAll);
    state.save = control(window, WC_BUTTONW, L"&Save…", BS_PUSHBUTTON | WS_TABSTOP, Save);
    state.open = control(window, WC_BUTTONW, L"&Open location", BS_PUSHBUTTON | WS_TABSTOP, OpenLocation);
    state.path = control(window, WC_BUTTONW, L"Copy &path", BS_PUSHBUTTON | WS_TABSTOP, CopyPath);
    state.value = control(window, WC_BUTTONW, L"Copy &value", BS_PUSHBUTTON | WS_TABSTOP, CopyValue);
    state.captureStack =
        control(window, WC_BUTTONW, L"Capture all &stacks…", BS_PUSHBUTTON | WS_TABSTOP, CaptureStack);
    state.tabs = control(window, WC_TABCONTROLW, L"Detail categories",
                         WS_TABSTOP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, Tabs);
    SetWindowLongPtrW(state.tabs, GWL_EXSTYLE,
                      GetWindowLongPtrW(state.tabs, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
    SetWindowSubclass(state.tabs, tabPageProc, 2, reinterpret_cast<DWORD_PTR>(window));
    if (!state.isService)
    {
        state.filterLabel = control(state.tabs, WC_STATICW, L"Filter:", SS_LEFT, FilterLabel);
        state.filter = control(state.tabs, WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, Filter);
        SendMessageW(state.filter, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"Search all columns in this view (Ctrl+F)"));
        state.clearFilter =
            control(state.tabs, WC_BUTTONW, L"Clear", BS_PUSHBUTTON | WS_TABSTOP, ClearFilter);
    }
    std::vector<std::wstring> names =
        state.isService
            ? std::vector<std::wstring>{L"General", L"Details"}
            : std::vector<std::wstring>{L"General", L"Threads", L"Modules", L"Memory", L"Environment",
                                        L"Handles", L"Network", L"Stacks",  L"Details"};
    if (!state.runtime.empty())
        names.insert(names.end() - 1, std::wstring(runtimeLabel(state)) + L" Stacks");
    for (int i = 0; i < static_cast<int>(names.size()); ++i)
    {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = names[i].data();
        TabCtrl_InsertItem(state.tabs, i, &item);
    }
    state.overview = control(state.tabs, OverviewClass, L"Overview properties",
                             WS_VSCROLL | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, Overview);
    SetWindowLongPtrW(state.overview, GWL_EXSTYLE,
                      GetWindowLongPtrW(state.overview, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
    SetWindowLongPtrW(state.overview, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    createOverviewFields(state);
    state.raw = control(state.tabs, WC_EDITW, L"Loading…",
                        WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS | ES_MULTILINE |
                            ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                        RawDetails);
    SendMessageW(state.raw, EM_SETLIMITTEXT, 16 * 1024 * 1024, 0);
    if (!state.isService)
    {
        state.stacks = control(state.tabs, WC_EDITW, nativeStackIntro(),
                               WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS |
                                   ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                               StackText);
        SendMessageW(state.stacks, EM_SETLIMITTEXT, 16 * 1024 * 1024, 0);
        if (!state.runtime.empty())
            createRuntimeStackControl(state);
        state.tables[0].create(state.tabs, Files, {{L"FD", 65, true}, {L"Target", 520}, {L"Flags", 170}});
        state.tables[1].create(state.tabs, Modules,
                               {{L"Module path", 350},
                                {L"Base address", 145},
                                {L"End address", 145},
                                {L"Mapped bytes", 110, true},
                                {L"Device", 85},
                                {L"Inode", 105, true},
                                {L"Deleted", 65}});
        migrateModuleLayout(state.tables[1], state.window);
        state.tables[MemoryTable].create(state.tabs, Memory,
                                         {{L"Start address", 145},
                                          {L"End address", 145},
                                          {L"Size (bytes)", 110, true},
                                          {L"Protection", 85},
                                          {L"File offset", 145},
                                          {L"Backing", 350},
                                          {L"Device", 85},
                                          {L"Inode", 105, true},
                                          {L"Deleted", 65}});
        state.tables[2].create(state.tabs, Environment, {{L"Variable", 230}, {L"Value", 600}});
        state.tables[3].create(state.tabs, Threads,
                               {{L"TID", 85, true}, {L"Name", 210}, {L"State", 100}, {L"Wait channel", 390}});
        state.tables[ConnectionsTable].create(state.tabs, Connections,
                                              {{L"Protocol", 85},
                                               {L"Local address", 195},
                                               {L"Local port", 90, true},
                                               {L"Remote address", 195},
                                               {L"Remote port", 95, true},
                                               {L"State", 125},
                                               {L"Socket inode", 130, true}});
        state.tables[0].kind = Table::Kind::Handles;
        state.tables[1].kind = Table::Kind::Modules;
        state.tables[MemoryTable].kind = Table::Kind::Memory;
        state.tables[3].kind = Table::Kind::Threads;
        state.tables[ConnectionsTable].kind = Table::Kind::Network;
        const wchar_t *labels[] = {L"Open file descriptors",       L"Loaded ELF modules",
                                   L"Environment variables",       L"Process threads",
                                   L"Process network connections", L"Virtual memory mappings"};
        for (size_t i = 0; i < state.tables.size(); ++i)
            SetWindowTextW(state.tables[i].window, labels[i]);
    }
    state.status = control(state.tabs, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, Status);
    EnumChildWindows(window, installShortcuts, reinterpret_cast<LPARAM>(window));
    WslApplyTheme(window);
    SetWindowSubclass(window, inspectorColorsProc, 3, 0);
    EnumChildWindows(window, styleInspectorControl, 0);
    createTooltips(state);
    updateInspectorFonts(state);
    showPage(state);
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
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->uiFont ? state->uiFont : font);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return dialogControlColor(
            reinterpret_cast<HDC>(wParam),
            IsChild(state->tabs, reinterpret_cast<HWND>(lParam)) ? pageBackground() : inspectorBackground());
    case WM_ERASEBKGND: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        HBRUSH brush = CreateSolidBrush(inspectorBackground());
        FillRect(reinterpret_cast<HDC>(wParam), &bounds, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_CREATE:
        createControls(*state);
        return 0;
    case WM_SIZE:
        layout(*state);
        return 0;
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
        info->ptMinTrackSize = {scale(window, 780), scale(window, 460)};
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT *rect = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        updateInspectorFonts(*state);
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
        if (hdr->hwndFrom == state->tooltips && hdr->code == TTN_GETDISPINFOW)
        {
            auto info = reinterpret_cast<NMTTDISPINFOW *>(lParam);
            state->tooltipText = tooltipText(*state, GetDlgCtrlID(reinterpret_cast<HWND>(hdr->idFrom)));
            info->lpszText = state->tooltipText.data();
            return 0;
        }
        if (hdr->hwndFrom == state->tabs && hdr->code == TCN_SELCHANGE)
        {
            state->page = pageFromTab(*state, TabCtrl_GetCurSel(state->tabs));
            showPage(*state);
            return 0;
        }
        for (auto &table : state->tables)
        {
            if (hdr->hwndFrom != table.window)
                continue;
            if (hdr->code == NM_CUSTOMDRAW)
                return table.customDraw(reinterpret_cast<NMLVCUSTOMDRAW *>(hdr));
            table.notify(hdr);
            if (hdr->code == NM_DBLCLK && (state->page == 1 || state->page == 2 || state->page == MemoryPage))
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
        if (state->tooltips)
            DestroyWindow(state->tooltips);
        for (auto &table : state->tables)
            if (table.window)
                table.saveLayout();
        state->mailbox->detach();
        drainReplies(window);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (state->rawFont)
            DeleteObject(state->rawFont);
        if (state->uiFont)
            DeleteObject(state->uiFont);
        delete state;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void openInspector(HWND owner, std::unique_ptr<Inspector> state)
{
    WNDCLASSEXW panel{sizeof(panel)};
    panel.hInstance = instance;
    panel.lpfnWndProc = overviewProc;
    panel.lpszClassName = OverviewClass;
    panel.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&panel) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        errorBox(owner, L"Could not register the overview panel.");
        return;
    }
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance;
    cls.lpfnWndProc = inspectorProc;
    cls.lpszClassName = InspectorClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = nullptr;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        errorBox(owner, L"Could not register the WSL inspector window.");
        return;
    }
    std::wstring title = state->isService ? wide(state->service) : cell(state->process, "name");
    if (state->isService)
        title += L" — " + state->distro + L" service";
    else
        title += L" (" + cell(state->process, "pid") + L" @ " + state->distro + L")";
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
    SetFocus(raw->overviewFields.empty() ? raw->tabs : raw->overviewFields.front().value);
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
