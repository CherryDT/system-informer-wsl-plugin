#include "resource_dialog.hpp"
#include "host_bridge.h"
#include "process_rules.hpp"
#include <algorithm>
#include <commdlg.h>
#include <stdexcept>
#include <set>
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
    CpuAll,
    CpuNone,
    CpuPage,
    CpuBase = 800,
    InputBase = 720
};
struct ToolWindow
{
    HWND window = nullptr, run = nullptr, copy = nullptr, save = nullptr, tabs = nullptr;
    HWND details = nullptr, status = nullptr, close = nullptr, saveScheduling = nullptr, tooltips = nullptr;
    HFONT uiFont = nullptr, textFont = nullptr;
    Table properties;
    std::vector<ResourceInput> inputs;
    std::vector<HWND> labels, edits, spinners, cpuChecks;
    HWND cpuLabel = nullptr, cpuAll = nullptr, cpuNone = nullptr, cpuPage = nullptr;
    std::vector<int> onlineCpus;
    std::set<int> selectedCpus, mixedCpus;
    bool affinity = false, affinityReady = false, loadingAffinity = false;
    size_t cpuPageIndex = 0;
    std::wstring distro, title, output, suggestedFilename;
    Json request, cachedResponse, submittedInputs;
    std::string schedulingExecutable, schedulingAction;
    std::wstring initialNotice, readyStatus, statusText;
    ResourceView view = ResourceView::Both;
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

LRESULT CALLBACK pageMessages(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                              DWORD_PTR owner)
{
    switch (message)
    {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_CONTEXTMENU:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return SendMessageW(reinterpret_cast<HWND>(owner), message, wparam, lparam);
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, pageMessages, 2);
        break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

constexpr size_t CpuPageSize = 64;
using InputChoices = std::vector<std::pair<const wchar_t *, const char *>>;
const InputChoices &choicesFor(const std::string &key)
{
    static const InputChoices policy{{L"Normal", "other"}, {L"Batch", "batch"}, {L"Idle", "idle"}};
    static const InputChoices ioClass{
        {L"Default", "none"}, {L"Best effort", "best-effort"}, {L"Idle", "idle"}};
    static const InputChoices none;
    return key == "policy" ? policy : key == "class" ? ioClass : none;
}

void updateInputs(ToolWindow &state)
{
    bool bestEffort = true;
    for (size_t i = 0; i < state.inputs.size() && i < state.edits.size(); ++i)
        if (state.inputs[i].key == "class")
            bestEffort = SendMessageW(state.edits[i], CB_GETCURSEL, 0, 0) == 1;
    for (size_t i = 0; i < state.edits.size(); ++i)
    {
        const bool enabled = !state.busy && (state.inputs[i].key != "level" || bestEffort);
        EnableWindow(state.edits[i], enabled);
        if (state.spinners[i])
            EnableWindow(state.spinners[i], enabled);
        EnableWindow(state.labels[i], enabled);
    }
    for (HWND child : state.cpuChecks)
        EnableWindow(child, state.affinityReady && !state.busy);
    for (HWND child : {state.cpuAll, state.cpuNone, state.cpuPage})
        if (child)
            EnableWindow(child, state.affinityReady && !state.busy);
    if (state.affinity)
        EnableWindow(state.run, state.affinityReady && !state.busy);
}

void showCpuPage(ToolWindow &state)
{
    const size_t first = state.cpuPageIndex * CpuPageSize;
    for (size_t i = 0; i < state.cpuChecks.size(); ++i)
    {
        const size_t index = first + i;
        HWND check = state.cpuChecks[i];
        if (index < state.onlineCpus.size())
        {
            const int cpu = state.onlineCpus[index];
            SetWindowTextW(check, (L"CPU " + std::to_wstring(cpu)).c_str());
            SendMessageW(check, BM_SETCHECK,
                         state.mixedCpus.count(cpu)      ? BST_INDETERMINATE
                         : state.selectedCpus.count(cpu) ? BST_CHECKED
                                                         : BST_UNCHECKED,
                         0);
            ShowWindow(check, SW_SHOW);
        }
        else
            ShowWindow(check, SW_HIDE);
    }
}

std::set<int> parseCpuList(const std::wstring &text)
{
    std::set<int> cpus;
    size_t position = 0;
    auto number = [&]() {
        if (position >= text.size() || text[position] < L'0' || text[position] > L'9')
            throw std::runtime_error("The saved CPU list is invalid.");
        int value = 0;
        while (position < text.size() && text[position] >= L'0' && text[position] <= L'9')
        {
            value = value * 10 + (text[position++] - L'0');
            if (value >= 8192)
                throw std::runtime_error("The saved CPU list contains an unsupported CPU number.");
        }
        return value;
    };
    while (position < text.size())
    {
        const int first = number();
        int last = first;
        if (position < text.size() && text[position] == L'-')
        {
            ++position;
            last = number();
        }
        if (last < first)
            throw std::runtime_error("The saved CPU list contains an invalid range.");
        for (int cpu = first; cpu <= last; ++cpu)
            cpus.insert(cpu);
        if (position == text.size())
            break;
        if (text[position++] != L',' || position == text.size())
            throw std::runtime_error("The saved CPU list is invalid.");
    }
    if (cpus.empty())
        throw std::runtime_error("The saved CPU list is empty.");
    return cpus;
}

bool showingProperties(const ToolWindow &state)
{
    return state.view == ResourceView::PropertiesOnly || (state.tabs && TabCtrl_GetCurSel(state.tabs) == 0);
}

void setStatus(ToolWindow &state, const std::wstring &value)
{
    state.statusText = value;
    if (state.status)
        SetWindowTextW(state.status, value.c_str());
}

void layout(ToolWindow &state)
{
    RECT bounds{};
    GetClientRect(state.window, &bounds);
    const int gap = scale(state.window, 6), height = editHeight(state.window);
    const int button = scale(state.window, 78);
    int label = scale(state.window, 142);
    HDC dc = GetDC(state.window);
    if (dc)
    {
        HGDIOBJ previous = SelectObject(dc, state.uiFont ? state.uiFont : GetStockObject(DEFAULT_GUI_FONT));
        for (const auto &input : state.inputs)
        {
            SIZE extent{};
            GetTextExtentPoint32W(dc, input.label.c_str(), static_cast<int>(input.label.size()), &extent);
            label = std::max(label, static_cast<int>(extent.cx) + gap);
        }
        SelectObject(dc, previous);
        ReleaseDC(state.window, dc);
    }
    label = std::min(label, static_cast<int>(bounds.right) - scale(state.window, 180));
    int y = gap;
    for (size_t i = 0; i < state.edits.size(); ++i)
    {
        place(state.labels[i], gap, y + scale(state.window, 3), label, height);
        const bool combo = state.mutation && !choicesFor(state.inputs[i].key).empty();
        const int spinWidth = state.spinners[i] ? scale(state.window, 18) : 0;
        place(state.edits[i], 2 * gap + label, y, bounds.right - label - 3 * gap - spinWidth,
              combo ? height * 6 : height);
        if (state.spinners[i])
            place(state.spinners[i], bounds.right - gap - spinWidth, y, spinWidth, height);
        y += height + gap;
    }
    if (state.affinity)
    {
        place(state.cpuLabel, gap, y + scale(state.window, 3), bounds.right - 3 * button - 5 * gap, height);
        place(state.cpuPage, bounds.right - 3 * button - 3 * gap, y, button, height * 8);
        place(state.cpuAll, bounds.right - 2 * button - 2 * gap, y, button, height);
        place(state.cpuNone, bounds.right - button - gap, y, button, height);
        y += height + gap;
        const int columns = 8, width = (bounds.right - 2 * gap) / columns;
        for (size_t i = 0; i < state.cpuChecks.size(); ++i)
            place(state.cpuChecks[i], gap + static_cast<int>(i % columns) * width,
                  y + static_cast<int>(i / columns) * (height + gap), width, height);
        const size_t count = std::min(CpuPageSize, state.onlineCpus.size());
        y += static_cast<int>((count + columns - 1) / columns) * (height + gap);
    }
    if (state.saveScheduling)
    {
        place(state.saveScheduling, gap, y, bounds.right - 2 * gap, height);
        y += height + gap;
    }
    if (state.mutation)
    {
        const int bottom = bounds.bottom - height - gap;
        place(state.run, bounds.right - 2 * button - 2 * gap, bottom, button, height);
        place(state.close, bounds.right - button - gap, bottom, button, height);
        return;
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
    RECT content{gap, y, bounds.right - gap, bounds.bottom - height - 2 * gap};
    if (state.tabs)
    {
        place(state.tabs, content.left, content.top, content.right - content.left,
              content.bottom - content.top);
        GetClientRect(state.tabs, &content);
        TabCtrl_AdjustRect(state.tabs, FALSE, &content);
    }
    for (HWND child : {state.properties.window, state.details})
        if (child)
            place(child, content.left, content.top, content.right - content.left,
                  content.bottom - content.top);
    const bool properties = showingProperties(state);
    if (state.properties.window)
        ShowWindow(state.properties.window, properties ? SW_SHOW : SW_HIDE);
    if (state.details)
        ShowWindow(state.details, properties ? SW_HIDE : SW_SHOW);
}

void applyFonts(ToolWindow &state)
{
    const HFONT oldUi = state.uiFont, oldText = state.textFont;
    state.uiFont = WslCreateUiFont(state.window);
    state.textFont = state.details ? WslCreateTextFont(state.window) : nullptr;
    const WPARAM ui =
        reinterpret_cast<WPARAM>(state.uiFont ? state.uiFont : GetStockObject(DEFAULT_GUI_FONT));
    for (HWND child : {state.run, state.copy, state.save, state.tabs, state.status, state.close,
                       state.saveScheduling, state.cpuLabel, state.cpuAll, state.cpuNone, state.cpuPage})
        if (child)
            SendMessageW(child, WM_SETFONT, ui, TRUE);
    for (HWND child : state.cpuChecks)
        SendMessageW(child, WM_SETFONT, ui, TRUE);
    for (HWND child : state.labels)
        SendMessageW(child, WM_SETFONT, ui, TRUE);
    for (HWND child : state.edits)
        SendMessageW(child, WM_SETFONT, ui, TRUE);
    if (state.details)
        SendMessageW(
            state.details, WM_SETFONT,
            reinterpret_cast<WPARAM>(state.textFont ? state.textFont : GetStockObject(ANSI_FIXED_FONT)),
            TRUE);
    if (state.properties.window)
        SendMessageW(state.properties.window, WM_SETFONT, reinterpret_cast<WPARAM>(WslGetHostFont()), TRUE);
    if (oldUi)
        DeleteObject(oldUi);
    if (oldText)
        DeleteObject(oldText);
}

void loadAffinity(ToolWindow &state, const Reply &reply)
{
    state.loadingAffinity = false;
    state.busy = false;
    std::wstring notice;
    try
    {
        if (!reply.error.empty())
            throw std::runtime_error(reply.error);
        state.onlineCpus = reply.data.at("online_cpus").get<std::vector<int>>();
        std::sort(state.onlineCpus.begin(), state.onlineCpus.end());
        state.onlineCpus.erase(std::unique(state.onlineCpus.begin(), state.onlineCpus.end()),
                               state.onlineCpus.end());
        if (state.onlineCpus.empty() || state.onlineCpus.size() > 8192 || state.onlineCpus.front() < 0 ||
            state.onlineCpus.back() >= 8192)
            throw std::runtime_error("The available CPU list is empty or unsupported.");
        for (const int cpu : reply.data.at("affinity_cpus").get<std::vector<int>>())
            if (std::binary_search(state.onlineCpus.begin(), state.onlineCpus.end(), cpu))
                state.selectedCpus.insert(cpu);
        for (const int cpu : reply.data.value("mixed_cpus", std::vector<int>{}))
            if (std::binary_search(state.onlineCpus.begin(), state.onlineCpus.end(), cpu))
                state.mixedCpus.insert(cpu);
        if (state.schedulingSaved && !state.inputs.empty())
        {
            try
            {
                auto saved = parseCpuList(state.inputs.front().value);
                for (const int cpu : saved)
                    if (!std::binary_search(state.onlineCpus.begin(), state.onlineCpus.end(), cpu))
                        throw std::runtime_error("Saved affinity includes CPU " + std::to_string(cpu) +
                                                 ", which is not online.");
                state.selectedCpus = std::move(saved);
                state.mixedCpus.clear();
            }
            catch (const std::exception &error)
            {
                notice = wide(error.what()) +
                         L"\n\nThe current affinity is shown. Choose the CPUs to use before saving.";
            }
        }
        const size_t pages = (state.onlineCpus.size() + CpuPageSize - 1) / CpuPageSize;
        for (size_t page = 0; page < pages; ++page)
        {
            const auto label = L"Page " + std::to_wstring(page + 1);
            SendMessageW(state.cpuPage, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(state.cpuPage, CB_SETCURSEL, 0, 0);
        ShowWindow(state.cpuPage, pages > 1 ? SW_SHOW : SW_HIDE);
        state.affinityReady = true;
        SetWindowTextW(state.cpuLabel, L"CPUs");
        showCpuPage(state);
        RECT window{};
        GetWindowRect(state.window, &window);
        const int rows = static_cast<int>((std::min(CpuPageSize, state.onlineCpus.size()) + 7) / 8);
        SetWindowPos(state.window, nullptr, 0, 0, window.right - window.left,
                     scale(state.window, 115 + rows * 28 + (state.saveScheduling ? 28 : 0)),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    catch (const std::exception &error)
    {
        notice = L"Could not load CPU affinity: " + wide(error.what());
        SetWindowTextW(state.cpuLabel, L"CPU affinity unavailable");
    }
    updateInputs(state);
    if (state.saveScheduling)
        EnableWindow(state.saveScheduling, state.affinityReady);
    layout(state);
    if (!notice.empty())
        errorBox(state.window, notice); // Do not touch state after this nested message loop.
}

void requestAffinity(ToolWindow &state)
{
    state.loadingAffinity = state.busy = true;
    Json request = state.request;
    request["action"] = "settings";
    request.erase("inputs");
    updateInputs(state);
    if (state.saveScheduling)
        EnableWindow(state.saveScheduling, FALSE);
    submit(state.distro, std::move(request), state.mailbox, ++state.generation);
}

void begin(ToolWindow &state)
{
    if (state.busy || state.cached)
        return;
    Json request = state.request;
    try
    {
        if (state.affinity)
        {
            if (!state.affinityReady)
                return;
            if (!state.mixedCpus.empty())
                throw std::runtime_error("Choose whether to include each CPU marked as mixed.");
            if (state.selectedCpus.empty())
                throw std::runtime_error("Select at least one CPU.");
            std::string value;
            for (auto cpu = state.selectedCpus.begin(); cpu != state.selectedCpus.end();)
            {
                const int first = *cpu;
                int last = first;
                while (++cpu != state.selectedCpus.end() && *cpu == last + 1)
                    last = *cpu;
                if (!value.empty())
                    value += ',';
                value += std::to_string(first);
                if (last != first)
                    value += "-" + std::to_string(last);
            }
            request["inputs"]["cpus"] = value;
        }
        else
            for (size_t i = 0; i < state.inputs.size(); ++i)
            {
                const auto &input = state.inputs[i];
                const auto &choices = state.mutation ? choicesFor(input.key) : choicesFor("");
                std::string value = utf8(windowText(state.edits[i]));
                if (!choices.empty())
                {
                    const LRESULT selected = SendMessageW(state.edits[i], CB_GETCURSEL, 0, 0);
                    if (selected < 0 || static_cast<size_t>(selected) >= choices.size())
                        throw std::runtime_error("Select a value for " + utf8(input.label) + ".");
                    value = choices[selected].second;
                }
                if (state.mutation && (input.key == "nice" || input.key == "level"))
                {
                    if (!IsWindowEnabled(state.edits[i]))
                        value = "0";
                    size_t end = 0;
                    int numeric = 0;
                    try
                    {
                        numeric = std::stoi(value, &end);
                    }
                    catch (...)
                    {
                        throw std::runtime_error("Enter a valid number for " + utf8(input.label) + ".");
                    }
                    if (end != value.size() || numeric < (input.key == "nice" ? -20 : 0) ||
                        numeric > (input.key == "nice" ? 19 : 7))
                        throw std::runtime_error("The value for " + utf8(input.label) + " is out of range.");
                }
                request["inputs"][input.key] = value;
            }
    }
    catch (const std::exception &error)
    {
        errorBox(state.window, wide(error.what()));
        return;
    }
    const bool saveSchedulingChoice =
        state.saveScheduling && SendMessageW(state.saveScheduling, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state.submittedInputs = request.value("inputs", Json::object());
    state.submittedSaveScheduling = saveSchedulingChoice;
    state.submittedHadSavedScheduling = state.schedulingSaved;
    state.busy = true;
    state.hasResult = state.hasBinary = false;
    state.output.clear();
    state.binary.clear();
    if (state.properties.window)
        state.properties.clear();
    if (state.details)
        SetWindowTextW(state.details, L"Loading...");
    if (!state.mutation)
        setStatus(state, L"Loading...");
    for (HWND child : {state.run, state.copy, state.save})
        EnableWindow(child, FALSE);
    updateInputs(state);
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
        const auto properties = state.properties.window ? state.properties.exportText() + L"\r\n" : L"";
        saveText(state.window, properties + state.output, name.c_str());
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

// A scheduling form has no result pages: keep it open on failure so the
// entered values can be corrected, and close only after applying and saving.
void finishMutation(ToolWindow &state, const Reply &reply)
{
    const bool applied = reply.data.is_object() && reply.data.value("mutation_applied", false);
    if (applied)
    {
        HWND owner = GetWindow(state.window, GW_OWNER);
        if (owner && IsWindow(owner))
            PostMessageW(owner, ResourceActionCompleted, 0, 0);
    }
    try
    {
        if (!reply.error.empty())
            throw std::runtime_error(reply.error);
        if (!applied || !reply.data.value("complete", false) || reply.data.value("failed", 0) != 0 ||
            reply.data.value("not_attempted", 0) != 0)
        {
            std::wstring diagnostic = text(reply.data, "text");
            if (diagnostic.empty())
                diagnostic =
                    applied ? L"The change was only partially applied." : L"The change could not be applied.";
            errorBox(state.window, diagnostic);
            return;
        }
        if (state.submittedSaveScheduling)
        {
            saveScheduling(state.distro, state.schedulingExecutable, state.schedulingAction,
                           state.submittedInputs);
            state.schedulingSaved = true;
        }
        else if (state.submittedHadSavedScheduling)
        {
            // Remove only this action, preserving other saved scheduling choices.
            removeSavedScheduling(state.distro, state.schedulingExecutable, state.schedulingAction);
            state.schedulingSaved = false;
        }
    }
    catch (const std::exception &error)
    {
        errorBox(state.window, (applied ? L"The change was applied, but could not be saved: "
                                        : L"Could not apply the change: ") +
                                   wide(error.what()));
        return;
    }
    DestroyWindow(state.window);
}

void loadReply(ToolWindow &state, const Reply &reply)
{
    if (reply.tag != state.generation)
        return;
    if (state.loadingAffinity)
    {
        loadAffinity(state, reply);
        return;
    }
    state.busy = false;
    EnableWindow(state.run, TRUE);
    updateInputs(state);
    if (state.saveScheduling)
        EnableWindow(state.saveScheduling, TRUE);
    if (state.mutation)
    {
        finishMutation(state, reply);
        return;
    }
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
        if (state.properties.window)
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
        setStatus(state, truncated                    ? L"Partial result: the collection limit was reached."
                         : incomplete                 ? L"The operation is incomplete."
                         : state.hasBinary            ? L"Ready. Save writes the captured binary data."
                         : !state.readyStatus.empty() ? state.readyStatus
                                                      : L"Ready.");
        if (truncated)
            state.output += L"\r\n\r\nPartial result: the collection limit was reached.";
        if (state.tabs && state.properties.rows.empty())
            TabCtrl_SetCurSel(state.tabs, 1);
    }
    catch (const std::exception &error)
    {
        state.binary.clear();
        state.hasBinary = false;
        state.output = L"Could not complete the operation: " + wide(error.what());
        state.hasResult = true; // Diagnostics remain copyable and saveable.
        setStatus(state, L"The operation failed.");
        if (state.tabs)
            TabCtrl_SetCurSel(state.tabs, 1);
        else if (!state.details)
        {
            errorBox(state.window, state.output);
            return; // The message box may have destroyed its owner during shutdown.
        }
    }
    state.output = editText(state.output);
    if (state.details)
        SetWindowTextW(state.details,
                       state.output.empty() ? L"No additional details." : state.output.c_str());
    EnableWindow(state.copy, state.hasResult);
    EnableWindow(state.save, state.hasResult);
    layout(state);
}

void copyResult(ToolWindow &state)
{
    if (showingProperties(state))
    {
        if (state.properties.selected())
        {
            copyText(state.window, state.properties.selectedText());
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
        const auto state = reinterpret_cast<ToolWindow *>(GetWindowLongPtrW(owner, GWLP_USERDATA));
        const bool mutation = state && state->mutation;
        const bool combo =
            controlId == CpuPage || (state && controlId >= InputBase && controlId < CpuBase &&
                                     static_cast<size_t>(controlId - InputBase) < state->inputs.size() &&
                                     !choicesFor(state->inputs[controlId - InputBase].key).empty());
        if (combo && (wParam == VK_RETURN || wParam == VK_ESCAPE) &&
            SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0))
            return DefSubclassProc(window, message, wParam, lParam);
        if (mutation && (wParam == VK_RETURN || (ctrl && wParam == 'S')))
        {
            if (wParam == VK_RETURN && (controlId == CpuAll || controlId == CpuNone ||
                                        (controlId >= CpuBase && controlId < CpuBase + CpuPageSize)))
                PostMessageW(owner, WM_COMMAND, MAKEWPARAM(controlId, BN_CLICKED),
                             reinterpret_cast<LPARAM>(window));
            else
                PostMessageW(owner, WM_COMMAND, controlId == IDCANCEL && wParam == VK_RETURN ? IDCANCEL : Run,
                             0);
            return 0;
        }
        if (ctrl && wParam == 'A' &&
            (controlId == Details || (controlId >= InputBase && controlId < CpuBase && state &&
                                      static_cast<size_t>(controlId - InputBase) < state->inputs.size() &&
                                      choicesFor(state->inputs[controlId - InputBase].key).empty())))
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
        if (wParam == VK_ESCAPE || (!mutation && ctrl && (wParam == 'R' || wParam == 'S')))
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
        for (size_t i = 0; !state->affinity && i < state->inputs.size(); ++i)
        {
            state->labels.push_back(
                control(window, WC_STATICW, state->inputs[i].label.c_str(), SS_NOPREFIX, 0));
            const auto &input = state->inputs[i];
            const auto &choices = state->mutation ? choicesFor(input.key) : choicesFor("");
            HWND spin = nullptr;
            HWND edit = control(
                window, choices.empty() ? WC_EDITW : WC_COMBOBOXW,
                choices.empty() ? input.value.c_str() : L"",
                WS_TABSTOP | (choices.empty() ? WS_BORDER | ES_AUTOHSCROLL : CBS_DROPDOWNLIST | WS_VSCROLL),
                InputBase + static_cast<int>(i));
            if (!choices.empty())
            {
                for (size_t choice = 0; choice < choices.size(); ++choice)
                {
                    SendMessageW(edit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choices[choice].first));
                    if (input.value == wide(choices[choice].second))
                        SendMessageW(edit, CB_SETCURSEL, choice, 0);
                }
            }
            else
            {
                SendMessageW(edit, EM_SETLIMITTEXT, 65536, 0);
                if (state->mutation && (input.key == "nice" || input.key == "level"))
                {
                    spin = control(window, UPDOWN_CLASSW, L"", UDS_ARROWKEYS | UDS_SETBUDDYINT, 0);
                    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edit), 0);
                    SendMessageW(spin, UDM_SETRANGE32, input.key == "nice" ? -20 : 0,
                                 input.key == "nice" ? 19 : 7);
                    // UDM_SETRANGE32 must not replace the supplied current value.
                    SetWindowTextW(edit, input.value.c_str());
                }
            }
            state->edits.push_back(edit);
            state->spinners.push_back(spin);
        }
        if (state->affinity)
        {
            state->cpuLabel = control(window, WC_STATICW, L"Loading CPUs...", SS_NOPREFIX, 0);
            state->cpuAll = control(window, WC_BUTTONW, L"Select all", WS_TABSTOP | BS_PUSHBUTTON, CpuAll);
            state->cpuNone = control(window, WC_BUTTONW, L"Clear", WS_TABSTOP | BS_PUSHBUTTON, CpuNone);
            state->cpuPage =
                control(window, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, CpuPage);
            ShowWindow(state->cpuPage, SW_HIDE);
            for (size_t i = 0; i < CpuPageSize; ++i)
            {
                HWND check =
                    control(window, WC_BUTTONW, L"", WS_TABSTOP | BS_3STATE, CpuBase + static_cast<int>(i));
                state->cpuChecks.push_back(check);
                ShowWindow(check, SW_HIDE);
            }
        }
        if (!state->schedulingExecutable.empty())
        {
            state->saveScheduling = control(window, WC_BUTTONW, L"Save for this executable",
                                            WS_TABSTOP | BS_AUTOCHECKBOX, SaveScheduling);
            SendMessageW(state->saveScheduling, BM_SETCHECK,
                         state->schedulingSaved ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        state->run = control(window, WC_BUTTONW,
                             state->mutation         ? L"Save"
                             : state->inputs.empty() ? L"Refresh"
                                                     : L"Run",
                             WS_TABSTOP | (state->mutation ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), Run);
        if (!state->mutation)
        {
            state->copy = control(window, WC_BUTTONW, L"Copy", WS_TABSTOP | BS_PUSHBUTTON, Copy);
            state->save = control(window, WC_BUTTONW, L"Save...", WS_TABSTOP | BS_PUSHBUTTON, Save);
            if (state->view == ResourceView::Both || state->view == ResourceView::DetailsFirst)
            {
                state->tabs = control(window, WC_TABCONTROLW, L"",
                                      WS_TABSTOP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, Tabs);
                SetWindowLongPtrW(state->tabs, GWL_EXSTYLE, WS_EX_CONTROLPARENT);
                SetWindowSubclass(state->tabs, pageMessages, 2, reinterpret_cast<DWORD_PTR>(window));
                for (const wchar_t *name : {L"Properties", L"Details"})
                {
                    TCITEMW item{};
                    item.mask = TCIF_TEXT;
                    item.pszText = const_cast<wchar_t *>(name);
                    TabCtrl_InsertItem(state->tabs, TabCtrl_GetItemCount(state->tabs), &item);
                }
                if (state->view == ResourceView::DetailsFirst)
                    TabCtrl_SetCurSel(state->tabs, 1);
            }
            HWND page = state->tabs ? state->tabs : window;
            if (state->view != ResourceView::DetailsOnly)
            {
                state->properties.create(page, Properties, {{L"Field", 210}, {L"Value", 520}});
                SetWindowLongPtrW(state->properties.window, GWL_STYLE,
                                  GetWindowLongPtrW(state->properties.window, GWL_STYLE) | WS_BORDER |
                                      WS_CLIPSIBLINGS);
            }
            if (state->view != ResourceView::PropertiesOnly)
            {
                state->details = control(page, WC_EDITW, L"Press \"Run\" to collect details.",
                                         WS_TABSTOP | WS_BORDER | WS_CLIPSIBLINGS | WS_VSCROLL | WS_HSCROLL |
                                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                         Details);
                SendMessageW(state->details, EM_SETLIMITTEXT, 32 * 1024 * 1024, 0);
            }
            state->status = control(window, WC_STATICW, L"Ready.", SS_NOPREFIX | SS_ENDELLIPSIS, Status);
            state->tooltips =
                CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
            if (state->tooltips)
            {
                SendMessageW(state->tooltips, TTM_SETMAXTIPWIDTH, 0, scale(window, 500));
                TTTOOLINFOW tool{sizeof(tool)};
                tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool.hwnd = window;
                tool.uId = reinterpret_cast<UINT_PTR>(state->status);
                tool.lpszText = LPSTR_TEXTCALLBACKW;
                SendMessageW(state->tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }
        state->close = control(window, WC_BUTTONW, state->mutation ? L"Cancel" : L"Close",
                               WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
        EnableWindow(state->copy, FALSE);
        EnableWindow(state->save, FALSE);
        WslApplyTheme(window);
        applyFonts(*state);
        SetWindowSubclass(window, shortcuts, 9, reinterpret_cast<DWORD_PTR>(window));
        EnumChildWindows(window, installShortcuts, reinterpret_cast<LPARAM>(window));
        layout(*state);
        updateInputs(*state);
        if (state->affinity)
            requestAffinity(*state);
        if (state->cached)
        {
            ShowWindow(state->run, SW_HIDE);
            Reply reply;
            reply.data = std::move(state->cachedResponse);
            loadReply(*state, reply);
        }
        else if (state->inputs.empty() && !state->mutation)
            PostMessageW(window, WM_COMMAND, Run, 0);
        if (!state->initialNotice.empty())
            PostMessageW(window, WM_APP + 89, 0, 0);
        return 0;
    }
    case WM_APP + 89: {
        const auto notice = state->initialNotice;
        errorBox(window, notice);
        return 0;
    }
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->uiFont);
    case WM_SIZE:
        layout(*state);
        return 0;
    case WM_GETMINMAXINFO: {
        const int rows = state->affinity
                             ? 1 + static_cast<int>((std::min(CpuPageSize, state->onlineCpus.size()) + 7) / 8)
                             : static_cast<int>(state->inputs.size());
        reinterpret_cast<MINMAXINFO *>(lParam)->ptMinTrackSize = {
            scale(window, state->mutation ? 480 : 620),
            scale(window, (state->mutation ? 100 : 390) + rows * 28 + (state->saveScheduling ? 28 : 0))};
        return 0;
    }
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
        if (state->properties.window && reinterpret_cast<HWND>(wParam) == state->properties.window)
        {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (point.x == -1 && point.y == -1)
            {
                const int selected = state->properties.selectedIndex();
                RECT row{};
                if (selected >= 0)
                    state->properties.rowRect(selected, row);
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
        if (LOWORD(wParam) >= CpuBase && LOWORD(wParam) < CpuBase + CpuPageSize &&
            HIWORD(wParam) == BN_CLICKED)
        {
            const size_t index = state->cpuPageIndex * CpuPageSize + LOWORD(wParam) - CpuBase;
            if (state->affinityReady && !state->busy && index < state->onlineCpus.size())
            {
                const int cpu = state->onlineCpus[index];
                if (state->mixedCpus.erase(cpu) || !state->selectedCpus.count(cpu))
                    state->selectedCpus.insert(cpu);
                else
                    state->selectedCpus.erase(cpu);
                showCpuPage(*state);
            }
            return 0;
        }
        if (HIWORD(wParam) == CBN_SELCHANGE)
        {
            if (LOWORD(wParam) == CpuPage)
            {
                const LRESULT page = SendMessageW(state->cpuPage, CB_GETCURSEL, 0, 0);
                if (page >= 0)
                {
                    state->cpuPageIndex = static_cast<size_t>(page);
                    showCpuPage(*state);
                }
            }
            else
                updateInputs(*state);
            return 0;
        }
        switch (LOWORD(wParam))
        {
        case CpuAll:
        case CpuNone:
            if (state->affinityReady && !state->busy)
            {
                state->selectedCpus.clear();
                state->mixedCpus.clear();
                if (LOWORD(wParam) == CpuAll)
                    state->selectedCpus.insert(state->onlineCpus.begin(), state->onlineCpus.end());
                showCpuPage(*state);
            }
            break;
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
        if (state->tabs && hdr->hwndFrom == state->tabs && hdr->code == TCN_SELCHANGE)
            layout(*state);
        else if (state->tooltips && hdr->hwndFrom == state->tooltips && hdr->code == TTN_GETDISPINFOW)
            reinterpret_cast<NMTTDISPINFOW *>(lParam)->lpszText = state->statusText.data();
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
        if (state->mutation && state->busy && !state->loadingAffinity)
        {
            // A syscall already in flight may finish after Cancel closes the
            // form. The serial worker will run this refresh after that action.
            HWND owner = GetWindow(window, GW_OWNER);
            if (owner && IsWindow(owner))
                PostMessageW(owner, ResourceActionCompleted, 0, 0);
        }
        if (state->properties.window)
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
    const bool mutation = state->mutation;
    const int height = mutation ? 110 + static_cast<int>(state->inputs.size()) * 28 +
                                      (!state->schedulingExecutable.empty() ? 28 : 0)
                                : 560;
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, ToolClass, state->title.c_str(),
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  scale(owner, mutation ? 540 : 820), scale(owner, height), owner, nullptr,
                                  instance, &state);
    if (!window)
    {
        errorBox(owner, L"Could not create the resource tool window.");
        return;
    }
    WslPositionDialog(window, owner);
    ShowWindow(window, SW_SHOW);
    auto opened = reinterpret_cast<ToolWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (opened)
        SetFocus(opened->affinity             ? opened->close
                 : hasInputs                  ? opened->edits.front()
                 : !cached                    ? opened->run
                 : showingProperties(*opened) ? opened->properties.window
                                              : opened->details);
}
} // namespace

void openResourceTool(HWND owner, const std::wstring &distro, Json request, const std::wstring &title,
                      std::vector<ResourceInput> inputs, bool mutation, ResourceView view)
{
    auto state = std::make_unique<ToolWindow>();
    state->distro = distro;
    state->request = std::move(request);
    state->title = title;
    state->inputs = std::move(inputs);
    state->mutation = mutation;
    state->view = view;
    state->affinity = mutation && state->request.value("action", std::string{}) == "set_affinity";
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

void openResourceReport(HWND owner, const std::wstring &title, Json response, ResourceView view,
                        const std::wstring &status)
{
    auto state = std::make_unique<ToolWindow>();
    state->title = title;
    state->cached = true;
    state->view = view;
    state->readyStatus = status;
    state->cachedResponse = std::move(response);
    showTool(owner, std::move(state));
}
} // namespace wsl
