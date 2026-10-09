#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Win32 base types must precede commctrl.h and shellapi.h.
#include <windows.h>

#include "../vendor/json.hpp"
#include "tree_bridge.h"
#include <atomic>
#include <commctrl.h>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <shellapi.h>
#include <string>
#include <vector>
#include <set>

namespace wsl
{
using Json = nlohmann::json;
std::wstring wide(const std::string &);
std::string utf8(const std::wstring &);
extern HINSTANCE instance;
extern HFONT font;
constexpr UINT ReplyMessage = WM_APP + 51;
constexpr UINT ColumnsChangedMessage = WM_APP + 83;
constexpr UINT SortResetMessage = WM_APP + 84;
constexpr UINT TableSelectionChanged = NM_FIRST - 400;
constexpr UINT TableSortChanged = NM_FIRST - 401;
constexpr UINT TableDoubleClick = NM_FIRST - 402;
struct Reply
{
    Json data;
    std::string error;
    uintptr_t tag = 0;
    bool componentMissing = false;
};
// A closed window detaches its mailbox. The controller owns no UI objects.
struct Mailbox
{
    std::atomic<HWND> window{nullptr};
    std::mutex gate;

    void detach()
    {
        std::lock_guard<std::mutex> lock(gate);
        window = nullptr;
    }
};
void submit(const std::wstring &distro, Json request, std::shared_ptr<Mailbox> mailbox, uintptr_t tag);
void openHandleSearch(HWND owner, const std::wstring &distro);
void openDetails(HWND owner, const std::wstring &distro, const Json &process);
void openServiceDetails(HWND owner, const std::wstring &distro, const std::string &name);
void openLinuxPath(HWND owner, const std::wstring &distro, const std::wstring &path, bool select = true);
void copyText(HWND owner, const std::wstring &text);
void saveText(HWND owner, const std::wstring &text, const wchar_t *defaultName);
void errorBox(HWND owner, const std::wstring &text);
std::wstring text(const Json &value, const char *key, const std::wstring &fallback = L"");
std::wstring number(double value, int decimals = 2);
std::wstring bytes(uint64_t value);
std::wstring formatStartTime(uint64_t ticks, const Json &clock);
HWND control(HWND parent, const wchar_t *cls, const wchar_t *label, DWORD style, int id);
void place(HWND child, int x, int y, int width, int height);
int scale(HWND window, int value);
int editHeight(HWND window);
void drainReplies(HWND window);

struct Column
{
    std::wstring title;
    int width;
    bool numeric = false;
    bool visible = true;
};
struct Row
{
    std::vector<std::wstring> cells;
    Json data;
    std::string key;
    // Removed rows remain copyable until the host highlighting period expires.
    bool removed = false;
    ULONGLONG highlightedSince = 0;
    // Sorting must not depend on rounding, hidden zeroes, or displayed units.
    std::map<size_t, double> numeric;
};
class Table
{
  public:
    enum class Kind
    {
        Generic,
        Processes,
        Services,
        Network,
        Threads,
        Modules,
        Memory,
        Handles,
        Environment
    };
    Kind kind = Kind::Generic;
    std::set<std::wstring> disabledHighlights;
    // Supplied by the main view; details grids retain their ordinary label tips.
    std::function<std::wstring(const Row &)> infoTip;
    HWND window = nullptr;
    std::vector<Row> rows;
    std::vector<Column> columns;
    int sortColumn = -1;
    bool descending = false;
    void create(HWND parent, int id, std::vector<Column> definitions, int defaultSort = -1,
                bool defaultDescending = false);
    void saveLayout() const;
    bool isColumnVisible(size_t column) const;
    std::vector<int> visibleColumns() const;
    void centerSelection();
    void showHeaderMenu(POINT point);
    void invalidateRows(int first, int last) const;
    void releaseDrawingResources();
    int selectedIndex() const;
    std::vector<int> selectedIndices() const;
    void ensureVisible(int index);
    bool rowRect(int index, RECT &rect) const;
    int columnWidth(int column) const;
    void setColumnWidth(int column, int width);
    void setColumnOrder(const std::vector<int> &order);
    void setSortIndicator();
    void resetSort();
    // Called only by the SDK adapter and the window's lifecycle subclass.
    void treeEvent(int event, int value, int extra, POINT point);
    void applyFont();
    void detachTree();
    void setAncestryOrder(bool enabled, bool showSort = false);
    // Always pass the complete snapshot. Filtering must not look like removal.
    // The predicate is retained for expiry redraws; capture local values by value.
    // Incomplete collections cannot establish that an absent object exited.
    void replace(std::vector<Row> next, std::function<bool(const Row &)> filter = {}, bool complete = true);
    void clear();
    void expireHighlights();
    void sort(int column);
    void sortRows(std::vector<Row> &items) const;

    const Row *selected() const;
    const Row *selectedActionable() const;
    void selectKey(const std::string &key);
    std::wstring selectedText() const;
    std::wstring exportText() const;

  private:
    std::wstring settingsPrefix;
    bool ancestryOrder = false;
    bool ancestrySortIndicator = false;
    int defaultSortColumn = -1;
    bool defaultSortDescending = false;
    std::vector<int> hiddenWidths;
    std::vector<int> columnOrder;
    mutable HFONT boldFont = nullptr;
    mutable HFONT boldSourceFont = nullptr;
    WSL_TREE *tree = nullptr;
    std::wstring tooltipText;
    bool presenting = false;
    int headerMenuColumn = -1;
    void applyColumns();
    void captureColumnOrder();
    void sendNotification(UINT code);
    void rowColors(int index, COLORREF &background, COLORREF &foreground) const;
    HFONT rowFont(int index) const;
    int findItem(int start, PCWSTR prefix, size_t length) const;
    bool initialized = false;
    std::vector<Row> source;
    std::function<bool(const Row &)> filter;
    void present();
    void refreshTimer();
    void order();
};
} // namespace wsl
extern "C" void WslApplyTheme(HWND);
extern "C" HFONT WslGetHostFont(void);

extern "C" BOOL WslIsDarkTheme(void);

extern "C" HFONT WslCreateUiFont(HWND window);
extern "C" HFONT WslCreateTextFont(HWND window);
extern "C" COLORREF WslDialogBackground(void);
extern "C" COLORREF WslDialogText(void);
extern "C" BOOL WslHasGlobalSearch(void);
extern "C" BOOL WslMatchesGlobalSearch(PCWSTR text);

extern "C" void WslClearGlobalSearch(void);
