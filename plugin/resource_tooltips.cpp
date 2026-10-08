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
    if (WslHostIntegerSetting(L"EnableCommandLineTooltips"))
        tip.section(L"Command line", text(data, "command"));
    tip.section(L"File", text(data, "exe"));
    auto state = text(data, "state");
    if (data.value("is_suspended", false))
        state = L"Suspended";
    else if (data.value("is_partially_suspended", false))
        state = L"Partially suspended";
    if (data.value("tracer_pid", 0) != 0)
        state += L"; traced by PID " + text(data, "tracer_pid");
    tip.section(L"State", state);
    std::wstring usage;
    if (data.contains("_cpu_percent"))
        usage = L"CPU: " + number(data["_cpu_percent"].get<double>()) + L"%";
    if (data.contains("rss_bytes"))
        usage += (usage.empty() ? L"" : L"; ") + std::wstring(L"Resident memory: ") +
                 bytes(data["rss_bytes"].get<uint64_t>());
    tip.section(L"Usage", usage);
    tip.section(L"Parent PID", text(data, "ppid"));
    tip.section(L"Working directory", text(data, "cwd"));
    return tip.finish();
}

std::wstring serviceTooltip(const Row &row)
{
    const auto &data = row.data;
    Tooltip tip(row);
    tip.section(L"Service", text(data, "name"));
    tip.section(L"Description", text(data, "description"));
    auto state = text(data, "active");
    const auto sub = text(data, "sub");
    if (!sub.empty())
        state += L" (" + sub + L")";
    tip.section(L"State", state);
    tip.section(L"Startup", text(data, "enabled"));
    tip.section(L"Load state", text(data, "load"));
    if (data.value("pid", 0) > 0)
        tip.section(L"Main PID", text(data, "pid"));
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
    tip.section(L"Protocol", text(data, "protocol"));
    const bool unixSocket = data.value("protocol", "") == "unix";
    tip.section(unixSocket ? L"Path" : L"Local address",
                unixSocket ? text(data, "local_address") : endpoint(data, "local_address", "local_port"));
    if (!unixSocket)
    {
        tip.section(L"Remote address", endpoint(data, "remote_address", "remote_port"));
        if (WslHostIntegerSetting(L"EnableNetworkResolve"))
            tip.section(L"Remote hostname", text(data, "remote_hostname"));
    }
    tip.section(L"State", text(data, "state"));
    tip.section(L"Socket inode", text(data, "inode"));
    return tip.finish();
}
} // namespace wsl
