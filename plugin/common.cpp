#include "common.hpp"
#include "settings.hpp"
#include "host_bridge.h"
#include <unordered_map>
#include <algorithm>
#include <commdlg.h>
#include <iomanip>
#include <sstream>
#include <uxtheme.h>
#include <vssym32.h>
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
LRESULT CALLBACK tableHeaderProc(HWND header, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                 DWORD_PTR context)
{
    auto table = reinterpret_cast<Table *>(context);
    if (message == WM_CONTEXTMENU && header == table->fixedHeader)
    {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1)
        {
            RECT bounds{};
            GetWindowRect(header, &bounds);
            point = {bounds.left + 8, (bounds.top + bounds.bottom) / 2};
        }
        table->showHeaderMenu(point);
        return 0;
    }
    // Like TreeNew, track the header itself. Common controls do not reliably
    // supply CDIS_HOT once its header painting is replaced with custom drawing.
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)
        table->trackHeaderHover({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, header);
    else if (message == WM_MOUSELEAVE || message == WM_CANCELMODE)
        table->clearHeaderHover();
    else if (message == WM_NCDESTROY)
        RemoveWindowSubclass(header, tableHeaderProc, id);
    return DefSubclassProc(header, message, wparam, lparam);
}
LRESULT CALLBACK tableLayoutProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                 DWORD_PTR context)
{
    auto table = reinterpret_cast<Table *>(context);
    if (message == SaveTableLayout)
    {
        table->updateColumnGeometry();
        table->saveLayout();
        return 0;
    }
    if (message == WM_TIMER && wparam == HighlightTimer)
    {
        table->expireHighlights();
        return 0;
    }
    if (message == WM_MOUSEMOVE)
    {
        table->trackHover({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        table->updateCellTooltip({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
    }
    if (message == WM_MOUSELEAVE)
    {
        table->clearHover();
        SendMessageW(table->cellTooltip, TTM_POP, 0, 0);
    }
    if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_KEYDOWN ||
        message == WM_MOUSEWHEEL || message == WM_HSCROLL || message == WM_VSCROLL)
        SendMessageW(table->cellTooltip, TTM_POP, 0, 0);
    if (message == WM_CONTEXTMENU)
    {
        const HWND header = ListView_GetHeader(window);
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        RECT bounds{};
        GetWindowRect(header, &bounds);
        const bool keyboard = point.x == -1 && point.y == -1;
        if (reinterpret_cast<HWND>(wparam) == header || (!keyboard && PtInRect(&bounds, point)))
        {
            if (keyboard)
                point = {bounds.left + scale(window, 12), (bounds.top + bounds.bottom) / 2};
            // Header controls also send NM_RCLICK. Handle only the context-menu
            // message so one right-click cannot open both column and row menus.
            table->showHeaderMenu(point);
            return 0;
        }
    }
    if (message == WM_NOTIFY)
    {
        auto header = reinterpret_cast<NMHDR *>(lparam);
        if (header->hwndFrom == table->fixedHeader)
            return table->fixedHeaderNotify(header);
        if (header->hwndFrom == table->cellTooltip && header->code == TTN_GETDISPINFOW)
        {
            table->provideCellTooltip(reinterpret_cast<NMTTDISPINFOW *>(lparam));
            return 0;
        }
        if (header->hwndFrom == ListView_GetHeader(window))
        {
            if (header->code == NM_CUSTOMDRAW)
                return table->drawHeader(reinterpret_cast<NMCUSTOMDRAW *>(lparam));
            if (header->code == NM_RCLICK)
                return 0; // WM_CONTEXTMENU owns the header popup.
            if ((header->code == HDN_BEGINTRACKW || header->code == HDN_BEGINTRACKA) &&
                !table->isColumnVisible(reinterpret_cast<NMHEADERW *>(lparam)->iItem))
                return TRUE;
        }
        const auto result = DefSubclassProc(window, message, wparam, lparam);
        if (header->hwndFrom == ListView_GetHeader(window) &&
            (header->code == HDN_ITEMCHANGEDW || header->code == HDN_ITEMCHANGEDA))
            table->updateColumnGeometry();
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
        table->releaseDrawingResources();
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, tableLayoutProc, id);
    // ListView scrolls only the area covered by its columns. We also paint row
    // backgrounds to the right of them, so that area must move with the rows.
    const bool mayScrollVertically = message == WM_VSCROLL || message == WM_MOUSEWHEEL ||
                                    message == WM_KEYDOWN || message == WM_CHAR ||
                                    message == LVM_ENSUREVISIBLE || message == LVM_SCROLL;
    const int previousTop = mayScrollVertically ? ListView_GetTopIndex(window) : -1;
    const auto result = DefSubclassProc(window, message, wparam, lparam);
    if (mayScrollVertically && previousTop != ListView_GetTopIndex(window))
        table->invalidateRows(0, static_cast<int>(table->rows.size()) - 1);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
    {
        const int selected = ListView_GetNextItem(window, -1, LVNI_SELECTED);
        table->invalidateRows(selected, selected);
    }
    if (message == WM_SIZE || message == WM_HSCROLL || message == WM_MOUSEHWHEEL || message == LVM_SCROLL ||
        message == LVM_SETCOLUMNWIDTH || message == LVM_SETCOLUMNORDERARRAY || message == WM_SETFONT)
    {
        table->updateColumnGeometry();
        if (message == WM_HSCROLL || message == WM_MOUSEHWHEEL || message == LVM_SCROLL)
            InvalidateRect(window, nullptr, FALSE);
    }
    return result;
}
} // namespace

void Table::create(HWND parent, int id, std::vector<Column> definitions, int defaultSort,
                   bool defaultDescending)
{
    columns = std::move(definitions);
    hiddenWidths.resize(columns.size());
    defaultSortColumn = defaultSort;
    defaultSortDescending = defaultDescending;
    settingsPrefix = L"Table." + std::to_wstring(id) + L".";
    auto savedSort = readSetting((settingsPrefix + L"SortColumn").c_str(), static_cast<DWORD>(defaultSort));
    sortColumn = savedSort < columns.size() ? static_cast<int>(savedSort) : -1;
    descending = readSetting((settingsPrefix + L"Descending").c_str(), defaultDescending ? 1 : 0) != 0;
    window = control(parent, WC_LISTVIEWW, L"",
                     WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SINGLESEL, id);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(WslGetHostFont()), TRUE);
    ListView_SetExtendedListViewStyle(window,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
    if (HWND tooltip = ListView_GetToolTips(window))
    {
        // Match the native grids: ordinary tooltip font, generous reading time,
        // and a wrapped content tip rather than an unfolding cell label.
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, scale(window, 550));
        SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAXSHORT);
        if (WslHostIntegerSetting(L"EnableInstantTooltips"))
            SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 0);
        SetWindowLongPtrW(tooltip, GWL_STYLE, GetWindowLongPtrW(tooltip, GWL_STYLE) | TTS_NOPREFIX);
    }
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
        hiddenWidths[i] = column.cx > 0 ? column.cx : scale(parent, columns[i].width);
        if (!readSetting((columnKey + L".Visible").c_str(), columns[i].visible ? 1 : 0))
            column.cx = 0;
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
    SetWindowSubclass(ListView_GetHeader(window), tableHeaderProc, 1, reinterpret_cast<DWORD_PTR>(this));
    WslApplyTheme(window);
    // TreeNew also keeps its fixed header separate and does not make that
    // header draggable. Choose columns can still choose a different first column.
    fixedHeader = CreateWindowExW(
        0, WC_HEADERW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | HDS_BUTTONS | HDS_HORZ | HDS_FULLDRAG,
        0, 0, 0, 0, window, nullptr, instance, nullptr);
    HDITEMW fixed{};
    fixed.mask = HDI_TEXT | HDI_WIDTH | HDI_FORMAT;
    fixed.pszText = const_cast<PWSTR>(L"");
    fixed.fmt = HDF_STRING;
    Header_InsertItem(fixedHeader, 0, &fixed);
    SetWindowSubclass(fixedHeader, tableHeaderProc, 1, reinterpret_cast<DWORD_PTR>(this));
    WslApplyTheme(fixedHeader);

    // ListView's label tips use its scrolling column rectangles. Our tooltip
    // follows the visible cell instead, including the fixed column after a scroll.
    cellTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                  WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                                  CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
    TOOLINFOW tool{sizeof(tool)};
    // Relay input ourselves so crossing into another row restarts the native
    // hover delay even though the tooltip tool covers the whole viewport.
    tool.uFlags = 0;
    tool.hwnd = window;
    tool.uId = 1;
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(cellTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    SendMessageW(cellTooltip, TTM_SETMAXTIPWIDTH, 0, scale(window, 550));
    SendMessageW(cellTooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAXSHORT);
    if (WslHostIntegerSetting(L"EnableInstantTooltips"))
        SendMessageW(cellTooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 0);
    updateColumnGeometry();
}

void Table::updateColumnGeometry()
{
    if (!fixedHeader || updatingGeometry || !IsWindow(window))
        return;
    updatingGeometry = true;
    const int previousWidth = fixedWidth;
    const int previousColumn = fixedColumn;
    HWND header = ListView_GetHeader(window);
    drawingColumns.clear();
    for (const int column : visibleColumns())
    {
        RECT bounds{};
        Header_GetItemRect(header, column, &bounds);
        MapWindowPoints(header, window, reinterpret_cast<POINT *>(&bounds), 2);
        drawingColumns.push_back({column, bounds});
    }
    fixedColumn = drawingColumns.empty() ? -1 : drawingColumns.front().index;
    fixedWidth = fixedColumn >= 0 ? ListView_GetColumnWidth(window, fixedColumn) : 0;
    RECT headerBounds{}, client{};
    GetWindowRect(header, &headerBounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&headerBounds), 2);
    GetClientRect(window, &client);
    const auto gridFont = SendMessageW(window, WM_GETFONT, 0, 0);
    SendMessageW(fixedHeader, WM_SETFONT, gridFont, FALSE);
    // TreeNew uses its grid font for content tips too. The replacement tooltip
    // does not inherit it automatically because it is a separate popup window.
    if (SendMessageW(cellTooltip, WM_GETFONT, 0, 0) != gridFont)
        SendMessageW(cellTooltip, WM_SETFONT, gridFont, FALSE);
    if (fixedColumn >= 0)
    {
        HDITEMW item{};
        item.mask = HDI_TEXT | HDI_WIDTH | HDI_FORMAT;
        item.pszText = columns[fixedColumn].title.data();
        item.cxy = fixedWidth;
        item.fmt = HDF_STRING | (columns[fixedColumn].numeric ? HDF_RIGHT : HDF_LEFT);
        if ((!ancestryOrder || ancestrySortIndicator) && fixedColumn == sortColumn)
            item.fmt |= descending ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(fixedHeader, 0, &item);
    }
    SetWindowPos(fixedHeader, HWND_TOP, 0, headerBounds.top,
                 std::min(fixedWidth, static_cast<int>(client.right)), headerBounds.bottom - headerBounds.top,
                 SWP_NOACTIVATE);
    // Clip the scrolling header, rather than relying on overlapping sibling
    // painting. The fixed divider must also own mouse input at the boundary.
    RECT scrollingHeader{};
    GetClientRect(header, &scrollingHeader);
    scrollingHeader.left = std::max(0L, fixedWidth - headerBounds.left);
    HRGN region = CreateRectRgnIndirect(&scrollingHeader);
    if (!SetWindowRgn(header, region, TRUE))
        DeleteObject(region);
    ShowWindow(fixedHeader, fixedColumn >= 0 ? SW_SHOWNA : SW_HIDE);
    TOOLINFOW tool{sizeof(tool)};
    tool.hwnd = window;
    tool.uId = 1;
    tool.rect = client;
    tool.rect.top = headerBounds.bottom;
    SendMessageW(cellTooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    updatingGeometry = false;
    if (previousWidth != fixedWidth || previousColumn != fixedColumn)
        InvalidateRect(window, nullptr, FALSE);
}

LRESULT Table::fixedHeaderNotify(NMHDR *notification)
{
    if (notification->code == NM_CUSTOMDRAW)
    {
        auto draw = *reinterpret_cast<NMCUSTOMDRAW *>(notification);
        if (draw.dwDrawStage == CDDS_ITEMPREPAINT)
            draw.dwItemSpec = fixedColumn;
        return drawHeader(&draw);
    }
    if (updatingGeometry)
        return 0;
    const auto item = reinterpret_cast<NMHEADERW *>(notification);
    if (notification->code == HDN_ITEMCLICKW || notification->code == HDN_ITEMCLICKA)
    {
        // Keep the parent on the same notification path as a scrolling header
        // click (the process tree also updates its hierarchy/sort state there).
        NMLISTVIEW click{};
        click.hdr = {window, static_cast<UINT_PTR>(GetDlgCtrlID(window)), LVN_COLUMNCLICK};
        click.iItem = -1;
        click.iSubItem = fixedColumn;
        SendMessageW(GetParent(window), WM_NOTIFY, click.hdr.idFrom, reinterpret_cast<LPARAM>(&click));
    }
    else if ((notification->code == HDN_ITEMCHANGINGW || notification->code == HDN_ITEMCHANGINGA) &&
             item->pitem && (item->pitem->mask & HDI_WIDTH))
    {
        item->pitem->cxy = std::max(scale(window, 20), item->pitem->cxy);
    }
    else if ((notification->code == HDN_ITEMCHANGEDW || notification->code == HDN_ITEMCHANGEDA) &&
             item->pitem && (item->pitem->mask & HDI_WIDTH))
    {
        ListView_SetColumnWidth(window, fixedColumn, item->pitem->cxy);
    }
    else if (notification->code == HDN_ENDTRACKW || notification->code == HDN_ENDTRACKA)
        PostMessageW(window, SaveTableLayout, 0, 0);
    else if (notification->code == HDN_DIVIDERDBLCLICKW || notification->code == HDN_DIVIDERDBLCLICKA)
    {
        const int column = fixedColumn;
        HDC dc = GetDC(window);
        const auto previousFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
        SIZE title{};
        GetTextExtentPoint32W(dc, columns[column].title.c_str(),
                              static_cast<int>(columns[column].title.size()), &title);
        SelectObject(dc, previousFont);
        ReleaseDC(window, dc);
        // Autosizing an empty owner-data list returns zero: keep the header
        // readable instead of accidentally hiding the pinned column.
        ListView_SetColumnWidth(window, column, LVSCW_AUTOSIZE);
        ListView_SetColumnWidth(window, column,
                                std::max(ListView_GetColumnWidth(window, column),
                                         static_cast<int>(title.cx) + scale(window, 20)));
        PostMessageW(window, SaveTableLayout, 0, 0);
    }
    return 0;
}

void Table::updateCellTooltip(POINT point)
{
    LVHITTESTINFO hit{};
    hit.pt = point;
    const int row = ListView_HitTest(window, &hit);
    int column = -1;
    if (row >= 0)
    {
        if (point.x >= 0 && point.x < fixedWidth)
            column = fixedColumn;
        else
            for (const auto &candidate : drawingColumns)
                if (candidate.index != fixedColumn && point.x >= candidate.bounds.left &&
                    point.x < candidate.bounds.right)
                {
                    column = candidate.index;
                    break;
                }
    }
    if (row != tooltipRow || column != tooltipColumn)
    {
        tooltipRow = row;
        tooltipColumn = column;
        SendMessageW(cellTooltip, TTM_ACTIVATE, FALSE, 0);
        SendMessageW(cellTooltip, TTM_ACTIVATE, TRUE, 0);
    }
    MSG mouse{};
    mouse.hwnd = window;
    mouse.message = WM_MOUSEMOVE;
    mouse.lParam = MAKELPARAM(point.x, point.y);
    mouse.time = GetMessageTime();
    mouse.pt = point;
    ClientToScreen(window, &mouse.pt);
    SendMessageW(cellTooltip, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&mouse));
}

void Table::provideCellTooltip(NMTTDISPINFOW *tip)
{
    tooltipText.clear();
    if (WslHostIntegerSetting(L"EnableTooltipSupport") && tooltipRow >= 0 && tooltipColumn >= 0 &&
        static_cast<size_t>(tooltipRow) < rows.size())
    {
        const auto &row = rows[tooltipRow];
        if (tooltipColumn == 0 && infoTip)
            tooltipText = infoTip(row);
        else if (static_cast<size_t>(tooltipColumn) < row.cells.size())
        {
            HDC dc = GetDC(window);
            const auto old =
                SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
            SIZE size{};
            const auto &value = row.cells[tooltipColumn];
            GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &size);
            SelectObject(dc, old);
            ReleaseDC(window, dc);
            if (size.cx > ListView_GetColumnWidth(window, tooltipColumn) - scale(window, 12))
                tooltipText = value;
        }
    }
    tip->lpszText = tooltipText.data();
}

void Table::drawFixedDivider(HDC dc) const
{
    if (fixedColumn < 0 || !fixedWidth)
        return;
    RECT bounds{};
    GetClientRect(window, &bounds);
    bounds.left = fixedWidth - 1;
    bounds.right = fixedWidth;
    SetDCBrushColor(dc, WslIsDarkTheme() ? RGB(90, 90, 90) : RGB(215, 215, 215));
    FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
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
        writeSetting((columnKey + L".Visible").c_str(), width > 0 ? 1 : 0);
        writeSetting((columnKey + L".Width").c_str(), MulDiv(width > 0 ? width : hiddenWidths[i], 96,
                                                             static_cast<int>(GetDpiForWindow(window))));
        writeSetting((columnKey + L".Order").c_str(), static_cast<DWORD>(order[i]));
    }
}

bool Table::isColumnVisible(size_t column) const
{
    return column < columns.size() && ListView_GetColumnWidth(window, static_cast<int>(column)) > 0;
}
std::vector<int> Table::visibleColumns() const
{
    std::vector<int> order(columns.size());
    ListView_GetColumnOrderArray(window, static_cast<int>(order.size()), order.data());
    order.erase(std::remove_if(order.begin(), order.end(), [&](int id) { return !isColumnVisible(id); }),
                order.end());
    return order;
}
void Table::centerSelection()
{
    const int selected = ListView_GetNextItem(window, -1, LVNI_SELECTED);
    if (selected < 0)
        return;
    ListView_EnsureVisible(window, selected, FALSE);
    RECT item{}, client{}, header{};
    ListView_GetItemRect(window, selected, &item, LVIR_BOUNDS);
    GetClientRect(window, &client);
    GetWindowRect(ListView_GetHeader(window), &header);
    const int center = (client.bottom + header.bottom - header.top) / 2;
    ListView_Scroll(window, 0, (item.top + item.bottom) / 2 - center);
}

namespace
{
// The host's chooser is internal (not an SDK export), and supports TreeNew only.
// Keep the same available/visible-column interaction for our owner-data ListView.
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
    HWND header = ListView_GetHeader(window);
    HDHITTESTINFO hit{};
    hit.pt = point;
    ScreenToClient(header, &hit.pt);
    RECT fixedBounds{};
    GetWindowRect(fixedHeader, &fixedBounds);
    const int column =
        PtInRect(&fixedBounds, point)
            ? fixedColumn
            : static_cast<int>(SendMessageW(header, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)));
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
    if (!alive())
        return;
    if (!command)
        return;
    auto sizeToFit = [&](int id) {
        ListView_SetColumnWidth(window, id, LVSCW_AUTOSIZE);
        HDC dc = GetDC(window);
        const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
        SIZE size{};
        GetTextExtentPoint32W(dc, columns[id].title.c_str(), static_cast<int>(columns[id].title.size()),
                              &size);
        SelectObject(dc, old);
        ReleaseDC(window, dc);
        ListView_SetColumnWidth(
            window, id,
            std::max(ListView_GetColumnWidth(window, id), static_cast<int>(size.cx) + scale(window, 20)));
    };
    if (command == 1 && valid)
        sizeToFit(column);
    if (command == 2)
        for (int id : visibleColumns())
            sizeToFit(id);
    if (command == 3 && valid && visibleColumns().size() > 1)
    {
        hiddenWidths[column] = ListView_GetColumnWidth(window, column);
        ListView_SetColumnWidth(window, column, 0);
    }
    if (command == 4)
    {
        const auto key = selected() ? selected()->key : "";
        sortColumn = defaultSortColumn;
        descending = defaultSortDescending;
        ancestryOrder = true; // Force header arrow refresh even when tree mode was active.
        setAncestryOrder(false);
        order();
        selectKey(key);
        InvalidateRect(window, nullptr, FALSE);
    }
    if (command == 5)
    {
        ColumnChoices choices{columns, visibleColumns(), {}};
        for (size_t id = 0; id < columns.size(); ++id)
            if (!isColumnVisible(id))
                choices.available.push_back(static_cast<int>(id));
        std::sort(choices.available.begin(), choices.available.end(),
                  [&](int a, int b) { return columns[a].title < columns[b].title; });
        // Empty standard dialog template; controls are sized from its actual DPI.
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
        std::vector<WORD> dialog{LOWORD(style), HIWORD(style), 0, 0, 0, 0, 0, 300, 240, 0, 0};
        const wchar_t title[] = L"Choose columns";
        dialog.insert(dialog.end(), title, title + std::size(title));
        if (DialogBoxIndirectParamW(instance, reinterpret_cast<DLGTEMPLATE *>(dialog.data()),
                                    GetAncestor(window, GA_ROOT), columnChoicesProc,
                                    reinterpret_cast<LPARAM>(&choices)) != IDOK ||
            !alive())
            return;
        std::vector<int> order = choices.visible;
        order.insert(order.end(), choices.available.begin(), choices.available.end());
        for (size_t id = 0; id < columns.size(); ++id)
        {
            const int width = ListView_GetColumnWidth(window, static_cast<int>(id));
            if (width > 0)
                hiddenWidths[id] = width;
            const bool visible =
                std::find(choices.visible.begin(), choices.visible.end(), id) != choices.visible.end();
            ListView_SetColumnWidth(window, static_cast<int>(id),
                                    visible ? std::max(1, hiddenWidths[id]) : 0);
        }
        ListView_SetColumnOrderArray(window, static_cast<int>(order.size()), order.data());
    }
    updateColumnGeometry();
    saveLayout();
    if (command == 3 || command == 5)
        PostMessageW(GetParent(window), ColumnsChangedMessage, 0, 0);
    if (command == 4)
        PostMessageW(GetParent(window), SortResetMessage, 0, reinterpret_cast<LPARAM>(window));
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
            invalidateRows(i, i);
    }
    // Clear a vacated tail once when processes exit; unchanged snapshots never
    // invalidate headers, scrollbars, or the empty part of the table.
    if (previous.size() > rows.size())
        InvalidateRect(window, nullptr, FALSE);
}
void Table::setAncestryOrder(bool enabled, bool showSort)
{
    if (ancestryOrder == enabled && ancestrySortIndicator == showSort)
        return;
    ancestryOrder = enabled;
    ancestrySortIndicator = showSort;
    HWND header = ListView_GetHeader(window);
    for (size_t i = 0; i < columns.size(); ++i)
    {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        Header_GetItem(header, static_cast<int>(i), &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if ((!enabled || showSort) && static_cast<int>(i) == sortColumn)
            item.fmt |= descending ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(header, static_cast<int>(i), &item);
    }
}
void Table::sort(int column)
{
    if (column < 0 || static_cast<size_t>(column) >= columns.size())
        return;
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
int Table::findItem(const NMLVFINDITEMW &request) const
{
    const auto &find = request.lvfi;
    if (rows.empty() || (find.flags & (LVFI_PARAM | LVFI_NEARESTXY)) || !find.psz || !*find.psz)
        return -1;

    // The native ListView manages the typed prefix, its timeout, and repeated
    // single-letter cycling. iStart already identifies the first row to check.
    const size_t start = std::min(rows.size(), static_cast<size_t>(std::max(0, request.iStart)));
    const size_t count = (find.flags & LVFI_WRAP) ? rows.size() : rows.size() - start;
    const size_t length = wcslen(find.psz);
    const bool prefix = (find.flags & (LVFI_PARTIAL | LVFI_SUBSTRING)) != 0;
    for (size_t offset = 0; offset < count; ++offset)
    {
        const size_t index = (start + offset) % rows.size();
        const auto &row = rows[index];
        if (row.cells.empty())
            continue;
        // Tree indentation is presentation, not part of the process name.
        const std::wstring name =
            kind == Kind::Processes && ancestryOrder ? text(row.data, "name") : row.cells.front();
        if (name.size() < length || (!prefix && name.size() != length))
            continue;
        if (CompareStringOrdinal(name.c_str(), static_cast<int>(length), find.psz, static_cast<int>(length),
                                 TRUE) == CSTR_EQUAL)
            return static_cast<int>(index);
    }
    return -1;
}

bool Table::notify(NMHDR *hdr)
{
    if (hdr->hwndFrom != window)
        return false;
    if (hdr->code == LVN_ITEMCHANGED)
    {
        const auto change = reinterpret_cast<NMLISTVIEW *>(hdr);
        if ((change->uChanged & LVIF_STATE) &&
            ((change->uOldState ^ change->uNewState) & (LVIS_SELECTED | LVIS_FOCUSED)))
            invalidateRows(change->iItem, change->iItem);
        // The owner may also need to update actions for the new selection.
        return false;
    }
    if (hdr->code == LVN_ODSTATECHANGED)
    {
        const auto change = reinterpret_cast<NMLVODSTATECHANGE *>(hdr);
        if ((change->uOldState ^ change->uNewState) & (LVIS_SELECTED | LVIS_FOCUSED))
            invalidateRows(change->iFrom, change->iTo);
        return false;
    }
    if (hdr->code == LVN_ENDSCROLL)
    {
        updateColumnGeometry();
        // The native control scrolls pixels horizontally. Repaint the fixed
        // cell and the newly exposed scrolling cells from their current geometry.
        InvalidateRect(window, nullptr, FALSE);
        return false;
    }
    if (hdr->code == LVN_GETDISPINFOW)
    {
        auto info = reinterpret_cast<NMLVDISPINFOW *>(hdr);
        size_t row = static_cast<size_t>(info->item.iItem), col = static_cast<size_t>(info->item.iSubItem);
        if ((info->item.mask & LVIF_TEXT) && row < rows.size() && col < rows[row].cells.size())
            info->item.pszText = rows[row].cells[col].data();
        return true;
    }
    if (hdr->code == LVN_GETINFOTIPW && infoTip)
    {
        auto tip = reinterpret_cast<NMLVGETINFOTIPW *>(hdr);
        if (tip->iSubItem != 0 || tip->iItem < 0 || static_cast<size_t>(tip->iItem) >= rows.size())
            return false;
        if (tip->pszText && tip->cchTextMax > 0)
            tip->pszText[0] = L'\0';
        if (tip->pszText && tip->cchTextMax > 0 && WslHostIntegerSetting(L"EnableTooltipSupport"))
        {
            auto content = infoTip(rows[tip->iItem]);
            const auto capacity = static_cast<size_t>(tip->cchTextMax - 1);
            if (content.size() > capacity)
            {
                content.resize(capacity);
                if (capacity >= 3)
                    content.replace(capacity - 3, 3, L"...");
            }
            std::copy(content.begin(), content.end(), tip->pszText);
            tip->pszText[content.size()] = L'\0';
        }
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
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    if (draw->nmcd.dwDrawStage == CDDS_POSTPAINT)
    {
        drawFixedDivider(draw->nmcd.hdc);
        return CDRF_DODEFAULT;
    }
    if (draw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT || draw->nmcd.dwItemSpec >= rows.size())
        return CDRF_DODEFAULT;

    const auto &row = rows[draw->nmcd.dwItemSpec];
    const auto &item = row.data;
    COLORREF background = WslIsDarkTheme() ? WslDialogBackground() : GetSysColor(COLOR_WINDOW);
    COLORREF foreground = WslIsDarkTheme() ? WslDialogText() : GetSysColor(COLOR_WINDOWTEXT);
    bool semantic = false;
    auto apply = [&](bool condition, PCWSTR setting) {
        if (!condition || semantic)
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
    const int index = static_cast<int>(draw->nmcd.dwItemSpec);
    const bool selected = (ListView_GetItemState(window, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const bool hot = index == hotRow;
    HDC dc = draw->nmcd.hdc;
    const int saved = SaveDC(dc);
    RECT bounds{};
    ListView_GetItemRect(window, index, &bounds, LVIR_BOUNDS);
    bounds.left = 0;
    RECT client{};
    GetClientRect(window, &client);
    bounds.right = std::max(bounds.right, client.right);
    SetDCBrushColor(dc, background);
    FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    // TreeNew paints the TreeView themed item *over* its semantic background.
    // ListView's built-in hot item replaces that background with opaque blue.
    // Drawing the same transparent TreeView overlay preserves the row color.
    if (selected || hot)
    {
        HTHEME theme = OpenThemeData(window, L"TreeView");
        if (theme)
        {
            const int state = selected ? (hot                    ? TREIS_HOTSELECTED
                                          : GetFocus() == window ? TREIS_SELECTED
                                                                 : TREIS_SELECTEDNOTFOCUS)
                                       : TREIS_HOT;
            DrawThemeBackground(theme, dc, TVP_TREEITEM, state, &bounds, nullptr);
            CloseThemeData(theme);
        }
        else if (selected)
        {
            FillRect(dc, &bounds, GetSysColorBrush(COLOR_HIGHLIGHT));
            foreground = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
    }
    HFONT normal = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    if (kind == Kind::Modules && item.value("main_module", false))
    {
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
        SelectObject(dc, boldFont ? boldFont : normal);
    }
    else
        SelectObject(dc, normal);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, foreground);
    // Clip the scrolling cells before drawing the fixed one. Cached header
    // geometry avoids allocating a column list and querying HWNDs for every row.
    for (const auto &definition : drawingColumns)
    {
        const int column = definition.index;
        if (static_cast<size_t>(column) >= row.cells.size())
            continue;
        const int cellDc = SaveDC(dc);
        RECT cell = definition.bounds;
        if (column == fixedColumn)
        {
            cell.left = 0;
            cell.right = fixedWidth;
            IntersectClipRect(dc, 0, bounds.top, fixedWidth - 1, bounds.bottom);
        }
        else
            IntersectClipRect(dc, fixedWidth, bounds.top, client.right, bounds.bottom);
        cell.top = bounds.top;
        cell.bottom = bounds.bottom;
        cell.left += scale(window, 6);
        cell.right -= scale(window, 6);
        DrawTextW(dc, row.cells[column].c_str(), static_cast<int>(row.cells[column].size()), &cell,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                      (columns[column].numeric ? DT_RIGHT : DT_LEFT));
        RestoreDC(dc, cellDc);
    }
    RestoreDC(dc, saved);
    return CDRF_SKIPDEFAULT;
}

void Table::releaseDrawingResources()
{
    if (cellTooltip)
        DestroyWindow(cellTooltip);
    cellTooltip = nullptr;
    if (boldFont)
        DeleteObject(boldFont);
    boldFont = nullptr;
    boldSourceFont = nullptr;
}
void Table::trackHover(POINT point)
{
    LVHITTESTINFO hit{};
    hit.pt = point;
    int next = ListView_HitTest(window, &hit);
    if (next != hotRow)
    {
        if (hotRow >= 0)
            invalidateRows(hotRow, hotRow);
        hotRow = next;
        if (hotRow >= 0)
            invalidateRows(hotRow, hotRow);
    }
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
    TrackMouseEvent(&tracking);
}
void Table::clearHover()
{
    const int old = hotRow;
    hotRow = -1;
    if (old >= 0)
        invalidateRows(old, old);
}
void Table::invalidateRows(int first, int last) const
{
    if (first < 0 || last < first || rows.empty())
        return;
    const int top = std::max(0, ListView_GetTopIndex(window));
    first = std::max(first, top);
    last = std::min({last, static_cast<int>(rows.size()) - 1, top + ListView_GetCountPerPage(window)});
    if (first > last)
        return;

    RECT bounds{}, end{}, client{}, header{};
    if (!ListView_GetItemRect(window, first, &bounds, LVIR_BOUNDS) ||
        !ListView_GetItemRect(window, last, &end, LVIR_BOUNDS))
        return;
    GetClientRect(window, &client);
    const HWND headerWindow = ListView_GetHeader(window);
    GetWindowRect(headerWindow, &header);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&header), 2);
    bounds.left = 0;
    bounds.right = client.right;
    bounds.top = std::max(bounds.top, header.bottom);
    bounds.bottom = std::min(end.bottom, client.bottom);
    // LVM_REDRAWITEMS stops at the last column. Our custom background and
    // selection fill the complete width, including the otherwise empty tail.
    if (bounds.top < bounds.bottom)
        InvalidateRect(window, &bounds, FALSE);
}
void Table::trackHeaderHover(POINT point, HWND header)
{
    RECT client{};
    GetClientRect(header, &client);
    HDHITTESTINFO hit{};
    hit.pt = point;
    int next = PtInRect(&client, point)
                   ? static_cast<int>(SendMessageW(header, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)))
                   : -1;
    if (header == fixedHeader && next >= 0)
        next = fixedColumn;
    if (next != hotHeaderColumn)
    {
        const int old = hotHeaderColumn;
        hotHeaderColumn = next;
        for (const int column : {old, next})
        {
            RECT bounds{};
            HWND changedHeader = column == fixedColumn ? fixedHeader : ListView_GetHeader(window);
            if (column >= 0 && Header_GetItemRect(changedHeader, column == fixedColumn ? 0 : column, &bounds))
                InvalidateRect(changedHeader, &bounds, FALSE);
        }
    }
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, header, 0};
    TrackMouseEvent(&tracking);
}
void Table::clearHeaderHover()
{
    const int old = hotHeaderColumn;
    hotHeaderColumn = -1;
    RECT bounds{};
    HWND header = old == fixedColumn ? fixedHeader : ListView_GetHeader(window);
    if (old >= 0 && Header_GetItemRect(header, old == fixedColumn ? 0 : old, &bounds))
        InvalidateRect(header, &bounds, FALSE);
}
LRESULT Table::drawHeader(NMCUSTOMDRAW *draw) const
{
    if (draw->dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
    if (draw->dwDrawStage != CDDS_ITEMPREPAINT || draw->dwItemSpec >= columns.size())
        return CDRF_DODEFAULT;
    const int column = static_cast<int>(draw->dwItemSpec);
    const int saved = SaveDC(draw->hdc);
    auto theme = OpenThemeData(draw->hdr.hwndFrom, L"Header");
    if (WslIsDarkTheme())
    {
        SetDCBrushColor(draw->hdc, WslDialogBackground());
        FillRect(draw->hdc, &draw->rc, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    else if (theme)
        DrawThemeBackground(theme, draw->hdc, HP_HEADERITEM,
                            (draw->uItemState & CDIS_SELECTED)                             ? HIS_PRESSED
                            : (column == hotHeaderColumn || (draw->uItemState & CDIS_HOT)) ? HIS_HOT
                                                                                           : HIS_NORMAL,
                            &draw->rc, nullptr);
    else
        FillRect(draw->hdc, &draw->rc, GetSysColorBrush(COLOR_WINDOW));
    RECT textBounds = draw->rc;
    textBounds.left += scale(window, 6);
    textBounds.right -= scale(window, 6);
    SelectObject(draw->hdc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    SetBkMode(draw->hdc, TRANSPARENT);
    SetTextColor(draw->hdc, WslIsDarkTheme() ? WslDialogText() : GetSysColor(COLOR_WINDOWTEXT));
    DrawTextW(draw->hdc, columns[column].title.c_str(), -1, &textBounds,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                  (columns[column].numeric ? DT_RIGHT : DT_LEFT));
    if (theme && !ancestryOrder && column == sortColumn)
    {
        SIZE size{};
        GetThemePartSize(theme, draw->hdc, HP_HEADERSORTARROW, descending ? HSAS_SORTEDDOWN : HSAS_SORTEDUP,
                         nullptr, TS_TRUE, &size);
        RECT arrow{(draw->rc.left + draw->rc.right - size.cx) / 2, draw->rc.top,
                   (draw->rc.left + draw->rc.right + size.cx) / 2, draw->rc.top + size.cy};
        DrawThemeBackground(theme, draw->hdc, HP_HEADERSORTARROW,
                            descending ? HSAS_SORTEDDOWN : HSAS_SORTEDUP, &arrow, nullptr);
    }
    if (theme)
        CloseThemeData(theme);
    if (draw->hdr.hwndFrom == fixedHeader)
    {
        RECT divider = draw->rc;
        divider.left = divider.right - 1;
        SetDCBrushColor(draw->hdc, WslIsDarkTheme() ? RGB(90, 90, 90) : RGB(215, 215, 215));
        FillRect(draw->hdc, &divider, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    RestoreDC(draw->hdc, saved);
    return CDRF_SKIPDEFAULT;
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
