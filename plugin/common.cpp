#include "common.hpp"
#include "settings.hpp"
#include "host_bridge.h"
#include <unordered_map>
#include <algorithm>
#include <commdlg.h>
#include <iomanip>
#include <sstream>

namespace wsl
{
HINSTANCE instance = nullptr;
HFONT font = nullptr;

int scale(HWND window, int value)
{
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}
int editHeight(HWND window)
{
    HDC dc = GetDC(window);
    HFONT windowFont = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    auto previous = SelectObject(dc, windowFont ? windowFont : font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    ReleaseDC(window, dc);
    return metrics.tmHeight + scale(window, 6);
}
void place(HWND child, int x, int y, int width, int height)
{
    MoveWindow(child, x, y, std::max(0, width), std::max(0, height), TRUE);
}
HWND control(HWND parent, const wchar_t *cls, const wchar_t *label, DWORD style, int id)
{
    HWND child = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return child;
}
std::wstring text(const Json &value, const char *key, const std::wstring &fallback)
{
    auto it = value.find(key);
    if (it == value.end() || it->is_null())
        return fallback;
    if (it->is_string())
        return wide(it->get<std::string>());
    return wide(it->dump());
}
std::wstring number(double value, int decimals)
{
    std::wostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
}
std::wstring bytes(uint64_t value)
{
    const wchar_t *units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double amount = static_cast<double>(value);
    size_t unit = 0;
    while (amount >= 1024 && unit < 4)
    {
        amount /= 1024;
        ++unit;
    }
    return number(amount, unit ? 1 : 0) + L" " + units[unit];
}
void errorBox(HWND owner, const std::wstring &message)
{
    MessageBoxW(owner, message.c_str(), L"WSL Tools", MB_OK | MB_ICONERROR);
}
void copyText(HWND owner, const std::wstring &value)
{
    if (!OpenClipboard(owner))
    {
        errorBox(owner, L"The clipboard is busy. Please try again.");
        return;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (value.size() + 1) * sizeof(wchar_t));
    if (memory)
    {
        if (void *data = GlobalLock(memory))
        {
            memcpy(data, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
        }
        else
            GlobalFree(memory);
    }
    CloseClipboard();
}
void saveText(HWND owner, const std::wstring &value, const wchar_t *defaultName)
{
    wchar_t path[32768]{};
    wcsncpy_s(path, defaultName, _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"Text / tab-separated values\0*.txt;*.tsv\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = L"tsv";
    if (!GetSaveFileNameW(&dialog))
        return;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        errorBox(owner, L"Unable to create the export file.");
        return;
    }
    auto encoded = utf8(value);
    DWORD written = 0;
    const bool ok = WriteFile(file, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr) &&
                    written == encoded.size();
    CloseHandle(file);
    if (!ok)
        errorBox(owner, L"The export could not be written completely.");
}
void openLinuxPath(HWND owner, const std::wstring &distro, const std::wstring &path, bool select)
{
    try
    {
        auto target = windowsPath(distro, path);
        // Explorer receives one quoted path, never a command interpreted by a shell.
        std::wstring arguments = select ? L"/select,\"" + target + L"\"" : L"\"" + target + L"\"";
        auto result =
            ShellExecuteW(owner, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
            errorBox(owner, L"Explorer could not open this path.");
    }
    catch (const std::exception &e)
    {
        errorBox(owner, wide(e.what()));
    }
}
void drainReplies(HWND window)
{
    MSG message{};
    while (PeekMessageW(&message, window, ReplyMessage, ReplyMessage, PM_REMOVE))
    {
        // WM_QUIT bypasses PeekMessage's filters. The host posts it while
        // destroying its main window, before our child view is torn down.
        // Preserve it so draining replies cannot keep a windowless host alive.
        if (message.message == WM_QUIT)
        {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        delete reinterpret_cast<Reply *>(message.lParam);
    }
}

namespace
{
constexpr UINT SaveTableLayout = WM_APP + 82;
constexpr UINT_PTR HighlightTimer = 0x57534c;
LRESULT CALLBACK tableLayoutProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                 DWORD_PTR context)
{
    auto table = reinterpret_cast<Table *>(context);
    if (message == SaveTableLayout)
    {
        table->saveLayout();
        return 0;
    }
    if (message == WM_TIMER && wparam == HighlightTimer)
    {
        table->expireHighlights();
        return 0;
    }
    if (message == WM_NOTIFY)
    {
        auto header = reinterpret_cast<NMHDR *>(lparam);
        const auto result = DefSubclassProc(window, message, wparam, lparam);
        if (header->hwndFrom == ListView_GetHeader(window) &&
            (header->code == HDN_ENDTRACKW || header->code == HDN_ENDTRACKA || header->code == HDN_ENDDRAG))
            // The native header applies its final order after notifying us.
            PostMessageW(window, SaveTableLayout, 0, 0);
        return result;
    }
    if (message == WM_DESTROY)
    {
        KillTimer(window, HighlightTimer);
        table->saveLayout();
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, tableLayoutProc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}
} // namespace

void Table::create(HWND parent, int id, std::vector<Column> definitions, int defaultSort,
                   bool defaultDescending)
{
    columns = std::move(definitions);
    settingsPrefix = L"Table." + std::to_wstring(id) + L".";
    auto savedSort = readSetting((settingsPrefix + L"SortColumn").c_str(), static_cast<DWORD>(defaultSort));
    sortColumn = savedSort < columns.size() ? static_cast<int>(savedSort) : -1;
    descending = readSetting((settingsPrefix + L"Descending").c_str(), defaultDescending ? 1 : 0) != 0;
    window = control(parent, WC_LISTVIEWW, L"",
                     WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SINGLESEL, id);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(WslGetHostFont()), TRUE);
    ListView_SetExtendedListViewStyle(window, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                  LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
    std::vector<int> order(columns.size());
    std::vector<bool> seen(columns.size(), false);
    bool validOrder = true;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        auto columnKey = settingsPrefix + L"Column." + std::to_wstring(i);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.pszText = columns[i].title.data();
        column.cx =
            scale(parent, static_cast<int>(std::min(
                              4096ul, readSetting((columnKey + L".Width").c_str(), columns[i].width))));
        column.fmt = columns[i].numeric ? LVCFMT_RIGHT : LVCFMT_LEFT;
        ListView_InsertColumn(window, static_cast<int>(i), &column);
        auto position = readSetting((columnKey + L".Order").c_str(), static_cast<DWORD>(i));
        if (position >= columns.size() || seen[position])
            validOrder = false;
        else
        {
            order[i] = static_cast<int>(position);
            seen[position] = true;
        }
    }
    if (validOrder)
        ListView_SetColumnOrderArray(window, static_cast<int>(order.size()), order.data());
    if (sortColumn >= 0)
    {
        HDITEMW header{};
        header.mask = HDI_FORMAT;
        Header_GetItem(ListView_GetHeader(window), sortColumn, &header);
        header.fmt |= descending ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(ListView_GetHeader(window), sortColumn, &header);
    }
    SetWindowSubclass(window, tableLayoutProc, 1, reinterpret_cast<DWORD_PTR>(this));
    WslApplyTheme(window);
}

void Table::saveLayout() const
{
    if (!window || !IsWindow(window) || settingsPrefix.empty())
        return;
    writeSetting((settingsPrefix + L"SortColumn").c_str(), static_cast<DWORD>(sortColumn));
    writeSetting((settingsPrefix + L"Descending").c_str(), descending ? 1 : 0);
    std::vector<int> order(columns.size());
    if (!ListView_GetColumnOrderArray(window, static_cast<int>(order.size()), order.data()))
        return;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        auto columnKey = settingsPrefix + L"Column." + std::to_wstring(i);
        int width = ListView_GetColumnWidth(window, static_cast<int>(i));
        writeSetting((columnKey + L".Width").c_str(),
                     MulDiv(width, 96, static_cast<int>(GetDpiForWindow(window))));
        writeSetting((columnKey + L".Order").c_str(), static_cast<DWORD>(order[i]));
    }
}

const Row *Table::selected() const
{
    int index = ListView_GetNextItem(window, -1, LVNI_SELECTED);
    return index >= 0 && static_cast<size_t>(index) < rows.size() ? &rows[index] : nullptr;
}
const Row *Table::selectedActionable() const
{
    const auto row = selected();
    return row && !row->removed ? row : nullptr;
}
void Table::selectKey(const std::string &key)
{
    const int current = ListView_GetNextItem(window, -1, LVNI_SELECTED);
    int next = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].key == key)
        {
            next = static_cast<int>(i);
            break;
        }
    if (current == next)
        return;
    if (current >= 0)
        ListView_SetItemState(window, current, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (next >= 0)
        ListView_SetItemState(window, next, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}
void Table::order()
{
    if (ancestryOrder || sortColumn < 0 || static_cast<size_t>(sortColumn) >= columns.size())
        return;
    const auto column = static_cast<size_t>(sortColumn);
    std::stable_sort(rows.begin(), rows.end(), [&](const Row &a, const Row &b) {
        if (column >= a.cells.size() || column >= b.cells.size())
            return false;
        int comparison;
        if (columns[column].numeric)
        {
            const double av = wcstod(a.cells[column].c_str(), nullptr),
                         bv = wcstod(b.cells[column].c_str(), nullptr);
            comparison = av < bv ? -1 : av > bv ? 1 : 0;
        }
        else
            comparison =
                CompareStringOrdinal(a.cells[column].c_str(), -1, b.cells[column].c_str(), -1, TRUE) -
                CSTR_EQUAL;
        if (!comparison)
            comparison = a.key.compare(b.key);
        return descending ? comparison > 0 : comparison < 0;
    });
}
void Table::replace(std::vector<Row> next, std::function<bool(const Row &)> predicate, bool complete)
{
    const auto now = GetTickCount64();
    const auto duration = WslHostIntegerSetting(L"HighlightingDuration");
    std::unordered_map<std::string, size_t> previous;
    for (size_t i = 0; i < source.size(); ++i)
        previous.emplace(source[i].key, i);

    for (auto &row : next)
    {
        row.removed = false;
        row.highlightedSince = 0;
        auto found = previous.find(row.key);
        if (found != previous.end() && !source[found->second].removed)
        {
            const auto started = source[found->second].highlightedSince;
            if (started && now - started < duration)
                row.highlightedSince = started;
        }
        else if (initialized && duration)
            row.highlightedSince = now;
        if (found != previous.end())
            previous.erase(found);
    }
    // Retain removed objects in their last known state. Their identity is never
    // handed to actions, although users can still copy and export these rows.
    for (auto &old : source)
    {
        if (!previous.count(old.key))
            continue;
        if (!complete && !old.removed)
        {
            // A truncated or inaccessible collection says nothing about absent
            // objects. Keep their last known values until collection succeeds.
            if (old.highlightedSince && now - old.highlightedSince >= duration)
                old.highlightedSince = 0;
            next.push_back(std::move(old));
            continue;
        }
        if (!duration)
            continue;
        if (!old.removed)
        {
            old.removed = true;
            old.highlightedSince = now;
        }
        if (now - old.highlightedSince < duration)
            next.push_back(std::move(old));
    }
    initialized = true;
    source = std::move(next);
    filter = std::move(predicate);
    present();
    refreshTimer();
}
void Table::clear()
{
    initialized = false;
    source.clear();
    filter = {};
    present();
    KillTimer(window, HighlightTimer);
}
void Table::refreshTimer()
{
    const bool pending =
        std::any_of(source.begin(), source.end(), [](const Row &row) { return row.highlightedSince != 0; });
    if (pending)
        SetTimer(window, HighlightTimer, 100, nullptr);
    else
        KillTimer(window, HighlightTimer);
}
void Table::expireHighlights()
{
    const auto now = GetTickCount64();
    const auto duration = WslHostIntegerSetting(L"HighlightingDuration");
    bool changed = false;
    source.erase(std::remove_if(source.begin(), source.end(),
                                [&](Row &row) {
                                    if (!row.highlightedSince || now - row.highlightedSince < duration)
                                        return false;
                                    changed = true;
                                    row.highlightedSince = 0;
                                    return row.removed;
                                }),
                 source.end());
    if (changed)
        present();
    refreshTimer();
}
void Table::present()
{
    std::vector<Row> next;
    next.reserve(source.size());
    for (const auto &row : source)
        if (!filter || filter(row))
            next.push_back(row);
    const std::string selectedKey = selected() ? selected()->key : "";
    auto previous = std::move(rows);
    rows = std::move(next);
    order();
    // Owner-data controls retain their items. Refreshing does not delete rows,
    // clear selection, or erase the whole background. Only changed visible
    // rows are invalidated after the model is stable.
    const bool visible = IsWindowVisible(window) != FALSE;
    if (visible)
        SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    if (previous.size() != rows.size())
        ListView_SetItemCountEx(window, static_cast<int>(rows.size()),
                                LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    selectKey(selectedKey);
    if (visible)
        SendMessageW(window, WM_SETREDRAW, TRUE, 0);

    const int top = std::max(0, ListView_GetTopIndex(window));
    const int end = std::min(static_cast<int>(rows.size()), top + ListView_GetCountPerPage(window) + 1);
    for (int i = top; i < end; ++i)
    {
        const auto index = static_cast<size_t>(i);
        if (index >= previous.size() || previous[index].key != rows[index].key ||
            previous[index].cells != rows[index].cells || previous[index].removed != rows[index].removed ||
            previous[index].highlightedSince != rows[index].highlightedSince ||
            previous[index].data != rows[index].data)
            ListView_RedrawItems(window, i, i);
    }
    // Clear a vacated tail once when processes exit; unchanged snapshots never
    // invalidate headers, scrollbars, or the empty part of the table.
    if (previous.size() > rows.size())
        InvalidateRect(window, nullptr, FALSE);
}
void Table::setAncestryOrder(bool enabled)
{
    if (ancestryOrder == enabled)
        return;
    ancestryOrder = enabled;
    HWND header = ListView_GetHeader(window);
    for (size_t i = 0; i < columns.size(); ++i)
    {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        Header_GetItem(header, static_cast<int>(i), &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (!enabled && static_cast<int>(i) == sortColumn)
            item.fmt |= descending ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(header, static_cast<int>(i), &item);
    }
}
void Table::sort(int column)
{
    setAncestryOrder(false);
    std::string key = selected() ? selected()->key : "";
    descending = sortColumn == column ? !descending : columns[column].numeric;
    sortColumn = column;
    order();
    HWND header = ListView_GetHeader(window);
    for (size_t i = 0; i < columns.size(); ++i)
    {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        Header_GetItem(header, static_cast<int>(i), &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (static_cast<int>(i) == column)
            item.fmt |= descending ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(header, static_cast<int>(i), &item);
    }
    selectKey(key);
    saveLayout();
    InvalidateRect(window, nullptr, FALSE);
}
bool Table::notify(NMHDR *hdr)
{
    if (hdr->hwndFrom != window)
        return false;
    if (hdr->code == LVN_GETDISPINFOW)
    {
        auto info = reinterpret_cast<NMLVDISPINFOW *>(hdr);
        size_t row = static_cast<size_t>(info->item.iItem), col = static_cast<size_t>(info->item.iSubItem);
        if ((info->item.mask & LVIF_TEXT) && row < rows.size() && col < rows[row].cells.size())
            info->item.pszText = rows[row].cells[col].data();
        return true;
    }
    if (hdr->code == LVN_COLUMNCLICK)
    {
        sort(reinterpret_cast<NMLISTVIEW *>(hdr)->iSubItem);
        return true;
    }
    return false;
}
LRESULT Table::customDraw(NMLVCUSTOMDRAW *draw) const
{
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
    if (draw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT || draw->nmcd.dwItemSpec >= rows.size())
        return CDRF_DODEFAULT;
    // Some owner-data list-view paint paths report CDIS_SELECTED for ordinary
    // rows. Query the retained selection state instead of suppressing colors
    // based on that notification flag.
    if (ListView_GetItemState(window, static_cast<int>(draw->nmcd.dwItemSpec), LVIS_SELECTED) & LVIS_SELECTED)
        return CDRF_DODEFAULT;

    const auto &row = rows[draw->nmcd.dwItemSpec];
    const auto &item = row.data;
    COLORREF background = CLR_NONE;
    COLORREF foreground = CLR_NONE;
    const auto state = item.value("state", std::string{});
    const auto active = item.value("active", std::string{});
    auto enabled = [](PCWSTR setting) { return WslHostIntegerSetting(setting) != 0; };
    auto color = [](PCWSTR setting) { return static_cast<COLORREF>(WslHostIntegerSetting(setting)); };

    // Temporary lifecycle colors have priority over semantic colors, matching
    // TreeNew's temporary background behavior. All colors come from the host.
    if (row.highlightedSince)
        background = color(row.removed ? L"ColorRemoved" : L"ColorNew");
    else if (kind == Kind::Processes || (kind == Kind::Generic && item.contains("uid")))
    {
        // Keep the native process priority: debugged, suspended, elevated,
        // then service. Do not guess Windows-only properties from Linux names.
        if (item.value("tracer_pid", 0) != 0 && enabled(L"UseColorDebuggedProcesses"))
            background = color(L"ColorDebuggedProcesses");
        else if ((state == "T" || state == "t") && enabled(L"UseColorSuspended"))
            background = color(L"ColorSuspended");
        else if (item.contains("uid") && item["uid"].is_number() && item["uid"] == 0 &&
                 enabled(L"UseColorElevatedProcesses"))
            background = color(L"ColorElevatedProcesses");
        else if (item.value("is_service", false) && enabled(L"UseColorServiceProcesses"))
            background = color(L"ColorServiceProcesses");
    }
    else if (kind == Kind::Services || (kind == Kind::Generic && item.contains("active")))
    {
        const bool stopped = active == "inactive" || active == "failed";
        const auto unitState = item.value("enabled", std::string{});
        if (stopped && (unitState == "disabled" || unitState.find("masked") == 0) &&
            enabled(L"UseColorServiceDisabled"))
            background = color(L"ColorServiceDisabled");
        else if (stopped && enabled(L"UseColorServiceStop"))
            foreground = color(L"ColorServiceStop");
    }
    else if (kind == Kind::Threads || (kind == Kind::Generic && item.contains("tid")))
    {
        if ((state == "T" || state == "t") && enabled(L"UseColorThreadSuspended"))
            background = color(L"ColorThreadSuspended");
    }
    else if (kind == Kind::Memory)
    {
        const auto permissions = item.value("permissions", std::string{});
        const auto path = item.value("path", std::string{});
        if (permissions.find('x') != std::string::npos && enabled(L"UseColorMemoryExecutePages"))
            background = color(L"ColorMemoryExecutePages");
        else if (permissions.find('p') != std::string::npos && (path.empty() || path.front() == '[') &&
                 enabled(L"UseColorMemoryPrivatePages"))
            background = color(L"ColorMemoryPrivatePages");
    }
    else if (kind == Kind::Network || (kind == Kind::Generic && item.contains("protocol")))
    {
        if (item.value("pid", 0) == 0 && enabled(L"UseColorNetworkUnknownProcess"))
            background = color(L"ColorNetworkUnknownProcess");
    }

    if (background != CLR_NONE)
    {
        draw->clrTextBk = background;
        // TreeNew uses this same brightness threshold for user-selected colors.
        const auto brightness =
            (std::min({GetRValue(background), GetGValue(background), GetBValue(background)}) +
             std::max({GetRValue(background), GetGValue(background), GetBValue(background)})) /
            2;
        foreground = brightness > 100 ? RGB(0, 0, 0) : RGB(255, 255, 255);
    }
    if (foreground != CLR_NONE)
        draw->clrText = foreground;
    return CDRF_NEWFONT;
}

std::wstring Table::exportText() const
{
    auto escape = [](std::wstring value) {
        for (auto &c : value)
            if (c == L'\t' || c == L'\r' || c == L'\n')
                c = L' ';
        // Spreadsheet applications must not interpret untrusted process text as a formula.
        if (!value.empty() && wcschr(L"=+-@", value.front()))
            value.insert(value.begin(), L'\'');
        return value;
    };
    std::wstring result;
    for (size_t i = 0; i < columns.size(); ++i)
        result += (i ? L"\t" : L"") + escape(columns[i].title);
    result += L"\r\n";
    for (const auto &row : rows)
    {
        for (size_t i = 0; i < row.cells.size(); ++i)
            result += (i ? L"\t" : L"") + escape(row.cells[i]);
        result += L"\r\n";
    }
    return result;
}
} // namespace wsl
