#include "controller.hpp"
#include "settings.hpp"
#include "view_state.hpp"
#include "graphs.hpp"
#include "host_bridge.h"
#include <cmath>
#include <algorithm>
#include <set>

namespace wsl::ui
{
namespace
{
std::string processKey(const Json &process)
{
    return process.at("pid").dump() + ":" + process.at("start_ticks").dump();
}
std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
bool matches(const Row &row, const std::wstring &query)
{
    std::wstring combined;
    for (const auto &cell : row.cells)
        combined += cell + L" ";
    if (WslHasGlobalSearch())
        return WslMatchesGlobalSearch(combined.c_str()) != FALSE;
    if (query.empty())
        return true;
    combined = lower(std::move(combined));
    // Fallback filtering is available when ToolStatus's search box is disabled.
    size_t begin = 0;
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
} // namespace

std::string connectionKey(const Json &c)
{
    return Json::array({c.value("protocol", ""), c.value("inode", 0ull), c.value("pid", 0),
                        c.value("start_ticks", 0ull), c.value("local_address", ""), c.value("local_port", 0),
                        c.value("remote_address", ""), c.value("remote_port", 0)})
        .dump();
}

void clearDistro(View &v)
{
    ++v.epoch;
    v.pending = false;
    v.refreshAfterPending = false;
    v.renderedRevision = UINT64_MAX;
    v.renderedSnapshotTime = 0;
    v.capture = std::make_shared<CaptureModel>();
    v.sockets = Json();
    v.units = Json();
    v.processes.clear();
    v.connections.clear();
    v.services.clear();
    v.collectConnections = false;
    v.collectServices = false;
    v.pendingSelection.clear();
    v.pendingExecutable.clear();
    v.pendingService.clear();
    v.refreshServiceMetadata = true;
    status(v, L"Connecting to the selected distribution…");
    PostMessageW(v.graph, GraphSampleChanged, 0, 0);
    PostMessageW(v.memoryGraph, GraphSampleChanged, 0, 0);
}
void updateButtons(View &v)
{
    EnableWindow(v.settings, TRUE);
    EnableWindow(v.exportButton, !v.table().rows.empty());
}
void render(View &v, uintptr_t changed)
{
    const auto rateValue = [](const std::map<std::string, double> &rates, const std::string &key) {
        const auto found = rates.find(key);
        return found == rates.end() ? 0.0 : found->second;
    };
    v.cpuPercentOfTotal = readSetting(L"CpuPercentOfTotal", 1) != 0;
    const double cpuDivisor = v.cpuPercentOfTotal && v.capture->snapshot.is_object()
                                  ? std::max(1.0, v.capture->snapshot.value("cpus", 1.0))
                                  : 1.0;
    std::vector<Row> rows;
    const auto query = lower(windowText(v.search));
    if ((!changed || changed == SnapshotTag) && v.capture->snapshot.contains("processes"))
    {
        const auto &items = v.capture->snapshot["processes"];
        const bool detect32Bit = readSetting(L"Detect32BitProcesses", 0) != 0;
        const double hz = std::max(1.0, v.capture->snapshot.value("clock_ticks", 100.0));
        rows.reserve(items.size());
        const bool tree = SendMessageW(v.tree, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool showSmallCpu = WslHostIntegerSetting(L"ShowCpuBelow001") != 0;
        const auto precision = static_cast<int>(std::min(6ul, WslHostIntegerSetting(L"MaxPrecisionUnit")));
        const double cpuThreshold = std::pow(10.0, -precision);
        const bool ownOnly = WslHostIntegerSetting(L"HideOtherUserProcesses") != 0;
        const bool hideSystem = WslHostIntegerSetting(L"HideMicrosoftProcesses") != 0;
        const bool hideWindowsToWsl = readSetting(L"HideWindowsToWslInterop", 1) != 0;
        const bool hideWslToWindows = readSetting(L"HideWslToWindowsInterop", 1) != 0;
        v.processes.setAncestryOrder(tree, tree && (WslHostIntegerSetting(L"SortChildProcesses") ||
                                                    WslHostIntegerSetting(L"SortRootProcesses")));
        auto cpuText = [&](double value) -> std::wstring {
            // Both CPU columns follow the host's precision and tiny-value option.
            if (value >= cpuThreshold)
                return number(value, precision);
            if (value > 0 && showSmallCpu)
                return L"< " + number(value, precision);
            return {};
        };
        for (const auto &p : items)
        {
            auto cpuTime = [&](const char *key) {
                return p.contains(key) ? number(p[key].get<double>() / hz) + L" s" : L"";
            };
            auto key = processKey(p);
            const double cpu = rateValue(v.capture->cpu, key) / cpuDivisor;
            const auto history = v.capture->cpuHistory.find(key);
            const double average =
                history != v.capture->cpuHistory.end() ? history->second.average() / cpuDivisor : 0;
            auto rate = [](double value) {
                return value >= 1 ? bytes(static_cast<uint64_t>(value)) + L"/s" : L"";
            };
            auto optionalBytes = [&](const char *field) {
                return p.contains(field) ? bytes(p[field].get<uint64_t>()) : L"";
            };
            const auto age = static_cast<uint64_t>(std::max(
                0.0, v.capture->snapshot.value("uptime_seconds", 0.0) - p.value("start_ticks", 0.0) / hz));
            wchar_t ageText[64]{};
            swprintf_s(ageText, L"%llu:%02llu:%02llu:%02llu", age / 86400, age / 3600 % 24, age / 60 % 60,
                       age % 60);
            std::wstring name = text(p, "name");
            Row row{{name,
                     text(p, "pid"),
                     text(p, "user"),
                     cpuText(cpu),
                     bytes(p.value("rss_bytes", 0ull)),
                     rate(rateValue(v.capture->readRate, key)),
                     rate(rateValue(v.capture->writeRate, key)),
                     text(p, "state"),
                     text(p, "threads"),
                     text(p, "ppid"),
                     text(p, "command"),
                     text(p, "uid"),
                     text(p, "euid"),
                     text(p, "gid"),
                     text(p, "egid"),
                     text(p, "tty"),
                     text(p, "nice"),
                     text(p, "priority"),
                     ageText,
                     optionalBytes("virtual_bytes"),
                     text(p, "session"),
                     text(p, "pgrp"),
                     text(p, "processor"),
                     text(p, "minor_faults"),
                     text(p, "major_faults"),
                     text(p, "exe"),
                     text(p, "cwd"),
                     text(p, "cgroup"),
                     text(p, "tracer_pid"),
                     optionalBytes("swap_bytes"),
                     optionalBytes("read_bytes"),
                     optionalBytes("write_bytes"),
                     optionalBytes("read_chars"),
                     optionalBytes("write_chars"),
                     text(p, "syscr"),
                     text(p, "syscw"),
                     text(p, "voluntary_switches"),
                     text(p, "involuntary_switches"),
                     text(p, "seccomp"),
                     p.contains("no_new_privs") ? (p.value("no_new_privs", false) ? L"Yes" : L"No") : L"",
                     (detect32Bit && p.contains("is_32bit"))
                         ? (p.value("is_32bit", false) ? L"32-bit" : L"64-bit")
                         : L"",
                     cpuTime("user_ticks"),
                     cpuTime("kernel_ticks"),
                     text(p, "policy"),
                     cpuText(average)},
                    p,
                    key};
            row.numeric = {{ProcessAge, static_cast<double>(age)},
                           {ProcessCpu, cpu},
                           {ProcessCpuAverage, average},
                           {ProcessRss, p.value("rss_bytes", 0.0)},
                           {ProcessRead, rateValue(v.capture->readRate, key)},
                           {ProcessWrite, rateValue(v.capture->writeRate, key)}};
            for (const auto &field :
                 std::initializer_list<std::pair<size_t, const char *>>{{ProcessVirtual, "virtual_bytes"},
                                                                        {ProcessSwap, "swap_bytes"},
                                                                        {ProcessReadTotal, "read_bytes"},
                                                                        {ProcessWriteTotal, "write_bytes"},
                                                                        {ProcessReadChars, "read_chars"},
                                                                        {ProcessWriteChars, "write_chars"}})
                row.numeric[field.first] = p.value(field.second, 0.0);
            row.data["_cpu_percent"] = cpu;
            row.data["_read_rate"] = rateValue(v.capture->readRate, key);
            row.data["_write_rate"] = rateValue(v.capture->writeRate, key);
            rows.push_back(std::move(row));
        }
        if (tree)
        {
            std::set<int> pids, visited;
            std::map<int, std::vector<Row>> children;
            for (const auto &row : rows)
                pids.insert(row.data.value("pid", 0));
            for (auto &row : rows)
            {
                const int parent = row.data.value("ppid", 0);
                children[pids.count(parent) ? parent : 0].push_back(std::move(row));
            }
            const bool sortRoots = WslHostIntegerSetting(L"SortRootProcesses") != 0;
            const bool sortChildren = WslHostIntegerSetting(L"SortChildProcesses") != 0;
            for (auto &[parent, siblings] : children)
                if (parent == 0 ? sortRoots : sortChildren)
                    v.processes.sortRows(siblings);
            rows.clear();
            std::function<void(int, int)> visit = [&](int parent, int depth) {
                for (auto &row : children[parent])
                {
                    const int pid = row.data.value("pid", 0);
                    if (!visited.insert(pid).second)
                        continue;
                    row.cells[0].insert(0, std::min(depth, 12) * 2, L' ');
                    rows.push_back(row);
                    visit(pid, depth + 1);
                }
            };
            visit(0, 0);
            // Races or a PID namespace cycle must not hide surviving processes.
            for (const auto &[parent, siblings] : children)
                for (const auto &row : siblings)
                    if (visited.insert(row.data.value("pid", 0)).second)
                        rows.push_back(row);
        }
        v.processes.replace(
            std::move(rows),
            [query, ownOnly, hideSystem, hideWindowsToWsl, hideWslToWindows,
             uid = v.capture->defaultUid](const Row &row) {
                // Deliberately exact: similarly named executables and longer
                // argv[0] prefixes must not disappear as interop plumbing.
                if (row.data.value("exe", std::string{}) == "/init")
                {
                    const auto command = row.data.value("command", std::string{});
                    if (hideWindowsToWsl && row.data.value("pid", 0) != 1 && command == "/init")
                        return false;
                    if (hideWslToWindows && command.size() > 6 && command.compare(0, 6, "/init ") == 0)
                        return false;
                }
                // A root process started through sudo belongs to Elevated,
                // independently of whether either highlighting color is enabled.
                const bool system = row.data.value("euid", row.data.value("uid", -1)) == 0 &&
                                    !row.data.value("sudo_root", false);
                return (!hideSystem || !system) &&
                       (!ownOnly ||
                        (uid && row.data.contains("euid") && row.data.value("euid", uint32_t(-1)) == *uid)) &&
                       matches(row, query);
            },
            !v.capture->snapshot.value("processes_truncated", false));
    }
    if ((!changed || changed == ConnectionsTag) && v.sockets.contains("connections"))
    {
        rows.clear();
        bool onlyListeners = SendMessageW(v.listeners, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool hideWaiting = WslHostIntegerSetting(L"HideWaitingConnections") != 0;
        for (const auto &c : v.sockets["connections"])
        {
            const auto protocol = text(c, "protocol"), state = text(c, "state");
            Row row{{protocol, text(c, "local_address"), text(c, "local_port"), text(c, "remote_address"),
                     text(c, "remote_port"), state, text(c, "pid"), text(c, "process"), text(c, "inode"),
                     WslHostIntegerSetting(L"EnableNetworkResolve") ? text(c, "remote_hostname") : L""},
                    c,
                    connectionKey(c)};
            rows.push_back(std::move(row));
        }
        v.connections.replace(
            std::move(rows),
            [query, onlyListeners, hideWaiting](const Row &row) {
                const auto protocol = text(row.data, "protocol"), state = text(row.data, "state");
                const bool listening =
                    state == L"LISTEN" || state == L"LISTENING" ||
                    (protocol.find(L"udp") != std::wstring::npos && row.data.value("remote_port", 0) == 0);
                const bool waiting = row.data.value("pid", 0) == 0 ||
                                     (protocol.rfind(L"tcp", 0) == 0 && state == L"CLOSE_WAIT");
                return (!hideWaiting || !waiting) && (!onlyListeners || listening) && matches(row, query);
            },
            !v.sockets.value("connections_truncated", false) &&
                v.sockets.value("inaccessible_processes", 0) == 0);
    }
    if ((!changed || changed == ServicesTag) && v.units.contains("services"))
    {
        rows.clear();
        for (const auto &s : v.units["services"])
        {
            Row row{{text(s, "name"), text(s, "active"), text(s, "sub"), text(s, "enabled"), text(s, "load"),
                     text(s, "description"), s.value("pid", 0) > 0 ? text(s, "pid") : L""},
                    s,
                    s.value("name", "")};
            rows.push_back(std::move(row));
        }
        const bool showInactive = SendMessageW(v.inactiveServices, BM_GETCHECK, 0, 0) == BST_CHECKED;
        v.services.replace(
            std::move(rows),
            [query, showInactive](const Row &row) {
                // Failed units remain visible: failure is distinct from an inactive unit.
                return (showInactive || row.data.value("active", "") != "inactive") && matches(row, query);
            },
            v.units.value("available", true) && !v.units.value("services_truncated", false));
    }
    updateButtons(v);
}
} // namespace wsl::ui
