#include "common.hpp"
#include "settings.hpp"
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
        delete reinterpret_cast<Reply *>(message.lParam);
}

void Table::create(HWND parent, int id, std::vector<Column> definitions)
{
    columns = std::move(definitions);
    window = control(parent, WC_LISTVIEWW, L"",
                     WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SINGLESEL, id);
    ListView_SetExtendedListViewStyle(window, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                  LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
    for (size_t i = 0; i < columns.size(); ++i)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.pszText = columns[i].title.data();
        column.cx = scale(parent, columns[i].width);
        column.fmt = columns[i].numeric ? LVCFMT_RIGHT : LVCFMT_LEFT;
        ListView_InsertColumn(window, static_cast<int>(i), &column);
    }
    WslApplyTheme(window);
}
const Row *Table::selected() const
{
    int index = ListView_GetNextItem(window, -1, LVNI_SELECTED);
    return index >= 0 && static_cast<size_t>(index) < rows.size() ? &rows[index] : nullptr;
}
void Table::selectKey(const std::string &key)
{
    ListView_SetItemState(window, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].key == key)
        {
            ListView_SetItemState(window, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            return;
        }
}
void Table::order()
{
    if (sortColumn < 0 || static_cast<size_t>(sortColumn) >= columns.size())
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
void Table::replace(std::vector<Row> next)
{
    std::string key = selected() ? selected()->key : "";
    rows = std::move(next);
    order();
    ListView_SetItemCountEx(window, static_cast<int>(rows.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    selectKey(key);
    InvalidateRect(window, nullptr, FALSE);
}
void Table::sort(int column)
{
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
