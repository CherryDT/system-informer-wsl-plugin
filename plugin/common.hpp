#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include "../vendor/json.hpp"

namespace wsl {
using Json = nlohmann::json;
std::wstring wide(const std::string&);
std::string utf8(const std::wstring&);
extern HINSTANCE instance;
extern HFONT font;
constexpr UINT ReplyMessage = WM_APP + 51;
struct Reply { Json data; std::string error; uintptr_t tag = 0; };
// A closed window detaches its mailbox. The controller owns no UI objects.
struct Mailbox {
    std::atomic<HWND> window{nullptr};
    std::mutex gate;

    void detach() {
        std::lock_guard<std::mutex> lock(gate);
        window = nullptr;
    }
};
void submit(const std::wstring& distro, Json request, std::shared_ptr<Mailbox> mailbox, uintptr_t tag);
void openDetails(HWND owner, const std::wstring& distro, const Json& process);
void openServiceDetails(HWND owner, const std::wstring& distro, const std::string& name);
void openLinuxPath(HWND owner, const std::wstring& distro, const std::wstring& path, bool select = true);
void copyText(HWND owner, const std::wstring& text);
void saveText(HWND owner, const std::wstring& text, const wchar_t* defaultName);
void errorBox(HWND owner, const std::wstring& text);
std::wstring text(const Json& value, const char* key, const std::wstring& fallback = L"");
std::wstring number(double value, int decimals = 1);
std::wstring bytes(uint64_t value);
HWND control(HWND parent, const wchar_t* cls, const wchar_t* label, DWORD style, int id);
void place(HWND child, int x, int y, int width, int height);
int scale(HWND window, int value);
void drainReplies(HWND window);

struct Column { std::wstring title; int width; bool numeric = false; };
struct Row { std::vector<std::wstring> cells; Json data; std::string key; };
class Table {
public:
    HWND window = nullptr;
    std::vector<Row> rows;
    std::vector<Column> columns;
    int sortColumn = -1;
    bool descending = false;
    void create(HWND parent, int id, std::vector<Column> definitions);
    void replace(std::vector<Row> next);
    void sort(int column);
    bool notify(NMHDR* hdr);
    const Row* selected() const;
    void selectKey(const std::string& key);
    std::wstring exportText() const;
private:
    void order();
};
}
extern "C" void WslApplyTheme(HWND);
extern "C" HFONT WslGetHostFont(void);

extern "C" BOOL WslIsDarkTheme(void);
