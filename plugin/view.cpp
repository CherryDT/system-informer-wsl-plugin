#include "common.hpp"
#include "controller.hpp"
#include "settings.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <windowsx.h>

namespace wsl
{
namespace
{
enum Id
{
    DistroCombo = 100,
    RefreshButton,
    PauseButton,
    SettingsButton,
    SearchEdit,
    ViewTabs,
    ProcessTable,
    ConnectionTable,
    ServiceTable,
    ListenerCheck,
    TreeCheck,
    InspectButton,
    ActionsButton,
    ExportButton,
    Inspect = 200,
    CopyRow,
    CopyCommand,
    OpenExecutable,
    Terminate,
    Kill,
    Suspend,
    Resume,
    User1,
    User2,
    StartService,
    StopService,
    RestartService,
    ReloadService,
    EnableService,
    DisableService,
    GoToProcess
};
struct ProcessSample
{
    uint64_t ticks = 0, read = 0, written = 0;
};
struct View
{
    HWND window{}, distro{}, refresh{}, pause{}, settings{}, search{}, tabs{}, listeners{}, tree{}, inspect{},
        actions{}, exportButton{}, status{}, summary{}, graph{}, scope{};
    Table processes, connections, services;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    Json snapshot, sockets, units;
    std::map<std::string, ProcessSample> previous;
    std::map<std::string, double> cpu, readRate, writeRate;
    std::deque<double> history;
    uint64_t previousTime = 0;
    std::string bootId;
    std::wstring selectedDistro;
    unsigned epoch = 1;
    int page = 0;
    bool active = false, paused = false, pending = false, failed = false;
    Table &table()
    {
        return page == 0 ? processes : page == 1 ? connections : services;
    }
};
View *mainView = nullptr;
const wchar_t *viewClass = L"WslTools.View";
std::string processKey(const Json &process)
{
    return process.at("pid").dump() + ":" + process.at("start_ticks").dump();
}
std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
std::wstring windowText(HWND window)
{
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
    value.resize(wcslen(value.c_str()));
    return value;
}
void status(View &v, const std::wstring &value)
{
    SetWindowTextW(v.status, value.c_str());
}
void queue(View &v, Json request, uintptr_t tag)
{
    v.pending = true;
    submit(v.selectedDistro, std::move(request), v.mailbox, (static_cast<uintptr_t>(v.epoch) << 16) | tag);
}
void refresh(View &v)
{
    if (!v.active || v.paused || v.pending || v.failed)
        return;
    if (v.selectedDistro.empty())
    {
        queue(v, {{"op", "discover"}}, DiscoverTag);
        return;
    }
    queue(v,
          {{"op", v.page == 0   ? "snapshot"
                  : v.page == 1 ? "connections"
                                : "services"}},
          v.page == 0   ? SnapshotTag
          : v.page == 1 ? ConnectionsTag
                        : ServicesTag);
}
void clearDistro(View &v)
{
    if (!v.selectedDistro.empty())
        disconnect(v.selectedDistro);
    ++v.epoch;
    v.pending = false;
    v.failed = false;
    v.previous.clear();
    v.cpu.clear();
    v.readRate.clear();
    v.writeRate.clear();
    v.previousTime = 0;
    v.bootId.clear();
    v.history.clear();
    v.snapshot = Json();
    v.sockets = Json();
    v.units = Json();
    v.processes.replace({});
    v.connections.replace({});
    v.services.replace({});
    SetWindowTextW(v.summary, L"Connecting to the selected distribution…");
    InvalidateRect(v.graph, nullptr, TRUE);
}
void updateButtons(View &v)
{
    bool selected = v.table().selected() != nullptr;
    EnableWindow(v.inspect, selected);
    EnableWindow(v.actions, selected);
    EnableWindow(v.settings, !v.selectedDistro.empty());
}
bool matches(const Row &row, const std::wstring &query)
{
    if (query.empty())
        return true;
    // Space-separated terms are ANDed, so "node 3000" narrows socket searches naturally.
    size_t begin = 0;
    std::wstring combined;
    for (const auto &cell : row.cells)
        combined += lower(cell) + L" ";
    while (begin < query.size())
    {
        size_t end = query.find(L' ', begin);
        auto term = query.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (!term.empty() && combined.find(term) == std::wstring::npos)
            return false;
        if (end == std::wstring::npos)
            break;
        begin = end + 1;
    }
    return true;
}
void render(View &v)
{
    std::vector<Row> rows;
    const auto query = lower(windowText(v.search));
    if (v.page == 0 && v.snapshot.contains("processes"))
    {
        auto items = v.snapshot["processes"].get<std::vector<Json>>();
        std::map<int, int> depths;
        const bool tree = SendMessageW(v.tree, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (tree)
        {
            std::map<int, std::vector<Json>> children;
            std::set<int> pids, visited;
            for (auto &p : items)
                pids.insert(p.value("pid", 0));
            for (auto &p : items)
                children[pids.count(p.value("ppid", 0)) ? p.value("ppid", 0) : 0].push_back(p);
            items.clear();
            std::function<void(int, int)> visit = [&](int parent, int depth) {
                for (auto &p : children[parent])
                {
                    int pid = p.value("pid", 0);
                    if (!visited.insert(pid).second)
                        continue;
                    depths[pid] = std::min(depth, 12);
                    items.push_back(p);
                    visit(pid, depth + 1);
                }
            };
            visit(0, 0);
            // A parent exiting mid-snapshot must not make a process disappear.
            for (auto &p : v.snapshot["processes"])
                if (!visited.count(p.value("pid", 0)))
                    items.push_back(p);
            v.processes.sortColumn = -1;
        }
        for (const auto &p : items)
        {
            auto key = processKey(p);
            std::wstring name = std::wstring(depths[p.value("pid", 0)] * 2, L' ') + text(p, "name");
            Row row{{name, text(p, "pid"), text(p, "user"), number(v.cpu[key]),
                     number(p.value("rss_bytes", 0ull) / 1048576.0), number(v.readRate[key] / 1024.0),
                     number(v.writeRate[key] / 1024.0), text(p, "state"), text(p, "threads"), text(p, "ppid"),
                     text(p, "command")},
                    p,
                    key};
            if (matches(row, query))
                rows.push_back(std::move(row));
        }
        v.processes.replace(std::move(rows));
    }
    else if (v.page == 1 && v.sockets.contains("connections"))
    {
        bool onlyListeners = SendMessageW(v.listeners, BM_GETCHECK, 0, 0) == BST_CHECKED;
        for (const auto &c : v.sockets["connections"])
        {
            const auto protocol = text(c, "protocol"), state = text(c, "state");
            const bool listening =
                state == L"LISTEN" || state == L"LISTENING" ||
                (protocol.find(L"udp") != std::wstring::npos && c.value("remote_port", 0) == 0);
            if (onlyListeners && !listening)
                continue;
            Row row{{protocol, text(c, "local_address"), text(c, "local_port"), text(c, "remote_address"),
                     text(c, "remote_port"), state, text(c, "pid"), text(c, "process"), text(c, "inode")},
                    c,
                    c.dump()};
            if (matches(row, query))
                rows.push_back(std::move(row));
        }
        v.connections.replace(std::move(rows));
    }
    else if (v.page == 2 && v.units.contains("services"))
    {
        for (const auto &s : v.units["services"])
        {
            Row row{{text(s, "name"), text(s, "active"), text(s, "sub"), text(s, "enabled"), text(s, "load"),
                     text(s, "description")},
                    s,
                    s.value("name", "")};
            if (matches(row, query))
                rows.push_back(std::move(row));
        }
        v.services.replace(std::move(rows));
    }
    updateButtons(v);
}
void updateSnapshot(View &v, const Json &data)
{
    uint64_t now = data.value("monotonic_ms", 0ull);
    auto boot = data.value("boot_id", "");
    if (v.bootId != boot || now <= v.previousTime)
    {
        v.previous.clear();
        v.previousTime = 0;
        v.history.clear();
        v.bootId = boot;
    }
    double elapsed = v.previousTime ? (now - v.previousTime) / 1000.0 : 0;
    double hz = data.value("clock_ticks", 100.0), total = 0;
    std::map<std::string, ProcessSample> samples;
    v.cpu.clear();
    v.readRate.clear();
    v.writeRate.clear();
    for (const auto &p : data.at("processes"))
    {
        auto key = processKey(p);
        ProcessSample sample{p.value("cpu_ticks", 0ull), p.value("read_bytes", 0ull),
                             p.value("write_bytes", 0ull)};
        double usage = 0, read = 0, written = 0;
        auto previous = v.previous.find(key);
        if (elapsed > 0 && hz > 0 && previous != v.previous.end())
        {
            auto &old = previous->second;
            if (sample.ticks >= old.ticks)
                usage = 100.0 * (sample.ticks - old.ticks) / hz / elapsed;
            if (sample.read >= old.read)
                read = (sample.read - old.read) / elapsed;
            if (sample.written >= old.written)
                written = (sample.written - old.written) / elapsed;
        }
        samples[key] = sample;
        v.cpu[key] = usage;
        v.readRate[key] = read;
        v.writeRate[key] = written;
        total += usage;
    }
    v.previous = std::move(samples);
    v.previousTime = now;
    v.snapshot = data;
    double cpus = std::max(1.0, data.value("cpus", 1.0));
    if (elapsed > 0)
    {
        v.history.push_back(total / cpus);
        if (v.history.size() > 120)
            v.history.pop_front();
    }
    auto count = data["processes"].size();
    std::wstring summary = std::to_wstring(count) + L" processes  ·  CPU " + number(total / cpus) + L"% of " +
                           number(cpus, 0) + L" vCPUs  ·  VM memory available " +
                           bytes(data.value("memory_available", 0ull)) + L" / " +
                           bytes(data.value("memory_total", 0ull));
    if (v.page == 0) SetWindowTextW(v.summary, summary.c_str());
    InvalidateRect(v.graph, nullptr, TRUE);
}
void inspect(View &v)
{
    const Row *row = v.table().selected();
    if (!row)
        return;
    if (v.page == 0)
        openDetails(v.window, v.selectedDistro, row->data);
    else if (v.page == 2)
        openServiceDetails(v.window, v.selectedDistro, row->data.value("name", ""));
    else
    {
        int pid = row->data.value("pid", 0);
        if (!pid)
        {
            errorBox(v.window, L"No owner was visible for this socket. It may have closed or belong to "
                               L"another PID namespace.");
            return;
        }
        if (!row->data.contains("start_ticks") || row->data["start_ticks"].is_null())
        {
            errorBox(v.window, L"The socket owner's identity is unavailable. Refresh the connections view.");
            return;
        }
        Json owner = {{"pid", pid}, {"start_ticks", row->data["start_ticks"]},
                      {"name", row->data.value("process", "")}};
        openDetails(v.window, v.selectedDistro, owner);
    }
}
void action(View &v, int id)
{
    const Row *selected = v.table().selected();
    if (!selected)
        return;
    Row row = *selected;
    if (id == Inspect || id == GoToProcess)
    {
        inspect(v);
        return;
    }
    if (id == CopyRow)
    {
        std::wstring result;
        for (auto &c : row.cells)
        {
            if (!result.empty())
                result += L"\t";
            result += c;
        }
        copyText(v.window, result);
        return;
    }
    if (id == CopyCommand)
    {
        copyText(v.window, text(row.data, "command"));
        return;
    }
    if (id == OpenExecutable)
    {
        openLinuxPath(v.window, v.selectedDistro, text(row.data, "exe"));
        return;
    }
    if (v.pending)
    {
        errorBox(v.window, L"A request is still running. Wait for it to finish, then try the action again.");
        return;
    }
    if (v.paused || !v.active || v.failed)
    {
        errorBox(v.window, L"Resume monitoring and refresh before changing a process or service.");
        return;
    }
    if (v.page == 0)
    {
        int signal = 0;
        std::wstring label;
        switch (id)
        {
        case Terminate:
            signal = 15;
            label = L"Send SIGTERM (graceful termination)";
            break;
        case Kill:
            signal = 9;
            label = L"Send SIGKILL (force termination)";
            break;
        case Suspend:
            signal = 19;
            label = L"Send SIGSTOP (suspend)";
            break;
        case Resume:
            signal = 18;
            label = L"Send SIGCONT (resume)";
            break;
        case User1:
            signal = 10;
            label = L"Send SIGUSR1";
            break;
        case User2:
            signal = 12;
            label = L"Send SIGUSR2";
            break;
        default:
            return;
        }
        auto prompt = label + L" to " + text(row.data, "name") + L" (PID " + text(row.data, "pid") +
                      L") in " + v.selectedDistro + L"?\r\n\r\n";
        prompt +=
            signal == 10 || signal == 12
                ? L"The program defines this signal's behavior. Without a handler, it terminates the process."
                : L"This action runs as Linux root. Termination may lose unsaved work.";
        if (MessageBoxW(v.window, prompt.c_str(), L"Confirm process signal",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
            return;
        queue(v,
              {{"op", "signal"},
               {"pid", row.data["pid"]},
               {"start_ticks", row.data["start_ticks"]},
               {"signal", signal}},
              ActionTag);
    }
    else if (v.page == 2)
    {
        const char *verb = nullptr;
        switch (id)
        {
        case StartService:
            verb = "start";
            break;
        case StopService:
            verb = "stop";
            break;
        case RestartService:
            verb = "restart";
            break;
        case ReloadService:
            verb = "reload";
            break;
        case EnableService:
            verb = "enable";
            break;
        case DisableService:
            verb = "disable";
            break;
        default:
            return;
        }
        auto name = row.data.value("name", "");
        auto prompt = wide(verb) + L" " + wide(name) + L" in " + v.selectedDistro +
                      L"?\r\n\r\nThis changes a system service as Linux root.";
        if (MessageBoxW(v.window, prompt.c_str(), L"Confirm service action",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
            return;
        queue(v, {{"op", "service_action"}, {"name", name}, {"action", verb}}, ActionTag);
    }
    status(v, L"Applying action…");
}
void menu(View &v, POINT point)
{
    if (!v.table().selected())
        return;
    HMENU popup = CreatePopupMenu();
    AppendMenuW(popup, MF_STRING, Inspect,
                v.page == 1 ? L"Inspect owning process\tEnter" : L"Inspect…\tEnter");
    AppendMenuW(popup, MF_STRING, CopyRow, L"Copy row\tCtrl+C");
    if (v.page == 0)
    {
        AppendMenuW(popup, MF_STRING, CopyCommand, L"Copy command line");
        AppendMenuW(popup, MF_STRING, OpenExecutable, L"Show executable in Explorer");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, Terminate, L"Terminate — SIGTERM");
        AppendMenuW(popup, MF_STRING, Kill, L"Force kill — SIGKILL");
        AppendMenuW(popup, MF_STRING, Suspend, L"Suspend — SIGSTOP");
        AppendMenuW(popup, MF_STRING, Resume, L"Resume — SIGCONT");
        AppendMenuW(popup, MF_STRING, User1, L"Send SIGUSR1");
        AppendMenuW(popup, MF_STRING, User2, L"Send SIGUSR2");
    }
    else if (v.page == 2)
    {
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, StartService, L"Start");
        AppendMenuW(popup, MF_STRING, StopService, L"Stop");
        AppendMenuW(popup, MF_STRING, RestartService, L"Restart");
        AppendMenuW(popup, MF_STRING, ReloadService, L"Reload configuration");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, EnableService, L"Enable at boot");
        AppendMenuW(popup, MF_STRING, DisableService, L"Disable at boot");
    }
    int chosen =
        TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, v.window, nullptr);
    DestroyMenu(popup);
    if (chosen)
        action(v, chosen);
}
void layout(View &v)
{
    RECT rect{};
    GetClientRect(v.window, &rect);
    auto s = [&](int x) { return scale(v.window, x); };
    int width = rect.right, height = rect.bottom, pad = s(10), line = s(28), gap = s(8);
    int combo = std::max(s(130), std::min(s(300), width - s(340)));
    place(v.distro, pad, pad, combo, s(300));
    place(v.refresh, pad + combo + gap, pad, s(86), line);
    place(v.pause, pad + combo + gap + s(94), pad, s(86), line);
    place(v.settings, pad + combo + gap + s(188), pad, s(88), line);
    place(v.summary, pad, s(48), width - 2 * pad, s(24));
    const bool showChart = v.page == 0 && height >= s(600);
    const int compactOffset = showChart ? 0 : s(54);
    ShowWindow(v.graph, showChart ? SW_SHOW : SW_HIDE);
    place(v.graph, pad, s(77), width - 2 * pad, s(46));
    place(v.tabs, pad, (s(132) - compactOffset), width - 2 * pad, s(30));
    place(v.search, pad, (s(173) - compactOffset), std::max(s(140), width - s(385)), line);
    place(v.listeners, width - s(230), (s(175) - compactOffset), (s(210) - compactOffset), line);
    place(v.tree, width - s(230), (s(175) - compactOffset), (s(210) - compactOffset), line);
    place(v.scope, pad, (s(210) - compactOffset), width - 2 * pad, s(23));
    int bottom = s(78);
    for (auto table : {&v.processes, &v.connections, &v.services})
        place(table->window, pad, (s(238) - compactOffset), width - 2 * pad, height - (s(238) - compactOffset) - bottom);
    place(v.inspect, pad, height - s(66), s(112), line);
    place(v.actions, pad + s(122), height - s(66), s(104), line);
    place(v.exportButton, pad + s(236), height - s(66), s(114), line);
    place(v.status, pad, height - s(29), width - 2 * pad, s(24));
}
void switchPage(View &v)
{
    v.page = TabCtrl_GetCurSel(v.tabs);
    ShowWindow(v.graph, v.page == 0 ? SW_SHOW : SW_HIDE);
    if (v.page != 0)
        SetWindowTextW(v.summary, L"Root inspection · selected distribution · process CPU history is available on the Processes tab");
    else if (!v.snapshot.is_null())
        SetWindowTextW(v.summary, L"Refreshing process CPU and VM memory…");
    ShowWindow(v.processes.window, v.page == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.connections.window, v.page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.services.window, v.page == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.listeners, v.page == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(v.tree, v.page == 0 ? SW_SHOW : SW_HIDE);
    SendMessageW(v.search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(v.page == 0   ? L"Filter name, PID, user or command…"
                                          : v.page == 1 ? L"Find port, address, PID or process…"
                                                        : L"Filter service, state or description…"));
    SetWindowTextW(v.scope,
                   v.page == 0 ? L"CPU: 100% = one vCPU. I/O is storage accounting. Double-click for details."
                   : v.page == 1 ? L"Current network namespace; owners visible in this distro. UDP endpoints "
                                   L"are shown as bound ports."
                                 : L"System services (root). Inspect a service for properties, unit "
                                   L"configuration and recent journal entries.");
    layout(v);
    render(v);
    refresh(v);
}
LRESULT CALLBACK graphProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message != WM_PAINT)
        return DefWindowProcW(window, message, wparam, lparam);
    auto v = reinterpret_cast<View *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window, &ps);
    RECT r{};
    GetClientRect(window, &r);
    const bool dark = WslIsDarkTheme() != FALSE;
    HBRUSH background = CreateSolidBrush(dark ? RGB(40, 40, 40) : GetSysColor(COLOR_WINDOW));
    FillRect(dc, &r, background);
    DeleteObject(background);
    HPEN grid = CreatePen(PS_SOLID, 1, dark ? RGB(60, 60, 60) : GetSysColor(COLOR_3DFACE));
    auto old = SelectObject(dc, grid);
    for (int i = 1; i < 4; ++i)
    {
        MoveToEx(dc, 0, r.bottom * i / 4, nullptr);
        LineTo(dc, r.right, r.bottom * i / 4);
    }
    SelectObject(dc, old);
    DeleteObject(grid);
    if (v && v->history.size() > 1)
    {
        HPEN line = CreatePen(PS_SOLID, 2, RGB(35, 155, 195));
        old = SelectObject(dc, line);
        for (size_t i = 0; i < v->history.size(); ++i)
        {
            int x = static_cast<int>(i) * r.right / 119;
            int y = r.bottom - 2 -
                    static_cast<int>(std::clamp(v->history[i], 0.0, 100.0) * (r.bottom - 4) / 100.0);
            if (i)
                LineTo(dc, x, y);
            else
                MoveToEx(dc, x, y, nullptr);
        }
        SelectObject(dc, old);
        DeleteObject(line);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dark ? RGB(190, 190, 190) : GetSysColor(COLOR_GRAYTEXT));
    auto oldFont = SelectObject(dc, font);
    DrawTextW(dc, L"Distro CPU history · 120 samples", -1, &r, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    EndPaint(window, &ps);
    return 0;
}
void manualRefresh(View &v)
{
    if (v.pending)
        return;
    if (v.failed)
        disconnect(v.selectedDistro);
    v.failed = false;
    v.paused = false;
    SetWindowTextW(v.pause, L"Pause");
    queue(v, {{"op", "discover"}}, DiscoverTag);
    status(v, L"Discovering running WSL2 distributions…");
}
LRESULT CALLBACK childKeys(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data)
{
    auto v = reinterpret_cast<View *>(data);
    if (message == WM_KEYDOWN)
    {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && wparam == 'F')
        {
            SetFocus(v->search);
            SendMessageW(v->search, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wparam == VK_F5)
        {
            manualRefresh(*v);
            return 0;
        }
        if (window == v->table().window)
        {
            if (wparam == VK_RETURN)
            {
                inspect(*v);
                return 0;
            }
            if (ctrl && wparam == 'C')
            {
                action(*v, CopyRow);
                return 0;
            }
            if (wparam == VK_DELETE && v->page == 0)
            {
                action(*v, Terminate);
                return 0;
            }
        }
        if (wparam == VK_ESCAPE && window == v->search)
        {
            SetWindowTextW(v->search, L"");
            return 0;
        }
        if (wparam == VK_TAB)
        {
            HWND next = GetNextDlgTabItem(v->window, window, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            if (next)
                SetFocus(next);
            return 0;
        }
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, childKeys, 1);
    return DefSubclassProc(window, message, wparam, lparam);
}
LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto v = reinterpret_cast<View *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        v = new View;
        v->window = window;
        v->mailbox->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v));
        mainView = v;
    }
    if (!v)
        return DefWindowProcW(window, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE: {
        v->distro =
            control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, DistroCombo);
        v->refresh = control(window, L"BUTTON", L"Refresh", WS_TABSTOP, RefreshButton);
        v->pause = control(window, L"BUTTON", L"Pause", WS_TABSTOP, PauseButton);
        v->settings = control(window, L"BUTTON", L"Settings…", WS_TABSTOP, SettingsButton);
        v->summary =
            control(window, L"STATIC", L"Select a running WSL2 distribution to inspect it as root.", 0, 0);
        v->graph = control(window, L"WslTools.Graph", L"CPU history", 0, 0);
        SetWindowLongPtrW(v->graph, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v));
        v->tabs = control(window, WC_TABCONTROLW, L"Views", WS_TABSTOP, ViewTabs);
        for (auto label : {L"Processes", L"Connections", L"Services"})
        {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t *>(label);
            TabCtrl_InsertItem(v->tabs, TabCtrl_GetItemCount(v->tabs), &item);
        }
        v->search = control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, SearchEdit);
        v->listeners = control(window, L"BUTTON", L"Listening / bound ports only",
                               BS_AUTOCHECKBOX | WS_TABSTOP, ListenerCheck);
        v->tree =
            control(window, L"BUTTON", L"Show process ancestry", BS_AUTOCHECKBOX | WS_TABSTOP, TreeCheck);
        v->scope = control(window, L"STATIC", L"", SS_LEFT, 0);
        v->processes.create(window, ProcessTable,
                            {{L"Process", 180},
                             {L"PID", 70, true},
                             {L"User", 90},
                             {L"CPU %", 80, true},
                             {L"RSS MiB", 90, true},
                             {L"Read KiB/s", 95, true},
                             {L"Write KiB/s", 95, true},
                             {L"State", 60},
                             {L"Threads", 65, true},
                             {L"PPID", 65, true},
                             {L"Command line", 540}});
        v->processes.sortColumn = 3;
        v->processes.descending = true;
        v->connections.create(window, ConnectionTable,
                              {{L"Protocol", 80},
                               {L"Local address", 200},
                               {L"Port", 75, true},
                               {L"Remote address", 200},
                               {L"Remote port", 90, true},
                               {L"State", 120},
                               {L"PID", 70, true},
                               {L"Process", 150},
                               {L"Socket inode", 110, true}});
        v->services.create(window, ServiceTable,
                           {{L"Service", 255},
                            {L"Active", 95},
                            {L"Substate", 105},
                            {L"Startup", 100},
                            {L"Load", 100},
                            {L"Description", 500}});
        v->inspect = control(window, L"BUTTON", L"Inspect…", WS_TABSTOP, InspectButton);
        v->actions = control(window, L"BUTTON", L"Actions ▾", WS_TABSTOP, ActionsButton);
        v->exportButton = control(window, L"BUTTON", L"Export view…", WS_TABSTOP, ExportButton);
        v->status = control(
            window, L"STATIC",
            L"Monitoring starts when this tab is selected. Stopped distros are not started intentionally.",
            SS_LEFT, 0);
        for (HWND child : {v->distro, v->refresh, v->pause, v->settings, v->tabs, v->search, v->listeners,
                           v->tree, v->processes.window, v->connections.window, v->services.window,
                           v->inspect, v->actions, v->exportButton})
            SetWindowSubclass(child, childKeys, 1, reinterpret_cast<DWORD_PTR>(v));
        WslApplyTheme(window);
        switchPage(*v);
        layout(*v);
        SetTimer(window, 1, 500, nullptr);
        startController();
        return 0;
    }
    case WM_SIZE:
        layout(*v);
        return 0;
    case WM_TIMER: {
        static ULONGLONG last = 0;
        auto now = GetTickCount64();
        auto interval = std::clamp(readSetting(L"RefreshInterval", 2000), 500ul, 60000ul);
        if (now - last >= interval)
        {
            last = now;
            refresh(*v);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wparam);
        if (id == DistroCombo && HIWORD(wparam) == CBN_SELCHANGE)
        {
            auto chosen = windowText(v->distro);
            if (chosen != v->selectedDistro)
            {
                clearDistro(*v);
                v->selectedDistro = chosen;
                refresh(*v);
            }
            return 0;
        }
        if (id == SearchEdit && HIWORD(wparam) == EN_CHANGE)
        {
            render(*v);
            return 0;
        }
        switch (id)
        {
        case RefreshButton:
            manualRefresh(*v);
            break;
        case PauseButton:
            v->paused = !v->paused;
            SetWindowTextW(v->pause, v->paused ? L"Resume" : L"Pause");
            if (v->paused)
            {
                disconnect(v->selectedDistro);
                ++v->epoch;
                v->pending = false;
                status(*v, L"Paused · collector disconnected; displayed data is a snapshot.");
            }
            else
            {
                v->failed = false;
                v->previousTime = 0;
                refresh(*v);
            }
            break;
        case SettingsButton:
            showSettings(window, v->selectedDistro);
            break;
        case TreeCheck:
        case ListenerCheck:
            render(*v);
            break;
        case InspectButton:
            inspect(*v);
            break;
        case ActionsButton: {
            RECT r{};
            GetWindowRect(v->actions, &r);
            menu(*v, {r.left, r.bottom});
            break;
        }
        case ExportButton:
            saveText(window, v->table().exportText(),
                     v->page == 0   ? L"wsl-processes.tsv"
                     : v->page == 1 ? L"wsl-connections.tsv"
                                    : L"wsl-services.tsv");
            break;
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto hdr = reinterpret_cast<NMHDR *>(lparam);
        if (hdr->hwndFrom == v->tabs && hdr->code == TCN_SELCHANGE)
        {
            switchPage(*v);
            return 0;
        }
        for (auto table : {&v->processes, &v->connections, &v->services})
            if (table->notify(hdr))
                return 0;
        if (hdr->hwndFrom == v->table().window)
        {
            if (hdr->code == NM_DBLCLK)
            {
                inspect(*v);
                return 0;
            }
            if (hdr->code == LVN_ITEMCHANGED)
            {
                updateButtons(*v);
                return 0;
            }
        }
        break;
    }
    case WM_CONTEXTMENU: {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (p.x == -1)
        {
            RECT r{};
            GetWindowRect(v->table().window, &r);
            p = {r.left + 30, r.top + 50};
        }
        menu(*v, p);
        return 0;
    }
    case ReplyMessage: {
        std::unique_ptr<Reply> reply(reinterpret_cast<Reply *>(lparam));
        if ((reply->tag >> 16) != v->epoch)
            return 0;
        v->pending = false;
        auto tag = reply->tag & 0xffff;
        if (!reply->error.empty())
        {
            v->failed = true;
            status(*v, L"Disconnected: " + wide(reply->error) + L"  ·  Press Refresh to reconnect.");
            if (tag == ActionTag)
                errorBox(window, wide(reply->error));
            return 0;
        }
        try
        {
            if (tag == DiscoverTag)
            {
                auto old = v->selectedDistro;
                SendMessageW(v->distro, CB_RESETCONTENT, 0, 0);
                int chosen = -1, index = 0;
                for (auto &name : reply->data)
                {
                    auto value = wide(name.get<std::string>());
                    SendMessageW(v->distro, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
                    if (value == old)
                        chosen = index;
                    ++index;
                }
                if (!index)
                {
                    clearDistro(*v);
                    v->selectedDistro.clear();
                    v->failed = true;
                    status(*v, L"No running WSL2 distributions. Start a distro, then press Refresh.");
                    SetWindowTextW(v->summary, L"No running WSL2 distributions");
                    updateButtons(*v);
                    return 0;
                }
                if (chosen < 0)
                    chosen = 0;
                SendMessageW(v->distro, CB_SETCURSEL, chosen, 0);
                auto distro = windowText(v->distro);
                if (distro != old)
                {
                    clearDistro(*v);
                    v->selectedDistro = distro;
                }
                refresh(*v);
            }
            else if (tag == SnapshotTag)
            {
                updateSnapshot(*v, reply->data);
                render(*v);
            }
            else if (tag == ConnectionsTag)
            {
                v->sockets = reply->data;
                render(*v);
            }
            else if (tag == ServicesTag)
            {
                v->units = reply->data;
                render(*v);
                if (!v->units.value("available", true))
                {
                    status(*v, text(v->units, "message", L"systemd is unavailable in this distribution."));
                    return 0;
                }
            }
            else if (tag == ActionTag)
            {
                status(*v, L"Action completed. Refreshing…");
                refresh(*v);
                return 0;
            }

            if (!v->pending)
            {
                SYSTEMTIME time{};
                GetLocalTime(&time);
                wchar_t stamp[32];
                swprintf_s(stamp, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
                auto statusText = L"Root · " + v->selectedDistro + L" · " +
                                  std::to_wstring(v->table().rows.size()) + L" visible rows · updated " +
                                  stamp;
                if (v->page == 1 && v->sockets.value("inaccessible_processes", 0) > 0)
                    statusText += L" · some socket owners were inaccessible";
                if ((v->page == 0 && v->snapshot.value("processes_truncated", false)) ||
                    (v->page == 1 && v->sockets.value("connections_truncated", false)))
                    statusText += L" · collection limit reached (partial results)";
                status(*v, statusText);
            }
        }
        catch (const std::exception &e)
        {
            v->failed = true;
            status(*v, L"Invalid observer response: " + wide(e.what()));
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window, 1);
        v->mailbox->detach();
        disconnect(v->selectedDistro);
        drainReplies(window);
        return 0;
    case WM_NCDESTROY:
        if (mainView == v)
            mainView = nullptr;
        delete v;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace
} // namespace wsl
extern "C" HWND WslCreateView(HWND parent, HINSTANCE dll)
{
    using namespace wsl;
    instance = dll;
    font = WslGetHostFont();
    if (!font)
        font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW graph{};
    graph.hInstance = instance;
    graph.lpfnWndProc = graphProc;
    graph.lpszClassName = L"WslTools.Graph";
    graph.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&graph);
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = viewProc;
    cls.lpszClassName = viewClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    return CreateWindowExW(WS_EX_CONTROLPARENT, viewClass, L"WSL Tools",
                           WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr, instance,
                           nullptr);
}
extern "C" void WslSetActive(BOOL active)
{
    using namespace wsl;
    if (!mainView)
        return;
    auto &v = *mainView;
    v.active = active != FALSE;
    if (v.active)
    {
        v.failed = false;
        v.previousTime = 0;
        refresh(v);
    }
    else
    {
        disconnect(v.selectedDistro);
        ++v.epoch;
        v.pending = false;
        status(v, L"Collector disconnected while the WSL tab is hidden.");
    }
}
extern "C" void WslShutdown(void)
{
    wsl::stopController();
}
