#include "controller.hpp"
#include "view_state.hpp"

namespace wsl::ui
{
void inspect(View &v)
{
    const Row *row = v.table().selectedActionable();
    if (!row)
        return;
    if (v.page == 0)
        openDetails(v.window, v.selectedDistro, row->data);
    else if (v.page == 2)
        openServiceDetails(v.window, v.selectedDistro, row->data.value("name", ""));
    else
    {
        goToProcess(v, row->data);
    }
}
void goToProcess(View &v, const Json &process)
{
    int pid = process.value("pid", 0);
    if (!pid)
    {
        errorBox(v.window, L"No running process was identified. It may have exited or belong to "
                           L"another PID namespace.");
        return;
    }
    if (!process.contains("start_ticks") || process["start_ticks"].is_null())
    {
        errorBox(v.window, L"The process identity is unavailable. Refresh this view.");
        return;
    }
    v.pendingSelection = std::to_string(pid) + ":" + process["start_ticks"].dump();
    WslClearGlobalSearch();
    SetWindowTextW(v.search, L"");
    selectPage(v, 0);
    v.processes.selectKey(v.pendingSelection);
    int index = ListView_GetNextItem(v.processes.window, -1, LVNI_SELECTED);
    if (index >= 0)
    {
        ListView_EnsureVisible(v.processes.window, index, FALSE);
        v.pendingSelection.clear();
    }
    SetFocus(v.processes.window);
}
void action(View &v, int id)
{
    const Row *selected = v.table().selected();
    if (!selected)
        return;
    Row row = *selected;
    if (id == Inspect)
    {
        inspect(v);
        return;
    }
    if (id == GoToProcess && !row.removed)
    {
        if (v.page != 2 || row.data.contains("pid"))
            goToProcess(v, row.data);
        else if (!v.pending && v.active && !v.failed)
        {
            // A hidden PID column avoids polling MainPID; fetch it on demand
            // when the user explicitly asks to navigate to the service process.
            v.pendingService = row.data.value("name", "");
            queue(v, {{"op", "services"}, {"include_pids", true}}, ServiceProcessTag);
            status(v, L"Resolving the service process…");
        }
        else
            errorBox(v.window, L"Wait for the current refresh to finish, then try again.");
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
    if (row.removed)
        return;
    if (v.pending)
    {
        errorBox(v.window, L"A request is still running. Wait for it to finish, then try the action again.");
        return;
    }
    if (!v.active || v.failed)
    {
        errorBox(v.window, L"Refresh before changing a process or service.");
        return;
    }
    if (id == OpenExecutable)
    {
        v.pendingExecutable = row.key;
        queue(v,
              {{"op", "snapshot"},
               {"fields", Json::array({"exe"})},
               {"pid", row.data["pid"]},
               {"start_ticks", row.data["start_ticks"]}},
              ExecutableTag);
        status(v, L"Resolving the process executable…");
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
        case Hangup:
            signal = 1;
            label = L"Send SIGHUP";
            break;
        case WindowChanged:
            signal = 28;
            label = L"Send SIGWINCH";
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
            signal == 1 || signal == 10 || signal == 12
                ? L"The program defines this signal's behavior. Without a handler, it terminates the process."
                : L"This action runs as Linux root. Termination may lose unsaved work.";
        const auto executable = text(row.data, "exe");
        const auto basename = executable.substr(executable.find_last_of(L'/') + 1);
        if ((signal == 9 || signal == 15 || signal == 19) &&
            (basename == L"wsl-observer" || text(row.data, "name") == L"wsl-observer"))
            prompt += L"\r\n\r\nThis is the WSL inspection component. Stopping it disconnects WSL "
                      L"monitoring and may interrupt another inspection. Use View > Refresh to reconnect.";
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
    AppendMenuW(popup, MF_STRING, Inspect, v.page == 1 ? L"Go to process\tEnter" : L"Inspect…\tEnter");
    SetMenuDefaultItem(popup, Inspect, FALSE);
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
        AppendMenuW(popup, MF_STRING, Hangup, L"Send SIGHUP");
        AppendMenuW(popup, MF_STRING, WindowChanged, L"Send SIGWINCH");
        AppendMenuW(popup, MF_STRING, User1, L"Send SIGUSR1");
        AppendMenuW(popup, MF_STRING, User2, L"Send SIGUSR2");
    }
    else if (v.page == 2)
    {
        const auto row = v.table().selectedActionable();
        const bool canNavigate = row && (!row->data.contains("pid") || row->data.value("pid", 0) > 0);
        AppendMenuW(popup, MF_STRING | (canNavigate ? 0 : MF_GRAYED), GoToProcess, L"Go to process");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, StartService, L"Start");
        AppendMenuW(popup, MF_STRING, StopService, L"Stop");
        AppendMenuW(popup, MF_STRING, RestartService, L"Restart");
        AppendMenuW(popup, MF_STRING, ReloadService, L"Reload configuration");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, EnableService, L"Enable at boot");
        AppendMenuW(popup, MF_STRING, DisableService, L"Disable at boot");
    }
    if (!v.table().selectedActionable())
        for (int id : {Inspect, OpenExecutable, Terminate, Kill, Suspend, Resume, Hangup, WindowChanged,
                       User1, User2, StartService, StopService, RestartService, ReloadService, EnableService,
                       DisableService, GoToProcess})
            EnableMenuItem(popup, id, MF_BYCOMMAND | MF_GRAYED);
    int chosen =
        TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, v.window, nullptr);
    DestroyMenu(popup);
    if (chosen)
        action(v, chosen);
}
} // namespace wsl::ui
