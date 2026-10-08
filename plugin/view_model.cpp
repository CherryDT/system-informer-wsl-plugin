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
    if (!v.selectedDistro.empty())
        disconnect(v.selectedDistro);
    ++v.epoch;
    v.pending = false;
    v.failed = false;
    v.componentMissing = false;
    v.previous.clear();
    v.cpu.clear();
    v.cpuHistory.clear();
    v.readRate.clear();
    v.writeRate.clear();
    v.previousTime = 0;
    v.bootId.clear();
    v.graphSamples.clear();
    v.graphSequence = 0;
    v.lastGraphTick = 0;
    v.snapshot = Json();
    v.sockets = Json();
    v.units = Json();
    v.processes.clear();
    v.connections.clear();
    v.services.clear();
    v.statistics.clear();
    v.defaultUid.reset();
    v.collectConnections = false;
    v.collectServices = false;
    v.pendingExecutable.clear();
    v.pendingService.clear();
    v.refreshServiceMetadata = true;
    v.newProcess.clear();
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
    v.cpuPercentOfTotal = readSetting(L"CpuPercentOfTotal", 1) != 0;
    const double cpuDivisor =
        v.cpuPercentOfTotal && v.snapshot.is_object() ? std::max(1.0, v.snapshot.value("cpus", 1.0)) : 1.0;
    std::vector<Row> rows;
    const auto query = lower(windowText(v.search));
    if ((!changed || changed == SnapshotTag) && v.snapshot.contains("processes"))
    {
        const auto &items = v.snapshot["processes"];
        const bool detect32Bit = readSetting(L"Detect32BitProcesses", 0) != 0;
        const double hz = std::max(1.0, v.snapshot.value("clock_ticks", 100.0));
        rows.reserve(items.size());
        const bool tree = SendMessageW(v.tree, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool showSmallCpu = WslHostIntegerSetting(L"ShowCpuBelow001") != 0;
        const auto precision = static_cast<int>(std::min(6ul, WslHostIntegerSetting(L"MaxPrecisionUnit")));
        const double cpuThreshold = std::pow(10.0, -precision);
        const bool ownOnly = WslHostIntegerSetting(L"HideOtherUserProcesses") != 0;
        const bool hideSystem = WslHostIntegerSetting(L"HideMicrosoftProcesses") != 0;
        const bool hideWindowsToWsl = readSetting(L"HideWindowsToWslInterop", 0) != 0;
        const bool hideWslToWindows = readSetting(L"HideWslToWindowsInterop", 0) != 0;
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
            const double cpu = v.cpu[key] / cpuDivisor;
            const auto history = v.cpuHistory.find(key);
            const double average = history != v.cpuHistory.end() ? history->second.average() / cpuDivisor : 0;
            auto rate = [](double value) {
                return value >= 1 ? bytes(static_cast<uint64_t>(value)) + L"/s" : L"";
            };
            auto optionalBytes = [&](const char *field) {
                return p.contains(field) ? bytes(p[field].get<uint64_t>()) : L"";
            };
            const auto age = static_cast<uint64_t>(
                std::max(0.0, v.snapshot.value("uptime_seconds", 0.0) - p.value("start_ticks", 0.0) / hz));
            wchar_t ageText[64]{};
            swprintf_s(ageText, L"%llu:%02llu:%02llu:%02llu", age / 86400, age / 3600 % 24, age / 60 % 60,
                       age % 60);
            std::wstring name = text(p, "name");
            Row row{{name,
                     text(p, "pid"),
                     text(p, "user"),
                     cpuText(cpu),
                     bytes(p.value("rss_bytes", 0ull)),
                     rate(v.readRate[key]),
                     rate(v.writeRate[key]),
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
            row.numeric = {{ProcessAge, static_cast<double>(age)}, {ProcessCpu, cpu},
                           {ProcessCpuAverage, average},           {ProcessRss, p.value("rss_bytes", 0.0)},
                           {ProcessRead, v.readRate[key]},         {ProcessWrite, v.writeRate[key]}};
            for (const auto &field :
                 std::initializer_list<std::pair<size_t, const char *>>{{ProcessVirtual, "virtual_bytes"},
                                                                        {ProcessSwap, "swap_bytes"},
                                                                        {ProcessReadTotal, "read_bytes"},
                                                                        {ProcessWriteTotal, "write_bytes"},
                                                                        {ProcessReadChars, "read_chars"},
                                                                        {ProcessWriteChars, "write_chars"}})
                row.numeric[field.first] = p.value(field.second, 0.0);
            row.data["_cpu_percent"] = cpu;
            row.data["_read_rate"] = v.readRate[key];
            row.data["_write_rate"] = v.writeRate[key];
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
             uid = v.defaultUid](const Row &row) {
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
            !v.snapshot.value("processes_truncated", false));
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
        v.services.replace(
            std::move(rows), [query](const Row &row) { return matches(row, query); },
            v.units.value("available", true) && !v.units.value("services_truncated", false));
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
        v.cpuHistory.clear();
        v.previousTime = 0;
        v.graphSamples.clear();
        v.graphSequence = 0;
        v.bootId = boot;
    }
    // Store raw Linux CPU percentages (100% = one vCPU). The display setting
    // only scales rendered rows; history always measures total guest capacity.
    double elapsed = v.previousTime ? (now - v.previousTime) / 1000.0 : 0;
    double hz = data.value("clock_ticks", 100.0), total = 0;
    GraphSample graph;
    GetSystemTimeAsFileTime(&graph.timestamp);
    graph.interval = elapsed;
    const size_t historyCapacity = std::max(1ul, WslHostIntegerSetting(L"SampleCount"));
    std::map<std::string, ProcessSample> samples;
    v.newProcess.clear();
    v.cpu.clear();
    v.readRate.clear();
    v.writeRate.clear();
    for (const auto &p : data.at("processes"))
    {
        auto key = processKey(p);
        ProcessSample sample{p.value("cpu_ticks", 0ull), p.value("read_bytes", 0ull),
                             p.value("write_bytes", 0ull),
                             p.contains("read_bytes") && p.contains("write_bytes")};
        double usage = 0, read = 0, written = 0;
        auto previous = v.previous.find(key);
        if (v.previousTime && previous == v.previous.end())
            v.newProcess = key;
        if (elapsed > 0 && hz > 0 && previous != v.previous.end())
        {
            auto &old = previous->second;
            if (sample.ticks >= old.ticks)
            {
                usage = 100.0 * (sample.ticks - old.ticks) / hz / elapsed;
                // A baseline-only reading and manual refreshes while capture is
                // paused are not CPU history samples. Neither adds a false zero.
                if (!v.paused)
                    v.cpuHistory[key].append(usage, historyCapacity);
            }
            else
                v.cpuHistory.erase(key); // Counter reset: start a fresh history.
            if (sample.hasIo && old.hasIo && sample.read >= old.read)
                read = (sample.read - old.read) / elapsed;
            if (sample.hasIo && old.hasIo && sample.written >= old.written)
                written = (sample.written - old.written) / elapsed;
        }
        samples[key] = sample;
        v.cpu[key] = usage;
        v.readRate[key] = read;
        v.writeRate[key] = written;
        total += usage;
        if (usage > graph.topCpu)
        {
            graph.topCpu = usage;
            graph.topName = text(p, "name");
            graph.topPid = p.value("pid", 0);
        }
        if (p.value("rss_bytes", 0ull) > graph.largestRss)
        {
            graph.largestRss = p.value("rss_bytes", 0ull);
            graph.largestRssName = text(p, "name");
            graph.largestRssPid = p.value("pid", 0);
        }
    }
    // Do not retain histories for exited processes, or transfer one to a reused PID.
    for (auto it = v.cpuHistory.begin(); it != v.cpuHistory.end();)
        if (samples.count(it->first))
            ++it;
        else
            it = v.cpuHistory.erase(it);
    v.previous = std::move(samples);
    v.previousTime = now;
    // Cheap background samples omit expensive fields. Retain the last known
    // values only for the same PID/start-time identity; PID reuse never inherits
    // another process's command line, credentials or highlighting metadata.
    std::map<std::string, Json> prior;
    if (v.snapshot.contains("processes"))
        for (auto &process : v.snapshot["processes"])
        {
            auto key = processKey(process);
            prior.emplace(std::move(key), std::move(process));
        }
    v.snapshot = data;
    std::set<std::string> requested;
    for (const auto &field : data.value("fields", Json::array()))
        requested.insert(field.get<std::string>());
    if (requested.count("user") || requested.count("sudo"))
        requested.insert("status");
    const bool allFields = !data.contains("fields");
    for (auto &process : v.snapshot["processes"])
    {
        auto previous = prior.find(processKey(process));
        if (previous != prior.end())
        {
            Json merged = std::move(previous->second);
            // Requested-but-unavailable fields must become unknown, not retain
            // a pre-exec image/credential value. Unrequested fields are cached.
            auto dropGroup = [&](const char *group, std::initializer_list<const char *> keys) {
                for (const auto key : keys)
                    if (allFields || requested.count(group) || requested.count(key))
                        merged.erase(key);
            };
            dropGroup("status", {"uid", "euid", "gid", "egid", "status_accessible", "tracer_pid",
                                 "voluntary_switches", "involuntary_switches", "seccomp", "no_new_privs",
                                 "capabilities", "swap_bytes", "is_own"});
            dropGroup("user", {"user"});
            dropGroup("io", {"read_bytes", "write_bytes", "read_chars", "write_chars", "syscr", "syscw",
                             "cancelled_write_bytes", "io_accessible"});
            dropGroup("exe", {"exe", "runtime"});
            dropGroup("cwd", {"cwd"});
            dropGroup("command", {"command"});
            dropGroup("cgroup", {"cgroup", "is_service", "service_unit", "service_scope"});
            dropGroup("sudo", {"sudo_root", "sudo_user", "sudo_uid", "sudo_gid", "sudo_command"});
            dropGroup("suspension", {"is_suspended", "is_partially_suspended", "stopped_threads"});
            dropGroup("elf32", {"is_32bit"});
            merged.update(process);
            process = std::move(merged);
        }
    }
    double cpus = std::max(1.0, data.value("cpus", 1.0));
    if (elapsed > 0)
    {
        v.lastGraphTick = GetTickCount64();
        graph.cpu = total / cpus;
        graph.cpus = static_cast<unsigned>(cpus);
        graph.memoryTotal = data.value("memory_total", 0ull);
        graph.memoryAvailable = std::min(graph.memoryTotal, data.value("memory_available", uint64_t{0}));
        graph.processCount = data["processes"].size();
        v.graphSamples.push_back(std::move(graph));
        ++v.graphSequence;
        if (v.graphSamples.size() > 120)
            v.graphSamples.pop_front();
    }
    auto count = data["processes"].size();
    v.statistics = std::to_wstring(count) + L" processes  ·  CPU " + number(total / cpus) + L"% / " +
                   number(cpus, 0) + L" vCPUs · VM available " + bytes(data.value("memory_available", 0ull)) +
                   L" / " + bytes(data.value("memory_total", 0ull));

    PostMessageW(v.graph, GraphSampleChanged, 0, 0);
    PostMessageW(v.memoryGraph, GraphSampleChanged, 0, 0);
}
} // namespace wsl::ui
