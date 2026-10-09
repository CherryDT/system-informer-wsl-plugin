#include "common.hpp"
#include "settings.hpp"
#include "host_bridge.h"
#include <unordered_map>
#include <algorithm>
#include <commdlg.h>
#include <iomanip>
#include <sstream>
#include <windowsx.h>
#include <cmath>

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
    // Use Windows' user locale, including custom decimal/grouping separators.
    // The C++ stream remains invariant so GetNumberFormatEx receives its expected input.
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(decimals) << value;
    const auto raw = out.str();
    wchar_t decimal[16]{}, thousands[16]{}, grouping[16]{};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, 16);
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, thousands, 16);
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SGROUPING, grouping, 16);
    NUMBERFMTW format{};
    format.NumDigits = static_cast<UINT>(std::max(0, decimals));
    format.LeadingZero = 1;
    format.lpDecimalSep = decimal;
    format.lpThousandSep = thousands;
    // Windows represents a repeated final group with a trailing zero (3;0 -> 3).
    for (const wchar_t *p = grouping; *p; ++p)
        if (*p >= L'1' && *p <= L'9')
            format.Grouping = format.Grouping * 10 + static_cast<UINT>(*p - L'0');
    if (*grouping && grouping[wcslen(grouping) - 1] != L'0')
        format.Grouping *= 10;
    DWORD negativeOrder = 1;
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_INEGNUMBER | LOCALE_RETURN_NUMBER,
                    reinterpret_cast<LPWSTR>(&negativeOrder), sizeof(negativeOrder) / sizeof(wchar_t));
    format.NegativeOrder = negativeOrder;
    const int length = GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, raw.c_str(), &format, nullptr, 0);
    if (!length)
        return raw;
    std::wstring formatted(static_cast<size_t>(length), L'\0');
    GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, raw.c_str(), &format, formatted.data(), length);
    formatted.resize(static_cast<size_t>(length - 1));
    return formatted;
}
std::wstring bytes(uint64_t value)
{
    const wchar_t *units[] = {L"B", L"kB", L"MB", L"GB", L"TB", L"PB", L"EB"};
    double amount = static_cast<double>(value);
    size_t unit = 0;
    while (amount >= 1024 && unit + 1 < std::size(units))
    {
        amount /= 1024;
        ++unit;
    }
    return number(amount, unit ? 2 : 0) + L" " + units[unit];
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
    if (message == WM_DESTROY)
    {
        KillTimer(window, HighlightTimer);
        table->saveLayout();
    }
    const auto result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_SETFONT || message == WM_THEMECHANGED)
        table->applyFont();
    if (message == WM_NCDESTROY)
    {
        table->detachTree();
        RemoveWindowSubclass(window, tableLayoutProc, id);
    }
    return result;
}
} // namespace

void Table::create(HWND parent, int id, std::vector<Column> definitions, int defaultSort,
                   bool defaultDescending)
{
    columns = std::move(definitions);
    hiddenWidths.resize(columns.size());
    columnOrder.resize(columns.size());
    defaultSortColumn = defaultSort;
    defaultSortDescending = defaultDescending;
    settingsPrefix = L"Table." + std::to_wstring(id) + L".";
    auto savedSort = readSetting((settingsPrefix + L"SortColumn").c_str(), static_cast<DWORD>(defaultSort));
    sortColumn = savedSort < columns.size() ? static_cast<int>(savedSort) : -1;
    descending = readSetting((settingsPrefix + L"Descending").c_str(), defaultDescending ? 1 : 0) != 0;
    WSL_TREE_CALLBACKS callbacks{};
    callbacks.text = [](void *context, int index, int column) -> PCWSTR {
        auto &table = *static_cast<Table *>(context);
        if (index < 0 || static_cast<size_t>(index) >= table.rows.size() || column < 0 ||
            static_cast<size_t>(column) >= table.rows[index].cells.size())
            return L"";
        return table.rows[index].cells[column].c_str();
    };
    callbacks.colors = [](void *context, int index, COLORREF *back, COLORREF *fore) {
        static_cast<Table *>(context)->rowColors(index, *back, *fore);
    };
    callbacks.font = [](void *context, int index) { return static_cast<Table *>(context)->rowFont(index); };
    callbacks.tooltip = [](void *context, int index, int column) -> PCWSTR {
        auto &table = *static_cast<Table *>(context);
        if (!WslHostIntegerSetting(L"EnableTooltipSupport"))
            return L"";
        if (column != 0 || !table.infoTip || index < 0 || static_cast<size_t>(index) >= table.rows.size())
            return nullptr; // Let TreeNew supply its normal clipped-cell tip.
        table.tooltipText = table.infoTip(table.rows[index]);
        return table.tooltipText.c_str();
    };
    callbacks.find = [](void *context, int start, PCWSTR prefix, size_t length) {
        return static_cast<Table *>(context)->findItem(start, prefix, length);
    };
    callbacks.event = [](void *context, int event, int value, int extra, POINT point) {
        static_cast<Table *>(context)->treeEvent(event, value, extra, point);
    };
    tree = WslTreeCreate(parent, id, instance, this, &callbacks);
    window = WslTreeWindow(tree);
    if (!window)
        throw std::runtime_error("Could not create the System Informer grid.");
    SetWindowSubclass(window, tableLayoutProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(WslGetHostFont()), TRUE);
    std::vector<bool> seen(columns.size(), false);
    bool validOrder = true;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        auto key = settingsPrefix + L"Column." + std::to_wstring(i);
        hiddenWidths[i] =
            scale(parent, static_cast<int>(std::clamp(
                              readSetting((key + L".Width").c_str(), columns[i].width), 1ul, 4096ul)));
        columns[i].visible = readSetting((key + L".Visible").c_str(), columns[i].visible ? 1 : 0) != 0;
        auto position = readSetting((key + L".Order").c_str(), static_cast<DWORD>(i));
        if (position >= columns.size() || seen[position])
            validOrder = false;
        else
        {
            columnOrder[i] = static_cast<int>(position);
            seen[position] = true;
        }
    }
    if (!validOrder)
        for (size_t i = 0; i < columns.size(); ++i)
            columnOrder[i] = static_cast<int>(i);
    if (std::none_of(columns.begin(), columns.end(), [](const Column &c) { return c.visible; }) &&
        !columns.empty())
        columns.front().visible = true;
    applyColumns();
    WslApplyTheme(window);
}
void Table::applyFont()
{
    if (tree)
        WslTreeSetFont(tree, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
}
void Table::detachTree()
{
    releaseDrawingResources();
    WslTreeFree(tree);
    tree = nullptr;
    window = nullptr;
}
void Table::applyColumns()
{
    std::vector<WSL_TREE_COLUMN> definitions(columns.size());
    for (size_t position = 0; position < columnOrder.size(); ++position)
    {
        const int id = columnOrder[position];
        definitions[id] = {columns[id].title.c_str(), hiddenWidths[id], columns[id].visible,
                           columns[id].numeric, static_cast<int>(position)};
    }
    WslTreeColumns(tree, definitions.data(), definitions.size());
    setSortIndicator();
    applyFont();
}
void Table::captureColumnOrder()
{
    const auto visible = visibleColumns();
    // Preserve hidden columns' relative positions in the persisted full order.
    size_t next = 0;
    for (auto &id : columnOrder)
        if (isColumnVisible(id) && next < visible.size())
            id = visible[next++];
    for (size_t i = 0; i < columns.size(); ++i)
        if (int width = columnWidth(static_cast<int>(i)); width > 0)
            hiddenWidths[i] = width;
}
void Table::saveLayout() const
{
    if (!window || !IsWindow(window) || settingsPrefix.empty())
        return;
    const_cast<Table *>(this)->captureColumnOrder();
    writeSetting((settingsPrefix + L"SortColumn").c_str(), static_cast<DWORD>(sortColumn));
    writeSetting((settingsPrefix + L"Descending").c_str(), descending ? 1 : 0);
    for (size_t i = 0; i < columns.size(); ++i)
    {
        auto key = settingsPrefix + L"Column." + std::to_wstring(i);
        writeSetting((key + L".Visible").c_str(), isColumnVisible(i) ? 1 : 0);
        writeSetting((key + L".Width").c_str(), MulDiv(hiddenWidths[i], 96, GetDpiForWindow(window)));
        writeSetting((key + L".Order").c_str(), static_cast<DWORD>(columnOrder[i]));
    }
}
bool Table::isColumnVisible(size_t column) const
{
    return column < columns.size() && columnWidth(static_cast<int>(column)) > 0;
}
int Table::columnWidth(int column) const
{
    return tree && column >= 0 && static_cast<size_t>(column) < columns.size()
               ? WslTreeColumnWidth(tree, column)
               : 0;
}
void Table::setColumnWidth(int column, int width)
{
    if (column < 0 || static_cast<size_t>(column) >= columns.size())
        return;
    captureColumnOrder();
    const bool visibilityChanged = columns[column].visible != (width > 0);
    columns[column].visible = width > 0;
    if (width > 0)
        hiddenWidths[column] = width;
    if (visibilityChanged)
        applyColumns();
    else if (width > 0)
        WslTreeSetColumnWidth(tree, column, width);
}
void Table::setColumnOrder(const std::vector<int> &order)
{
    if (order.size() != columns.size())
        return;
    auto ids = order;
    std::sort(ids.begin(), ids.end());
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] != static_cast<int>(i))
            return;
    captureColumnOrder();
    columnOrder = order;
    applyColumns();
}
std::vector<int> Table::visibleColumns() const
{
    std::vector<int> result(columns.size());
    result.resize(tree ? WslTreeVisibleColumns(tree, result.data(), result.size()) : 0);
    return result;
}
int Table::selectedIndex() const
{
    return tree ? WslTreeSelected(tree, -1) : -1;
}
std::vector<int> Table::selectedIndices() const
{
    std::vector<int> result;
    if (tree)
        for (int i = WslTreeSelected(tree, -1); i >= 0; i = WslTreeSelected(tree, i))
            result.push_back(i);
    return result;
}
void Table::ensureVisible(int index)
{
    if (tree)
        WslTreeEnsureVisible(tree, index);
}
bool Table::rowRect(int index, RECT &rect) const
{
    return tree && WslTreeRowRect(tree, index, &rect);
}
void Table::centerSelection()
{
    const int index = selectedIndex();
    if (index >= 0)
        WslTreeCenter(tree, index);
}
namespace
{
// The host's chooser is internal (not an SDK export). Keep its available/visible
// column interaction while applying the result through the public TreeNew API.
struct ColumnChoices
{
    std::vector<Column> columns;
    std::vector<int> visible;
    std::vector<int> available;
    HFONT font = nullptr;
};
constexpr int AvailableColumns = 410, VisibleColumns = 411, ShowColumn = 412, HideColumn = 413,
              MoveColumnUp = 414, MoveColumnDown = 415;
void fillColumnChoices(HWND dialog, ColumnChoices &choices)
{
    auto fill = [&](int id, const std::vector<int> &columns) {
        HWND list = GetDlgItem(dialog, id);
        const auto selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (int column : columns)
            SendMessageW(list, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(choices.columns[column].title.c_str()));
        if (!columns.empty())
            SendMessageW(list, LB_SETCURSEL,
                         std::clamp(static_cast<int>(selected), 0, static_cast<int>(columns.size()) - 1), 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    };
    fill(AvailableColumns, choices.available);
    fill(VisibleColumns, choices.visible);
    EnableWindow(GetDlgItem(dialog, ShowColumn), !choices.available.empty());
    EnableWindow(GetDlgItem(dialog, HideColumn), choices.visible.size() > 1);
}
INT_PTR CALLBACK columnChoicesProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto choices = reinterpret_cast<ColumnChoices *>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        choices = reinterpret_cast<ColumnChoices *>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(choices));
        choices->font = WslCreateUiFont(dialog);
        auto child = [&](const wchar_t *type, const wchar_t *label, DWORD style, int id, int x, int y,
                         int width, int height) {
            HWND control =
                CreateWindowExW(wcscmp(type, L"LISTBOX") == 0 ? WS_EX_CLIENTEDGE : 0, type, label,
                                WS_CHILD | WS_VISIBLE | style, scale(dialog, x), scale(dialog, y),
                                scale(dialog, width), scale(dialog, height), dialog,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(choices->font), TRUE);
        };
        child(L"STATIC", L"Available columns:", 0, -1, 12, 12, 220, 20);
        child(L"STATIC", L"Visible columns:", 0, -1, 320, 12, 220, 20);
        child(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, AvailableColumns,
              12, 34, 220, 266);
        child(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, VisibleColumns,
              320, 34, 220, 266);
        child(L"BUTTON", L"Show >", WS_TABSTOP, ShowColumn, 242, 105, 68, 24);
        child(L"BUTTON", L"< Hide", WS_TABSTOP, HideColumn, 242, 137, 68, 24);
        child(L"BUTTON", L"Move up", WS_TABSTOP, MoveColumnUp, 320, 310, 106, 24);
        child(L"BUTTON", L"Move down", WS_TABSTOP, MoveColumnDown, 434, 310, 106, 24);
        child(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 372, 351, 80, 24);
        child(L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, 460, 351, 80, 24);
        RECT area{0, 0, scale(dialog, 552), scale(dialog, 387)};
        AdjustWindowRectExForDpi(&area, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)),
                                 GetDpiForWindow(dialog));
        SetWindowPos(dialog, nullptr, 0, 0, area.right - area.left, area.bottom - area.top,
                     SWP_NOZORDER | SWP_NOMOVE);
        WslPositionDialog(dialog, GetParent(dialog));
        fillColumnChoices(dialog, *choices);
        WslApplyTheme(dialog);
        return TRUE;
    }
    if (!choices)
        return FALSE;
    if (message == WM_COMMAND)
    {
        int command = LOWORD(wparam);
        if (HIWORD(wparam) == LBN_DBLCLK)
            command = command == AvailableColumns ? ShowColumn : HideColumn;
        if (command == IDOK || command == IDCANCEL)
        {
            EndDialog(dialog, command);
            return TRUE;
        }
        const bool show = command == ShowColumn;
        if (show || command == HideColumn)
        {
            auto &from = show ? choices->available : choices->visible;
            auto &to = show ? choices->visible : choices->available;
            const int selected = static_cast<int>(
                SendDlgItemMessageW(dialog, show ? AvailableColumns : VisibleColumns, LB_GETCURSEL, 0, 0));
            if (selected >= 0 && static_cast<size_t>(selected) < from.size() && (show || from.size() > 1))
            {
                to.push_back(from[selected]);
                from.erase(from.begin() + selected);
                std::sort(choices->available.begin(), choices->available.end(), [&](int a, int b) {
                    return choices->columns[a].title < choices->columns[b].title;
                });
                fillColumnChoices(dialog, *choices);
            }
            return TRUE;
        }
        if (command == MoveColumnUp || command == MoveColumnDown)
        {
            const int selected =
                static_cast<int>(SendDlgItemMessageW(dialog, VisibleColumns, LB_GETCURSEL, 0, 0));
            const int next = selected + (command == MoveColumnUp ? -1 : 1);
            if (selected >= 0 && next >= 0 && static_cast<size_t>(next) < choices->visible.size())
            {
                std::swap(choices->visible[selected], choices->visible[next]);
                fillColumnChoices(dialog, *choices);
                SendDlgItemMessageW(dialog, VisibleColumns, LB_SETCURSEL, next, 0);
            }
            return TRUE;
        }
    }
    if (message == WM_DESTROY && choices->font)
    {
        DeleteObject(choices->font);
        choices->font = nullptr;
    }
    return FALSE;
}
} // namespace
void Table::showHeaderMenu(POINT point)
{
    const HWND savedWindow = window;
    auto alive = [savedWindow, identity = reinterpret_cast<DWORD_PTR>(this)] {
        DWORD_PTR context = 0;
        return IsWindow(savedWindow) && GetWindowSubclass(savedWindow, tableLayoutProc, 1, &context) &&
               context == identity;
    };
    const int column = headerMenuColumn;
    HMENU menu = CreatePopupMenu();
    const bool valid = column >= 0 && isColumnVisible(column);
    AppendMenuW(menu, MF_STRING | (valid ? 0 : MF_GRAYED), 1, L"Size column to fit");
    AppendMenuW(menu, MF_STRING, 2, L"Size all columns to fit");
    AppendMenuW(menu, MF_STRING | (valid && visibleColumns().size() > 1 ? 0 : MF_GRAYED), 3, L"Hide column");
    AppendMenuW(menu, MF_STRING, 4, L"Reset sort");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"Choose columns...");
    const auto command =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    if (!alive() || !command)
        return;
    if (command == 1 && valid)
        WslTreeAutoSize(tree, column);
    if (command == 2)
        for (int id : visibleColumns())
            WslTreeAutoSize(tree, id);
    if (command == 3 && valid && visibleColumns().size() > 1)
        setColumnWidth(column, 0);
    if (command == 4)
        resetSort();
    if (command == 5)
    {
        ColumnChoices choices{columns, visibleColumns(), {}};
        for (size_t id = 0; id < columns.size(); ++id)
            if (!isColumnVisible(id))
                choices.available.push_back(static_cast<int>(id));
        std::sort(choices.available.begin(), choices.available.end(),
                  [&](int a, int b) { return columns[a].title < columns[b].title; });
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
        std::vector<WORD> dialog{LOWORD(style), HIWORD(style), 0, 0, 0, 0, 0, 300, 240, 0, 0};
        const wchar_t title[] = L"Choose columns";
        dialog.insert(dialog.end(), title, title + std::size(title));
        if (DialogBoxIndirectParamW(instance, reinterpret_cast<DLGTEMPLATE *>(dialog.data()),
                                    GetAncestor(window, GA_ROOT), columnChoicesProc,
                                    reinterpret_cast<LPARAM>(&choices)) != IDOK ||
            !alive())
            return;
        captureColumnOrder();
        columnOrder = choices.visible;
        columnOrder.insert(columnOrder.end(), choices.available.begin(), choices.available.end());
        for (size_t id = 0; id < columns.size(); ++id)
            columns[id].visible =
                std::find(choices.visible.begin(), choices.visible.end(), id) != choices.visible.end();
        applyColumns();
    }
    saveLayout();
    if (command == 3 || command == 5)
        PostMessageW(GetParent(window), ColumnsChangedMessage, 0, 0);
    if (command == 4)
        PostMessageW(GetParent(window), SortResetMessage, 0, reinterpret_cast<LPARAM>(window));
}
const Row *Table::selected() const
{
    int index = selectedIndex();
    return index >= 0 && static_cast<size_t>(index) < rows.size() ? &rows[index] : nullptr;
}
const Row *Table::selectedActionable() const
{
    const auto row = selected();
    return row && !row->removed && WslTreeSelected(tree, selectedIndex()) < 0 ? row : nullptr;
}
void Table::selectKey(const std::string &key)
{
    int next = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].key == key)
        {
            next = static_cast<int>(i);
            break;
        }
    if (selectedIndex() != next)
        WslTreeSelect(tree, next);
}
void Table::order()
{
    if (!ancestryOrder)
        sortRows(rows);
}
void Table::sortRows(std::vector<Row> &items) const
{
    if (sortColumn < 0 || static_cast<size_t>(sortColumn) >= columns.size())
        return;
    const auto column = static_cast<size_t>(sortColumn);
    wchar_t decimal[16]{}, separator[16]{};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, 16);
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, separator, 16);
    auto numericValue = [&](std::wstring value) {
        if (*separator)
            for (size_t position; (position = value.find(separator)) != std::wstring::npos;)
                value.erase(position, wcslen(separator));
        if (*decimal && wcscmp(decimal, L".") != 0)
            if (auto position = value.find(decimal); position != std::wstring::npos)
                value.replace(position, wcslen(decimal), L".");
        return wcstod(value.c_str(), nullptr);
    };
    std::stable_sort(items.begin(), items.end(), [&](const Row &a, const Row &b) {
        if (column >= a.cells.size() || column >= b.cells.size())
            return false;
        int comparison;
        if (columns[column].numeric)
        {
            const auto an = a.numeric.find(column), bn = b.numeric.find(column);
            const double av = an != a.numeric.end() ? an->second : numericValue(a.cells[column]);
            const double bv = bn != b.numeric.end() ? bn->second : numericValue(b.cells[column]);
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
    std::set<std::string> selectedKeys;
    for (int index : selectedIndices())
        if (static_cast<size_t>(index) < rows.size())
            selectedKeys.insert(rows[index].key);
    auto previous = std::move(rows);
    rows.clear();
    rows.reserve(source.size());
    for (const auto &row : source)
        if (!filter || filter(row))
            rows.push_back(row);
    order();
    presenting = true;
    WslTreeSetCount(tree, rows.size());
    std::vector<int> selection;
    for (size_t i = 0; i < rows.size(); ++i)
        if (selectedKeys.count(rows[i].key))
            selection.push_back(static_cast<int>(i));
    WslTreeRestoreSelection(tree, selection.data(), selection.size());
    presenting = false;
    for (size_t i = 0; i < rows.size(); ++i)
        if (i >= previous.size() || previous[i].key != rows[i].key || previous[i].cells != rows[i].cells ||
            previous[i].removed != rows[i].removed ||
            previous[i].highlightedSince != rows[i].highlightedSince || previous[i].data != rows[i].data)
            invalidateRows(static_cast<int>(i), static_cast<int>(i));
    if (previous.size() > rows.size())
        InvalidateRect(window, nullptr, FALSE);
}
void Table::setAncestryOrder(bool enabled, bool showSort)
{
    ancestryOrder = enabled;
    ancestrySortIndicator = showSort;
    setSortIndicator();
}
void Table::setSortIndicator()
{
    if (tree)
        WslTreeSetSort(tree, ancestryOrder && !ancestrySortIndicator ? -1 : sortColumn, descending);
}
void Table::resetSort()
{
    sortColumn = defaultSortColumn;
    descending = defaultSortDescending;
    ancestryOrder = false;
    setSortIndicator();
    present();
    saveLayout();
}
void Table::sort(int column)
{
    if (column < 0 || static_cast<size_t>(column) >= columns.size())
        return;
    descending = sortColumn == column ? !descending : columns[column].numeric;
    sortColumn = column;
    ancestryOrder = false;
    setSortIndicator();
    present();
    saveLayout();
}
void Table::sendNotification(UINT code)
{
    NMHDR notification{window, static_cast<UINT_PTR>(GetDlgCtrlID(window)), code};
    SendMessageW(GetParent(window), WM_NOTIFY, notification.idFrom, reinterpret_cast<LPARAM>(&notification));
}
void Table::treeEvent(int event, int value, int extra, POINT point)
{
    switch (event)
    {
    case WslTreeSelection:
        if (!presenting)
            sendNotification(TableSelectionChanged);
        break;
    case WslTreeSort:
        sortColumn = value;
        descending = extra != 0;
        ancestryOrder = false;
        present();
        saveLayout();
        sendNotification(TableSortChanged);
        break;
    case WslTreeDoubleClick:
        sendNotification(TableDoubleClick);
        break;
    case WslTreeContext:
        SendMessageW(GetParent(window), WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window),
                     MAKELPARAM(point.x, point.y));
        break;
    case WslTreeHeaderContext:
        headerMenuColumn = value;
        showHeaderMenu(point);
        break;
    case WslTreeLayout:
        PostMessageW(window, SaveTableLayout, 0, 0);
        break;
    }
}
int Table::findItem(int start, PCWSTR prefix, size_t length) const
{
    if (rows.empty() || !prefix || !length)
        return -1;
    const size_t first = static_cast<size_t>(std::max(0, start)) % rows.size();
    for (size_t offset = 0; offset < rows.size(); ++offset)
    {
        const size_t index = (first + offset) % rows.size();
        const auto &row = rows[index];
        if (row.cells.empty())
            continue;
        const auto name =
            kind == Kind::Processes && ancestryOrder ? text(row.data, "name") : row.cells.front();
        if (name.size() >= length && CompareStringOrdinal(name.c_str(), static_cast<int>(length), prefix,
                                                          static_cast<int>(length), TRUE) == CSTR_EQUAL)
            return static_cast<int>(index);
    }
    return -1;
}
void Table::invalidateRows(int first, int last) const
{
    if (tree && first >= 0 && first <= last && static_cast<size_t>(first) < rows.size())
        WslTreeInvalidate(tree, first, std::min(last, static_cast<int>(rows.size()) - 1));
}
void Table::rowColors(int index, COLORREF &background, COLORREF &foreground) const
{
    if (index < 0 || static_cast<size_t>(index) >= rows.size())
        return;
    const auto &row = rows[index];
    const auto &item = row.data;
    background = WslIsDarkTheme() ? WslDialogBackground() : GetSysColor(COLOR_WINDOW);
    foreground = WslIsDarkTheme() ? WslDialogText() : GetSysColor(COLOR_WINDOWTEXT);
    bool semantic = false;
    auto apply = [&](bool condition, PCWSTR setting) {
        if (!condition || semantic || disabledHighlights.count(setting))
            return;
        if (WslHostIntegerSetting((std::wstring(L"Use") + setting).c_str()))
        {
            background = WslHostIntegerSetting(setting);
            semantic = true;
        }
    };

    // The ordering below follows proctree.c/thrdlist.c/modlist.c/memlist.c.
    // Linux-only approximations are made by the collector, not from display text.
    if (row.highlightedSince)
    {
        background = WslHostIntegerSetting(row.removed ? L"ColorRemoved" : L"ColorNew");
        semantic = true;
    }
    else if (kind == Kind::Processes)
    {
        apply(item.value("tracer_pid", 0) != 0, L"ColorDebuggedProcesses");
        apply(item.value("is_suspended", false), L"ColorSuspended");
        apply(item.value("is_partially_suspended", false), L"ColorPartiallySuspended");
        apply(item.value("no_tty", false), L"ColorBackgroundProcesses");
        apply(item.value("name", std::string{}) == "wsl-observer", L"ColorHandleFiltered");
        apply(item.value("sudo_root", false), L"ColorElevatedProcesses");
        apply(readSetting(L"Detect32BitProcesses", 0) != 0 && item.value("is_32bit", false),
              L"ColorWow64Processes");
        apply(item.value("euid", item.value("uid", -1)) == 0 && !item.value("sudo_root", false),
              L"ColorSystemProcesses");
        apply(item.value("is_own", false), L"ColorOwnProcesses");
        apply(item.value("is_service", false), L"ColorServiceProcesses");
    }
    else if (kind == Kind::Services)
    {
        const auto active = item.value("active", std::string{});
        const bool stopped = active == "inactive" || active == "failed";
        const auto unitState = item.value("enabled", std::string{});
        apply(stopped && (unitState == "disabled" || unitState.find("masked") == 0), L"ColorServiceDisabled");
        if (!semantic && stopped && WslHostIntegerSetting(L"UseColorServiceStop"))
            foreground = WslHostIntegerSetting(L"ColorServiceStop");
    }
    else if (kind == Kind::Threads)
    {
        const auto wait = item.value("wait_kind", std::string{});
        const auto state = item.value("state", std::string{});
        apply(wait == "suspended" || state == "T" || state == "t", L"ColorThreadSuspended");
        apply(wait == "delay", L"ColorThreadDelayExecution");
        apply(wait == "user_request", L"ColorThreadUserRequest");
        apply(wait == "alert", L"ColorThreadAlertByThreadId");
        apply(wait == "queue", L"ColorThreadQueue");
        apply(wait == "executive", L"ColorThreadExecutive");
    }
    else if (kind == Kind::Memory)
    {
        apply(item.value("execute_pages", false), L"ColorMemoryExecutePages");
        // Linux has no direct equivalent of a Windows CFG bitmap allocation.
        apply(item.value("system_pages", false), L"ColorMemorySystemPages");
        apply(item.value("private_pages", false), L"ColorMemoryPrivatePages");
    }
    else if (kind == Kind::Modules)
    {
        // Normal ELF ASLR is not the Windows image-base collision condition.
        apply(item.value("known_library", false), L"ColorModuleImageKnownDll");
        apply(item.value("native_module", false), L"ColorModuleSystem");
        apply(item.value("mapped_module", false), L"ColorModuleMapped");
    }
    else if (kind == Kind::Network)
        apply(item.value("pid", 0) == 0, L"ColorNetworkUnknownProcess");
    else if (kind == Kind::Handles)
        apply(item.value("inherited", false), L"ColorInheritHandles");
    else if (kind == Kind::Environment)
    {
        const auto scope = item.value("scope", std::string("process"));
        apply(scope == "process", L"ColorEnvironmentProcess");
        apply(scope == "user", L"ColorEnvironmentUser");
        apply(scope == "system", L"ColorEnvironmentSystem");
    }

    if (semantic)
    {
        const auto brightness =
            (std::min({GetRValue(background), GetGValue(background), GetBValue(background)}) +
             std::max({GetRValue(background), GetGValue(background), GetBValue(background)})) /
            2;
        foreground = brightness > 100 ? RGB(0, 0, 0) : RGB(255, 255, 255);
    }
}
HFONT Table::rowFont(int index) const
{
    const HFONT normal = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    if (index < 0 || static_cast<size_t>(index) >= rows.size())
        return normal;
    const auto &data = rows[index].data;
    const bool bold = (kind == Kind::Modules && data.value("main_module", false)) ||
                      (kind == Kind::Services && data.value("active", std::string{}) == "active");
    if (!bold)
        return normal;
    if (boldSourceFont != normal)
    {
        if (boldFont)
            DeleteObject(boldFont);
        LOGFONTW description{};
        GetObjectW(normal, sizeof(description), &description);
        description.lfWeight = FW_BOLD;
        boldFont = CreateFontIndirectW(&description);
        boldSourceFont = normal;
    }
    return boldFont ? boldFont : normal;
}
void Table::releaseDrawingResources()
{
    if (boldFont)
        DeleteObject(boldFont);
    boldFont = nullptr;
    boldSourceFont = nullptr;
}
std::wstring Table::selectedText() const
{
    std::wstring result;
    const auto visible = visibleColumns();
    for (int index : selectedIndices())
    {
        if (static_cast<size_t>(index) >= rows.size())
            continue;
        if (!result.empty())
            result += L"\r\n";
        for (size_t i = 0; i < visible.size(); ++i)
        {
            if (i)
                result += L'\t';
            if (static_cast<size_t>(visible[i]) < rows[index].cells.size())
                result += rows[index].cells[visible[i]];
        }
    }
    return result;
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
    const auto visible = visibleColumns();
    for (size_t i = 0; i < visible.size(); ++i)
        result += (i ? L"\t" : L"") + escape(columns[visible[i]].title);
    result += L"\r\n";
    for (const auto &row : rows)
    {
        for (size_t i = 0; i < visible.size(); ++i)
        {
            if (i)
                result += L"\t";
            if (static_cast<size_t>(visible[i]) < row.cells.size())
                result += escape(row.cells[visible[i]]);
        }
        result += L"\r\n";
    }
    return result;
}
} // namespace wsl
