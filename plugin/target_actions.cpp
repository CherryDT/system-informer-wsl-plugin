#include "target_actions.hpp"
#include "resource_dialog.hpp"
#include "process_rules.hpp"

namespace wsl
{
namespace
{
constexpr wchar_t MenuProperty[] = L"WslTools.TargetOptions.Active";
enum Command
{
    CopyCommand = 1,
    OpenLocation,
    FirstSignal = 100,
    FirstService = 200,
    ForceScriptStacks = 300
};

struct SignalAction
{
    int number;
    const wchar_t *label;
    const wchar_t *confirmation;
};
constexpr SignalAction Signals[] = {
    {15, L"Terminate — SIGTERM", L"Send SIGTERM (graceful termination)"},
    {2, L"Interrupt — SIGINT", L"Send SIGINT (interrupt)"},
    {9, L"Force kill — SIGKILL", L"Send SIGKILL (force termination)"},
    {19, L"Suspend — SIGSTOP", L"Send SIGSTOP (suspend)"},
    {18, L"Resume — SIGCONT", L"Send SIGCONT (resume)"},
    {1, L"Send SIGHUP", L"Send SIGHUP"},
    {28, L"Send SIGWINCH", L"Send SIGWINCH"},
    {10, L"Send SIGUSR1", L"Send SIGUSR1"},
    {12, L"Send SIGUSR2", L"Send SIGUSR2"},
};
struct ServiceAction
{
    const char *verb;
    const wchar_t *label;
};
constexpr ServiceAction Services[] = {
    {"start", L"Start"},           {"stop", L"Stop"},
    {"restart", L"Restart"},       {"reload", L"Reload configuration"},
    {"enable", L"Enable at boot"}, {"disable", L"Disable at boot"},
};

// Menus and confirmations pump messages: the owner can close or be refreshed
// during either. A window property also guards against a recycled HWND and
// prevents a second target menu from starting on the same inspector.
class MenuLifetime
{
  public:
    explicit MenuLifetime(HWND owner) : window(owner)
    {
        installed = SetPropW(window, MenuProperty, this) != FALSE;
    }
    ~MenuLifetime()
    {
        if (valid())
            RemovePropW(window, MenuProperty);
    }
    bool valid() const
    {
        return installed && IsWindow(window) && GetPropW(window, MenuProperty) == this;
    }

  private:
    HWND window;
    bool installed = false;
};

bool availablePath(const std::wstring &path)
{
    constexpr wchar_t Deleted[] = L" (deleted)";
    constexpr size_t deletedLength = std::size(Deleted) - 1;
    return !path.empty() && path.front() == L'/' &&
           (path.size() < deletedLength ||
            path.compare(path.size() - deletedLength, deletedLength, Deleted) != 0);
}

bool hasProcessIdentity(const Json &process)
{
    return process.contains("pid") && process["pid"].is_number_integer() &&
           process["pid"].get<int64_t>() > 0 && process.contains("start_ticks") &&
           process["start_ticks"].is_number_integer();
}
} // namespace

void appendProcessSchedulingMenu(HMENU menu, bool enabled, const std::wstring &distro, const Json &process)
{
    const UINT flags = MF_STRING | (enabled ? 0 : MF_GRAYED);
    const auto executable = utf8(text(process, "exe"));
    const auto add = [&](UINT id, const wchar_t *label, const char *action) {
        const bool saved = !executable.empty() && !savedSchedulingInputs(distro, executable, action).empty();
        AppendMenuW(menu, flags | (saved ? MF_CHECKED : 0), id, label);
    };
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(ProcessNice, L"Niceness…", "set_nice");
    add(ProcessAffinity, L"CPU affinity…", "set_affinity");
    add(ProcessPolicy, L"Scheduling policy…", "set_policy");
    add(ProcessIoPriority, L"I/O priority…", "set_io_priority");
    if (!executable.empty() && hasSavedScheduling(distro, executable))
        AppendMenuW(menu, flags, RemoveSavedScheduling, L"Remove saved settings");
}

bool openProcessScheduling(HWND owner, const std::wstring &distro, const Json &process, int command)
{
    if (command < ProcessNice || command > RemoveSavedScheduling)
        return false;
    if (!IsWindow(owner))
        return true;

    // The resource window has its own mailbox. Capture the identity now so a
    // refresh, PID reuse, or a different row selection cannot retarget the change.
    const Json target = process;
    const auto distribution = distro;
    const auto executable = utf8(text(target, "exe"));
    if (command == RemoveSavedScheduling)
    {
        if (!executable.empty())
        {
            try
            {
                removeSavedScheduling(distribution, executable);
            }
            catch (const std::exception &error)
            {
                errorBox(owner, wide(error.what()));
                return true;
            }
            if (IsWindow(owner))
                PostMessageW(owner, ResourceActionCompleted, 0, 0);
        }
        return true;
    }
    if (!hasProcessIdentity(target))
    {
        errorBox(owner, L"The process identity is unavailable. Refresh this view.");
        return true;
    }
    Json request{{"op", "thread"},
                 {"all_threads", true},
                 {"pid", target["pid"]},
                 {"start_ticks", target["start_ticks"]}};
    if (!executable.empty() && executable.front() == '/')
        request["expected_exe"] = executable;
    std::wstring title;
    std::vector<ResourceInput> inputs;
    switch (command)
    {
    case ProcessNice:
        request["action"] = "set_nice";
        title = L"Process niceness";
        inputs = {{L"Niceness (-20 to 19)", "nice", text(target, "nice", L"0")}};
        break;
    case ProcessAffinity:
        request["action"] = "set_affinity";
        title = L"Process CPU affinity";
        inputs = {{L"CPU list (for example 0-3,5)", "cpus", L""}};
        break;
    case ProcessPolicy:
        request["action"] = "set_policy";
        title = L"Process scheduling policy";
        inputs = {{L"Scheduling policy", "policy",
                   target.value("policy", 0) == 3   ? L"batch"
                   : target.value("policy", 0) == 5 ? L"idle"
                                                    : L"other"}};
        break;
    case ProcessIoPriority:
        request["action"] = "set_io_priority";
        title = L"Process I/O priority";
        inputs = {{L"I/O class", "class", L"best-effort"}, {L"Level (0 highest, 7 lowest)", "level", L"4"}};
        break;
    }
    title += L" — all current threads — " + text(target, "name") + L" (" + text(target, "pid") + L" @ " +
             distribution + L")";
    if (IsWindow(owner))
        openResourceTool(owner, distribution, std::move(request), title, std::move(inputs), true);
    return true;
}

TargetOptionsResult targetOptions(HWND owner, HWND anchor, const std::wstring &distro, const Json &process,
                                  const std::string &service, bool busy, bool scriptStacksForced)
{
    if (!IsWindow(owner) || !IsWindow(anchor) || GetPropW(owner, MenuProperty))
        return {};
    MenuLifetime lifetime(owner);
    if (!lifetime.valid())
        return {};

    // Never retain references into an inspector across a nested message loop.
    const Json target = process;
    const auto distribution = distro;
    const auto unit = service;
    const bool isService = !unit.empty();
    const auto path = text(target, isService ? "fragment_path" : "exe");
    const auto commandLine = text(target, isService ? "exec_start" : "command");
    const bool processIdentity = hasProcessIdentity(target);
    const bool serviceTemplate = unit.size() >= 9 && unit.compare(unit.size() - 9, 9, "@.service") == 0;
    const bool canChange = !busy && (isService ? !serviceTemplate : processIdentity);

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return {};
    AppendMenuW(menu, MF_STRING | (availablePath(path) ? 0 : MF_GRAYED), OpenLocation,
                isService ? L"Show unit file in Explorer" : L"Show executable in Explorer");
    AppendMenuW(menu, MF_STRING | (commandLine.empty() ? MF_GRAYED : 0), CopyCommand, L"Copy command line");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const UINT actionFlags = MF_STRING | (canChange ? 0 : MF_GRAYED);
    if (isService)
    {
        for (size_t i = 0; i < std::size(Services); ++i)
        {
            if (i == 4)
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, actionFlags, FirstService + i, Services[i].label);
        }
    }
    else
    {
        for (size_t i = 0; i < std::size(Signals); ++i)
            AppendMenuW(menu, actionFlags, FirstSignal + i, Signals[i].label);
        appendProcessSchedulingMenu(menu, canChange, distribution, target);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (scriptStacksForced ? MF_GRAYED : 0), ForceScriptStacks,
                    L"Force-show script stacks");
    }

    RECT button{};
    GetWindowRect(anchor, &button);
    TPMPARAMS placement{sizeof(placement)};
    placement.rcExclude = button;
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL,
                                          button.left, button.bottom, owner, &placement);
    DestroyMenu(menu);
    if (!lifetime.valid())
        return {};
    if (command == ForceScriptStacks && !isService && !scriptStacksForced)
        return {std::nullopt, true};
    if (command == OpenLocation && availablePath(path))
        openLinuxPath(owner, distribution, path);
    else if (command == CopyCommand && !commandLine.empty())
        copyText(owner, commandLine);
    else if (canChange && !isService && openProcessScheduling(owner, distribution, target, command))
        return {};
    else if (canChange && isService && command >= FirstService &&
             command < FirstService + std::size(Services))
    {
        const auto &action = Services[command - FirstService];
        auto prompt = wide(action.verb) + L" " + wide(unit) + L" in " + distribution +
                      L"?\r\n\r\nThis changes a system service as Linux root.";
        if (MessageBoxW(owner, prompt.c_str(), L"Confirm service action",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES &&
            lifetime.valid())
            return {Json{{"op", "service_action"}, {"name", unit}, {"action", action.verb}}, false};
    }
    else if (canChange && !isService && command >= FirstSignal && command < FirstSignal + std::size(Signals))
    {
        const auto &action = Signals[command - FirstSignal];
        const int signal = action.number;
        auto prompt = std::wstring(action.confirmation) + L" to " + text(target, "name") + L" (PID " +
                      text(target, "pid") + L") in " + distribution + L"?\r\n\r\n";
        prompt += L"This action runs as Linux root.";
        if (signal == 2 || signal == 9 || signal == 15)
            prompt += L" Termination may lose unsaved work.";
        else if (signal == 1 || signal == 10 || signal == 12)
            prompt +=
                L" The program defines this signal's behavior. Without a handler, it terminates the process.";
        else if (signal == 19)
            prompt += L" The process will pause until it is resumed.";
        const auto basename = path.substr(path.find_last_of(L'/') + 1);
        if ((signal == 2 || signal == 9 || signal == 15 || signal == 19) &&
            (basename == L"wsl-observer" || text(target, "name") == L"wsl-observer"))
            prompt += L"\r\n\r\nThis is the WSL inspection component. Stopping it disconnects WSL "
                      L"monitoring and may interrupt another inspection. Use View > Refresh to reconnect.";
        if (MessageBoxW(owner, prompt.c_str(), L"Confirm process signal",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES &&
            lifetime.valid())
            return {Json{{"op", "signal"},
                         {"pid", target["pid"]},
                         {"start_ticks", target["start_ticks"]},
                         {"signal", signal}},
                    false};
    }
    return {};
}
} // namespace wsl
