#include "resource_dialog.hpp"
#include "host_bridge.h"
#include "process_rules.hpp"
#include <algorithm>
#include <commdlg.h>
#include <stdexcept>
#include <windowsx.h>

namespace wsl
{
namespace
{
constexpr wchar_t ToolClass[] = L"WslTools.ResourceTool";
constexpr size_t MaximumBinaryBytes = 16 * 1024 * 1024;
enum ControlId
{
    Run = 700,
    Copy,
    Save,
    Tabs,
    Properties,
    Details,
    Status,
    SaveScheduling,
    InputBase = 720
};
struct ToolWindow
{
    HWND window = nullptr, run = nullptr, copy = nullptr, save = nullptr, tabs = nullptr;
    HWND details = nullptr, status = nullptr, close = nullptr, saveScheduling = nullptr;
    HFONT uiFont = nullptr, textFont = nullptr;
    Table properties;
    std::vector<ResourceInput> inputs;
    std::vector<HWND> labels, edits;
    std::wstring distro, title, output, suggestedFilename;
    Json request, cachedResponse, submittedInputs;
    std::string schedulingExecutable, schedulingAction;
    std::wstring initialNotice;
    bool schedulingSaved = false, submittedSaveScheduling = false, submittedHadSavedScheduling = false;
    bool cached = false;
    std::vector<unsigned char> binary;
    bool mutation = false, busy = false, hasResult = false, hasBinary = false;
    uintptr_t generation = 0;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
};

std::wstring windowText(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, result.data(), static_cast<int>(result.size()));
    result.resize(length);
    return result;
}

std::wstring editText(const std::wstring &value)
{
    std::wstring result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == L'\n' && (i == 0 || value[i - 1] != L'\r'))
            result += L'\r';
        result += value[i];
    }
    return result;
}

LRESULT CALLBACK pageMessages(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                              UINT_PTR, DWORD_PTR owner)
{
    switch (message)
    {
    case WM_COMMAND: case WM_NOTIFY: case WM_CONTEXTMENU:
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT:
        return SendMessageW(reinterpret_cast<HWND>(owner), message, wparam, lparam);
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, pageMessages, 2);
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void layout(ToolWindow &state)
{
    RECT bounds{};
    GetClientRect(state.window, &bounds);
    const int gap = scale(state.window, 6), height = editHeight(state.window);
    const int button = scale(state.window, 78), label = scale(state.window, 142);
    int y = gap;
    for (size_t i = 0; i < state.inputs.size(); ++i)
    {
        place(state.labels[i], gap, y + scale(state.window, 3), label, height);
        place(state.edits[i], 2 * gap + label, y, bounds.right - label - 3 * gap, height);
        y += height + gap;
    }
    if (state.saveScheduling)
    {
        place(state.saveScheduling, gap, y, bounds.right - 2 * gap, height);
        y += height + gap;
    }
    int x = gap;
    for (HWND child : {state.run, state.copy, state.save})
    {
        if (state.cached && child == state.run)
            continue;
        place(child, x, y, button, height);
        x += button + gap;
    }
    y += height + gap;
    place(state.status, gap, bounds.bottom - height - gap, bounds.right - button - 3 * gap, height);
    place(state.close, bounds.right - button - gap, bounds.bottom - height - gap, button, height);
    place(state.tabs, gap, y, bounds.right - 2 * gap, bounds.bottom - y - height - 2 * gap);
    RECT content{};
    GetClientRect(state.tabs, &content);
    TabCtrl_AdjustRect(state.tabs, FALSE, &content);
    for (HWND child : {state.properties.window, state.details})
        place(child, content.left, content.top, content.right - content.left, content.bottom - content.top);
    const bool properties = TabCtrl_GetCurSel(state.tabs) == 0;
    ShowWindow(state.properties.window, properties ? SW_SHOW : SW_HIDE);
    ShowWindow(state.details, properties ? SW_HIDE : SW_SHOW);
}

void applyFonts(ToolWindow &state)
{
    const HFONT oldUi = state.uiFont, oldText = state.textFont;
    state.uiFont = WslCreateUiFont(state.window);
    state.textFont = WslCreateTextFont(state.window);
    const WPARAM ui =
        reinterpret_cast<WPARAM>(state.uiFont ? state.uiFont : GetStockObject(DEFAULT_GUI_FONT));
    for (HWND child :
         {state.run, state.copy, state.save, state.tabs, state.status, state.close, state.saveScheduling})
        SendMessageW(child, WM_SETFONT, ui, TRUE);
    for (HWND child : state.labels)
        SendMessageW(child, WM_SETFONT, ui, TRUE);
    for (HWND child : state.edits)
        SendMessageW(child, WM_SETFONT, ui, TRUE);
    SendMessageW(state.details, WM_SETFONT,
                 reinterpret_cast<WPARAM>(state.textFont ? state.textFont : GetStockObject(ANSI_FIXED_FONT)),
                 TRUE);
    SendMessageW(state.properties.window, WM_SETFONT, reinterpret_cast<WPARAM>(WslGetHostFont()), TRUE);
    if (oldUi)
        DeleteObject(oldUi);
    if (oldText)
        DeleteObject(oldText);
}

void begin(ToolWindow &state)
{
    if (state.busy || state.cached)
        return;
    Json request = state.request;
    for (size_t i = 0; i < state.inputs.size(); ++i)
        request["inputs"][state.inputs[i].key] = utf8(windowText(state.edits[i]));
    const bool saveSchedulingChoice =
        state.saveScheduling && SendMessageW(state.saveScheduling, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (state.mutation)
    {
        std::wstring target = state.title + L"\n\nApply this change to process " + text(request, "pid") +
                              L" in " + state.distro + L"?";
        for (size_t i = 0; i < state.inputs.size(); ++i)
            target += L"\n" + state.inputs[i].label + L": " + windowText(state.edits[i]).substr(0, 512);
        target += L"\n\nThis changes the running process and may disrupt its work.";
        const HWND window = state.window;
        const auto mailbox = state.mailbox;
        const auto context = reinterpret_cast<LONG_PTR>(&state);
        const int choice = MessageBoxW(window, target.c_str(), L"Confirm process change",
                                       MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
        // MessageBox runs a nested message loop: shutdown may destroy this tool
        // while confirmation is open. Its retained mailbox is our lifetime token.
        if (!IsWindow(window) || GetWindowLongPtrW(window, GWLP_USERDATA) != context ||
            mailbox->window.load() != window || choice != IDOK)
            return;
    }
    state.submittedInputs = request.value("inputs", Json::object());
    state.submittedSaveScheduling = saveSchedulingChoice;
    state.submittedHadSavedScheduling = state.schedulingSaved;
    state.busy = true;
    state.hasResult = state.hasBinary = false;
    state.output.clear();
    state.binary.clear();
    state.properties.clear();
    SetWindowTextW(state.details, L"Loading...");
    SetWindowTextW(state.status, state.mutation ? L"Applying change..." : L"Loading...");
    for (HWND child : {state.run, state.copy, state.save})
        EnableWindow(child, FALSE);
    for (HWND child : state.edits)
        EnableWindow(child, FALSE);
    if (state.saveScheduling)
        EnableWindow(state.saveScheduling, FALSE);
    submit(state.distro, std::move(request), state.mailbox, ++state.generation);
}

// Remote names are suggestions for a basename, never a local output path.
std::wstring safeFilename(std::wstring name, const wchar_t *fallback)
{
    const size_t slash = name.find_last_of(L"/\\");
    if (slash != std::wstring::npos)
        name.erase(0, slash + 1);
    for (wchar_t &c : name)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c))
            c = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
        name.pop_back();
    if (name.empty() || name.size() > 180)
        return fallback;
    std::wstring stem = name.substr(0, name.find(L'.'));
    CharUpperBuffW(stem.data(), static_cast<DWORD>(stem.size()));
    const bool numberedDevice = stem.size() == 4 &&
                                (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) &&
                                stem[3] >= L'1' && stem[3] <= L'9';
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || numberedDevice)
        name.insert(0, L"wsl-");
    return name;
}

void saveResult(ToolWindow &state)
{
    if (!state.hasResult)
        return;
    if (!state.hasBinary)
    {
        const auto name = safeFilename(state.suggestedFilename, L"wsl-resource.txt");
        saveText(state.window, state.properties.exportText() + L"\r\n" + state.output, name.c_str());
        return;
    }
    wchar_t path[32768]{};
    const auto name = safeFilename(state.suggestedFilename, L"wsl-memory.bin");
    wcsncpy_s(path, name.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter = L"Binary files\0*.bin\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"bin";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog))
        return;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        errorBox(state.window, L"Could not create the output file.");
        return;
    }
    DWORD written = 0;
    const bool ok =
        WriteFile(file, state.binary.data(), static_cast<DWORD>(state.binary.size()), &written, nullptr) &&
        written == state.binary.size();
    CloseHandle(file);
    if (!ok)
        errorBox(state.window, L"The output file could not be written completely.");
}

void loadReply(ToolWindow &state, const Reply &reply)
{
    if (reply.tag != state.generation)
        return;
    state.busy = false;
    EnableWindow(state.run, TRUE);
    for (HWND child : state.edits)
        EnableWindow(child, TRUE);
    if (state.saveScheduling)
        EnableWindow(state.saveScheduling, TRUE);
    try
    {
        if (!reply.error.empty())
            throw std::runtime_error(reply.error);
        std::vector<Row> rows;
        auto fields = reply.data.find("fields");
        if (fields != reply.data.end() && fields->is_array())
            for (const auto &field : *fields)
                rows.push_back(
                    {{text(field, "name"), text(field, "value")}, field, std::to_string(rows.size())});
        state.properties.replace(std::move(rows));
        state.output = text(reply.data, "text");
        state.suggestedFilename = text(reply.data, "suggested_filename");
        const auto hex = reply.data.find("data_hex");
        if (hex != reply.data.end() && hex->is_string())
        {
            const auto &encoded = hex->get_ref<const std::string &>();
            if (encoded.size() % 2 || encoded.size() / 2 > MaximumBinaryBytes)
                throw std::runtime_error("The binary response has an invalid or excessive length.");
            auto digit = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            state.binary.reserve(encoded.size() / 2);
            for (size_t i = 0; i < encoded.size(); i += 2)
            {
                const int high = digit(encoded[i]), low = digit(encoded[i + 1]);
                if (high < 0 || low < 0)
                    throw std::runtime_error("The binary response contains invalid hexadecimal data.");
                state.binary.push_back(static_cast<unsigned char>(high * 16 + low));
            }
            state.hasBinary = true;
        }
        state.hasResult = true;
        const bool truncated = reply.data.value("truncated", false);
        const bool incomplete = !reply.data.value("complete", true) || reply.data.value("failed", 0) != 0 ||
                                reply.data.value("not_attempted", 0) != 0;
        SetWindowTextW(state.status, truncated         ? L"Partial result: the collection limit was reached."
                                     : incomplete      ? L"The operation is incomplete. See Details."
                                     : state.hasBinary ? L"Ready. Save writes the captured binary data."
                                                       : L"Ready.");
        if (truncated)
            state.output += L"\r\n\r\nPartial result: the collection limit was reached.";
        if (state.properties.rows.empty())
            TabCtrl_SetCurSel(state.tabs, 1);
        if (state.mutation && (state.submittedSaveScheduling || state.submittedHadSavedScheduling) &&
            reply.data.value("failed", 0) == 0 && reply.data.value("complete", false) &&
            reply.data.value("not_attempted", 0) == 0 && reply.data.value("mutation_applied", false))
        {
            try
            {
                if (state.submittedSaveScheduling)
                {
                    saveScheduling(state.distro, state.schedulingExecutable, state.schedulingAction,
                                   state.submittedInputs);
                    state.schedulingSaved = true;
                    SetWindowTextW(state.status, L"Applied and saved for this executable.");
                }
                else
                {
                    // Unchecking this action must not erase affinity, priority,
                    // or other scheduling choices saved through another tool.
                    removeSavedScheduling(state.distro, state.schedulingExecutable, state.schedulingAction);
                    state.schedulingSaved = false;
                    SetWindowTextW(state.status, L"Applied. The saved setting for this action was removed.");
                }
            }
            catch (const std::exception &error)
            {
                const std::wstring notice =
                    L"The change was applied, but its saved settings could not be updated: " +
                    wide(error.what());
                state.output += L"\r\n\r\n" + notice;
                SetWindowTextW(state.status, notice.c_str());
            }
        }
        if (state.mutation)
        {
            // Refresh the inspector after the helper confirms the change. Posting
            // keeps its refresh out of this reply handler and its modal children.
            HWND owner = GetWindow(state.window, GW_OWNER);
            if (owner && IsWindow(owner))
                PostMessageW(owner, ResourceActionCompleted, 0, 0);
        }
    }
    catch (const std::exception &error)
    {
        state.binary.clear();
        state.hasBinary = false;
        state.output = L"Could not complete the operation: " + wide(error.what());
        state.hasResult = true; // Diagnostics remain copyable and saveable.
        SetWindowTextW(state.status, L"The operation failed. See Details.");
        TabCtrl_SetCurSel(state.tabs, 1);
    }
    state.output = editText(state.output);
    SetWindowTextW(state.details, state.output.empty() ? L"No additional details." : state.output.c_str());
    EnableWindow(state.copy, state.hasResult);
    EnableWindow(state.save, state.hasResult);
    layout(state);
}

void copyResult(ToolWindow &state)
{
    if (TabCtrl_GetCurSel(state.tabs) == 0)
    {
        if (const Row *row = state.properties.selected())
        {
            copyText(state.window, row->cells[0] + L"\t" + row->cells[1]);
            return;
        }
        copyText(state.window, state.properties.exportText());
    }
    else
    {
        DWORD first = 0, last = 0;
        SendMessageW(state.details, EM_GETSEL, reinterpret_cast<WPARAM>(&first),
                     reinterpret_cast<LPARAM>(&last));
        const auto displayed = windowText(state.details);
        copyText(state.window, first != last ? displayed.substr(first, last - first) : displayed);
    }
}

LRESULT CALLBACK shortcuts(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                           DWORD_PTR context)
{
    HWND owner = reinterpret_cast<HWND>(context);
    if (message == WM_KEYDOWN)
    {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const int controlId = GetDlgCtrlID(window);
        if (ctrl && wParam == 'A' && (controlId == Details || controlId >= InputBase))
        {
            SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wParam == VK_RETURN &&
            ((controlId >= Run && controlId <= Save) || controlId == IDCANCEL || controlId >= InputBase))
        {
            PostMessageW(owner, WM_COMMAND, controlId >= InputBase ? Run : controlId, 0);
            return 0;
        }
        if (wParam == VK_ESCAPE || (ctrl && (wParam == 'R' || wParam == 'S')))
        {
            PostMessageW(owner, WM_COMMAND, wParam == VK_ESCAPE ? IDCANCEL : wParam == 'R' ? Run : Save, 0);
            return 0;
        }
        if (ctrl && wParam == 'C' && GetDlgCtrlID(window) == Properties)
        {
            PostMessageW(owner, WM_COMMAND, Copy, 0);
            return 0;
        }
        if (wParam == VK_TAB && !ctrl)
        {
            HWND next = GetNextDlgTabItem(owner, window, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            if (next)
                SetFocus(next);
            return 0;
        }
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, shortcuts, id);
    return DefSubclassProc(window, message, wParam, lParam);
}

BOOL CALLBACK installShortcuts(HWND child, LPARAM parent)
{
    SetWindowSubclass(child, shortcuts, 9, static_cast<DWORD_PTR>(parent));
    return TRUE;
}

LRESULT CALLBACK toolProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto state = reinterpret_cast<ToolWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        auto incoming = static_cast<std::unique_ptr<ToolWindow> *>(
            reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
        state = incoming->release();
        state->window = window;
        state->mailbox->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, wParam, lParam);
    switch (message)
    {
    case WM_CREATE: {
        for (size_t i = 0; i < state->inputs.size(); ++i)
        {
            state->labels.push_back(
                control(window, WC_STATICW, state->inputs[i].label.c_str(), SS_NOPREFIX, 0));
            state->edits.push_back(control(window, WC_EDITW, state->inputs[i].value.c_str(),
                                           WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                           InputBase + static_cast<int>(i)));
            SendMessageW(state->edits.back(), EM_SETLIMITTEXT, 65536, 0);
        }
        if (!state->schedulingExecutable.empty())
        {
            state->saveScheduling = control(window, WC_BUTTONW, L"Save for this executable",
                                            WS_TABSTOP | BS_AUTOCHECKBOX, SaveScheduling);
            SendMessageW(state->saveScheduling, BM_SETCHECK,
                         state->schedulingSaved ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        state->run = control(window, WC_BUTTONW,
                             state->mutation         ? L"Apply"
                             : state->inputs.empty() ? L"Refresh"
                                                     : L"Run",
                             WS_TABSTOP | BS_PUSHBUTTON, Run);
        state->copy = control(window, WC_BUTTONW, L"Copy", WS_TABSTOP | BS_PUSHBUTTON, Copy);
        state->save = control(window, WC_BUTTONW, L"Save...", WS_TABSTOP | BS_PUSHBUTTON, Save);
        state->tabs = control(window, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, Tabs);
        SetWindowLongPtrW(state->tabs, GWL_EXSTYLE, WS_EX_CONTROLPARENT);
        SetWindowSubclass(state->tabs, pageMessages, 2, reinterpret_cast<DWORD_PTR>(window));
        for (const wchar_t *name : {L"Properties", L"Details"})
        {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t *>(name);
            TabCtrl_InsertItem(state->tabs, TabCtrl_GetItemCount(state->tabs), &item);
        }
        state->properties.create(state->tabs, Properties, {{L"Field", 210}, {L"Value", 520}});
        SetWindowLongPtrW(state->properties.window, GWL_STYLE,
                          GetWindowLongPtrW(state->properties.window, GWL_STYLE) | WS_BORDER |
                              WS_CLIPSIBLINGS);
        state->details = control(state->tabs, WC_EDITW, L"Press \"Run\" to collect details.",
                                 WS_TABSTOP | WS_BORDER | WS_CLIPSIBLINGS | WS_VSCROLL | WS_HSCROLL |
                                     ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                 Details);
        SendMessageW(state->details, EM_SETLIMITTEXT, 32 * 1024 * 1024, 0);
        state->status = control(window, WC_STATICW,
                                state->mutation ? L"Review the values, then press \"Apply\"." : L"Ready.",
                                SS_NOPREFIX, Status);
        if (!state->initialNotice.empty())
            SetWindowTextW(state->status, state->initialNotice.c_str());
        state->close = control(window, WC_BUTTONW, L"Close", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
        EnableWindow(state->copy, FALSE);
        EnableWindow(state->save, FALSE);
        WslApplyTheme(window);
        applyFonts(*state);
        EnumChildWindows(window, installShortcuts, reinterpret_cast<LPARAM>(window));
        layout(*state);
        if (state->cached)
        {
            ShowWindow(state->run, SW_HIDE);
            Reply reply;
            reply.data = std::move(state->cachedResponse);
            loadReply(*state, reply);
        }
        else if (state->inputs.empty() && !state->mutation)
            PostMessageW(window, WM_COMMAND, Run, 0);
        return 0;
    }
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->uiFont);
    case WM_SIZE:
        layout(*state);
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO *>(lParam)->ptMinTrackSize = {
            scale(window, 620), scale(window, 390 + static_cast<int>(state->inputs.size()) * 28 +
                                                  (state->saveScheduling ? 28 : 0))};
        return 0;
    case WM_DPICHANGED: {
        const RECT *rect = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        applyFonts(*state);
        layout(*state);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        HBRUSH brush = CreateSolidBrush(WslDialogBackground());
        FillRect(reinterpret_cast<HDC>(wParam), &bounds, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        const HWND child = reinterpret_cast<HWND>(lParam);
        const bool edit = child == state->details ||
                          std::find(state->edits.begin(), state->edits.end(), child) != state->edits.end();
        const COLORREF background = edit && !WslIsDarkTheme() ? RGB(255, 255, 255) : WslDialogBackground();
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, WslDialogText());
        SetBkColor(dc, background);
        SetDCBrushColor(dc, background);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == state->properties.window)
        {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (point.x == -1 && point.y == -1)
            {
                const int selected = ListView_GetNextItem(state->properties.window, -1, LVNI_SELECTED);
                RECT row{};
                if (selected >= 0)
                    ListView_GetItemRect(state->properties.window, selected, &row, LVIR_BOUNDS);
                point = {row.left + scale(window, 12), row.bottom};
                ClientToScreen(state->properties.window, &point);
            }
            HMENU menu = CreatePopupMenu();
            const Row *row = state->properties.selected();
            AppendMenuW(menu, MF_STRING | (row ? 0 : MF_GRAYED), 1, L"Copy");
            AppendMenuW(menu, MF_STRING | (row ? 0 : MF_GRAYED), 2, L"Copy value");
            AppendMenuW(menu, MF_STRING, 3, L"Copy all");
            const UINT choice =
                TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
            DestroyMenu(menu);
            // Menus run a nested message loop. Resolve the row again afterward.
            row = state->properties.selected();
            if (choice == 1)
                copyResult(*state);
            else if (choice == 2 && row && row->cells.size() > 1)
                copyText(window, row->cells[1]);
            else if (choice == 3)
                copyText(window, state->properties.exportText());
            return 0;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case Run:
            begin(*state);
            break;
        case Copy:
            if (state->hasResult)
                copyResult(*state);
            break;
        case Save:
            saveResult(*state);
            break;
        case IDCANCEL:
            DestroyWindow(window);
            break;
        }
        return 0;
    case WM_NOTIFY: {
        const auto hdr = reinterpret_cast<NMHDR *>(lParam);
        if (hdr->hwndFrom == state->tabs && hdr->code == TCN_SELCHANGE)
            layout(*state);
        if (hdr->hwndFrom == state->properties.window)
        {
            if (hdr->code == NM_CUSTOMDRAW)
                return state->properties.customDraw(reinterpret_cast<NMLVCUSTOMDRAW *>(hdr));
            if (hdr->code == LVN_ODFINDITEMW)
                return state->properties.findItem(*reinterpret_cast<NMLVFINDITEMW *>(hdr));
            state->properties.notify(hdr);
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
        state->properties.saveLayout();
        state->mailbox->detach();
        drainReplies(window);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (state->uiFont)
            DeleteObject(state->uiFont);
        if (state->textFont)
            DeleteObject(state->textFont);
        delete state;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
void showTool(HWND owner, std::unique_ptr<ToolWindow> state)
{
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance;
    cls.lpfnWndProc = toolProc;
    cls.lpszClassName = ToolClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        errorBox(owner, L"Could not register the resource tool window.");
        return;
    }
    const bool hasInputs = !state->inputs.empty();
    const bool cached = state->cached;
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, ToolClass, state->title.c_str(),
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  scale(owner, 820), scale(owner, 560), owner, nullptr, instance, &state);
    if (!window)
    {
        errorBox(owner, L"Could not create the resource tool window.");
        return;
    }
    WslPositionDialog(window, owner);
    ShowWindow(window, SW_SHOW);
    SetFocus(GetDlgItem(window, hasInputs ? InputBase : cached ? Tabs : Run));
}
} // namespace

void openResourceTool(HWND owner, const std::wstring &distro, Json request, const std::wstring &title,
                      std::vector<ResourceInput> inputs, bool mutation)
{
    auto state = std::make_unique<ToolWindow>();
    state->distro = distro;
    state->request = std::move(request);
    state->title = title;
    state->inputs = std::move(inputs);
    state->mutation = mutation;
    if (mutation && state->request.value("all_threads", false))
    {
        state->schedulingExecutable = state->request.value("expected_exe", std::string{});
        state->schedulingAction = state->request.value("action", std::string{});
        if (!state->schedulingExecutable.empty())
        {
            try
            {
                const Json saved =
                    savedSchedulingInputs(distro, state->schedulingExecutable, state->schedulingAction);
                state->schedulingSaved = !saved.is_null();
                if (saved.is_object())
                    for (auto &input : state->inputs)
                        if (saved.contains(input.key))
                            input.value = text(saved, input.key.c_str());
            }
            catch (const std::exception &error)
            {
                state->initialNotice = L"Could not load saved scheduling settings: " + wide(error.what());
            }
        }
    }
    showTool(owner, std::move(state));
}

void openResourceReport(HWND owner, const std::wstring &title, Json response)
{
    auto state = std::make_unique<ToolWindow>();
    state->title = title;
    state->cached = true;
    state->cachedResponse = std::move(response);
    showTool(owner, std::move(state));
}
} // namespace wsl
