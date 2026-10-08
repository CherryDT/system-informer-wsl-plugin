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
#include <atomic>
#include <commctrl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <shellapi.h>
#include <string>
#include <vector>

namespace wsl
{
using Json = nlohmann::json;
std::wstring wide(const std::string &);
std::string utf8(const std::wstring &);
extern HINSTANCE instance;
extern HFONT font;
constexpr UINT ReplyMessage = WM_APP + 51;
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
void openDetails(HWND owner, const std::wstring &distro, const Json &process);
void openServiceDetails(HWND owner, const std::wstring &distro, const std::string &name);
void openLinuxPath(HWND owner, const std::wstring &distro, const std::wstring &path, bool select = true);
void copyText(HWND owner, const std::wstring &text);
void saveText(HWND owner, const std::wstring &text, const wchar_t *defaultName);
void errorBox(HWND owner, const std::wstring &text);
std::wstring text(const Json &value, const char *key, const std::wstring &fallback = L"");
std::wstring number(double value, int decimals = 1);
std::wstring bytes(uint64_t value);
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
};
struct Row
{
    std::vector<std::wstring> cells;
    Json data;
    std::string key;
};
class Table
{
  public:
    HWND window = nullptr;
    std::vector<Row> rows;
    std::vector<Column> columns;
    int sortColumn = -1;
    bool descending = false;
    void create(HWND parent, int id, std::vector<Column> definitions, int defaultSort = -1,
                bool defaultDescending = false);
    void saveLayout() const;
    void setAncestryOrder(bool enabled);
    void replace(std::vector<Row> next);
    void sort(int column);
    bool notify(NMHDR *hdr);
    LRESULT customDraw(NMLVCUSTOMDRAW *draw) const;
    const Row *selected() const;
    void selectKey(const std::string &key);
    std::wstring exportText() const;

  private:
    std::wstring settingsPrefix;
    bool ancestryOrder = false;
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
