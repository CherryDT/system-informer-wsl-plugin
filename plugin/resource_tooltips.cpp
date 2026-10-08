#include "resource_tooltips.hpp"
#include "host_bridge.h"
#include <algorithm>

namespace wsl
{
namespace
{
// Keep the same section/indent style as System Informer's itemtips.c. Values
// come from the latest collected row, so hovering never starts a WSL command.
class Tooltip
{
  public:
    explicit Tooltip(const Row &row)
    {
        if (row.removed)
            value = L"Removed object (last collected information)\n\n";
    }
    void section(const wchar_t *title, const std::wstring &content)
    {
        if (content.empty())
            return;
        value += title;
        value += L":\n    ";
        // A command line or unit description can contain newlines/control
        // characters; keep it readable without allowing fake tooltip sections.
        auto clean = content.substr(0, 384);
        std::replace_if(clean.begin(), clean.end(), [](wchar_t ch) { return ch < L' '; }, L' ');
        value += clean;
        if (content.size() > clean.size())
            value += L"...";
        value += L'\n';
    }
    std::wstring finish()
    {
        if (!value.empty() && value.back() == L'\n')
            value.pop_back();
        return std::move(value);
    }

  private:
    std::wstring value;
};
std::wstring endpoint(const Json &data, const char *addressKey, const char *portKey)
{
    auto address = text(data, addressKey);
    if (address.empty())
        return {};
    if (address.find(L':') != std::wstring::npos)
        address = L"[" + address + L"]";
    return address + L":" + text(data, portKey);
}
} // namespace

std::wstring processTooltip(const Row &row)
{
    const auto &data = row.data;
    Tooltip tip(row);
    tip.section(L"Process", text(data, "name") + L" (" + text(data, "pid") + L")");
    auto user = text(data, "user");
    if (data.contains("euid"))
        user += (user.empty() ? L"" : L" ") + std::wstring(L"(UID ") + text(data, "euid") + L")";
    tip.section(L"User", user);
    auto unit = text(data, "service_unit");
    if (!unit.empty())
        unit += data.value("service_scope", "") == "user" ? L" (user service)" : L" (system service)";
    tip.section(L"Service", unit);
    if (data.value("sudo_root", false))
    {
        auto origin = text(data, "sudo_user");
        if (data.contains("sudo_uid"))
            origin += (origin.empty() ? L"" : L" ") + std::wstring(L"(UID ") + text(data, "sudo_uid") + L")";
        if (data.contains("sudo_gid"))
            origin += L", GID " + text(data, "sudo_gid");
        // SUDO_* survives into descendants. Label the source instead of
        // suggesting that we proved a direct sudo parent/child relationship.
        tip.section(L"Sudo origin (environment)", origin);
        if (WslHostIntegerSetting(L"EnableCommandLineTooltips"))
            tip.section(L"Sudo command", text(data, "sudo_command"));
    }
    if (WslHostIntegerSetting(L"EnableCommandLineTooltips"))
        tip.section(L"Command line", text(data, "command"));
    tip.section(L"File", text(data, "exe"));
    tip.section(L"Working directory", text(data, "cwd"));
    if (data.value("tracer_pid", 0) != 0)
        tip.section(L"Debugger", L"Attached tracer: PID " + text(data, "tracer_pid"));
    tip.section(L"Terminal", text(data, "tty"));
    const int policy = data.value("policy", 0);
    auto scheduling = policy == 1   ? L"Real-time FIFO"
                      : policy == 2 ? L"Real-time round robin"
                      : policy == 3 ? L"Batch"
                      : policy == 5 ? L"Idle"
                      : policy == 6 ? L"Deadline"
                                    : L"";
    std::wstring schedulingText = scheduling;
    if (data.value("nice", 0) != 0)
        schedulingText +=
            (schedulingText.empty() ? L"" : L"; ") + std::wstring(L"nice ") + text(data, "nice");
    tip.section(L"Scheduling", schedulingText);
    std::wstring restrictions;
    const auto seccomp = text(data, "seccomp");
    if (seccomp == L"1")
        restrictions = L"Strict seccomp";
    else if (seccomp == L"2")
        restrictions = L"Seccomp filter";
    if (data.value("no_new_privs", false))
        restrictions += (restrictions.empty() ? L"" : L"; ") + std::wstring(L"no new privileges");
    tip.section(L"Restrictions", restrictions);
    return tip.finish();
}

std::wstring serviceTooltip(const Row &row)
{
    const auto &data = row.data;
    Tooltip tip(row);
    tip.section(L"Service", text(data, "name"));
    tip.section(L"Description", text(data, "description"));
    const auto name = text(data, "name");
    if (name.size() >= 9 && name.compare(name.size() - 9, 9, L"@.service") == 0)
        tip.section(L"Template", L"Instantiate this unit with a name after @ to run it.");
    const auto startup = text(data, "enabled");
    if (startup == L"masked" || startup == L"masked-runtime")
        tip.section(L"Startup", L"Masked: systemd refuses activation until the unit is unmasked.");
    else if (startup == L"static")
        tip.section(
            L"Startup",
            L"Static: can be started explicitly or pulled in by another unit; cannot be enabled directly.");
    else if (startup == L"disabled")
        tip.section(L"Startup",
                    L"Disabled: not enabled for automatic startup, but other units may still activate it.");
    if (text(data, "active") == L"failed")
        tip.section(L"Diagnostics", L"Open the service's Journal tab to inspect its failure.");
    else if (text(data, "sub") == L"exited")
        tip.section(L"Execution",
                    L"The start command has exited; the unit may remain active without a running process.");
    if (data.value("pid", 0) > 0)
        tip.section(L"Process", L"Main PID " + text(data, "pid") + L" (Go to process is available).");
    return tip.finish();
}

std::wstring networkTooltip(const Row &row)
{
    const auto &data = row.data;
    Tooltip tip(row);
    auto process = text(data, "process");
    if (data.value("pid", 0) > 0)
        process += L" (" + text(data, "pid") + L")";
    else
        process = L"Unknown process";
    tip.section(L"Process", process);
    const bool unixSocket = data.value("protocol", "") == "unix";
    const auto local = text(data, "local_address");
    tip.section(unixSocket ? L"Socket path" : L"Local endpoint",
                unixSocket ? local : endpoint(data, "local_address", "local_port"));
    if (unixSocket)
    {
        if (!local.empty() && local.front() == L'@')
            tip.section(L"Namespace", L"Abstract Unix socket: the name is not a filesystem path.");
        else if (local.empty())
            tip.section(L"Namespace", L"Unnamed Unix socket.");
    }
    else
    {
        if (data.value("remote_port", 0) != 0)
            tip.section(L"Remote endpoint", endpoint(data, "remote_address", "remote_port"));
        if (WslHostIntegerSetting(L"EnableNetworkResolve"))
            tip.section(L"Remote hostname", text(data, "remote_hostname"));
        if (local == L"0.0.0.0" || local == L"::")
            tip.section(L"Binding", L"All local addresses in this network namespace.");
        else if (local.rfind(L"127.", 0) == 0 || local == L"::1")
            tip.section(L"Binding", L"Loopback address in this network namespace.");
        const auto state = text(data, "state");
        if (state == L"TIME_WAIT")
            tip.section(L"Connection",
                        L"Closed connection retained by the kernel; a process owner may no longer exist.");
        else if (state == L"CLOSE_WAIT")
            tip.section(
                L"Connection",
                L"The peer has closed its side; the local application has not closed the socket yet.");
    }
    if (data.value("pid", 0) == 0 && text(data, "state") != L"TIME_WAIT")
        tip.section(L"Ownership", L"No owning file descriptor was found in the collected processes.");
    return tip.finish();
}
} // namespace wsl
