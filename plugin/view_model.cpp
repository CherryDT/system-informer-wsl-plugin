#include "controller.hpp"
#include "settings.hpp"
#include "view_state.hpp"
#include "graphs.hpp"
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
    v.readRate.clear();
    v.writeRate.clear();
    v.previousTime = 0;
    v.bootId.clear();
    v.graphSamples.clear();
    v.graphSequence = 0;
    v.snapshot = Json();
    v.sockets = Json();
    v.units = Json();
    v.processes.clear();
    v.connections.clear();
    v.services.clear();
    v.statistics.clear();
    status(v, L"Connecting to the selected distribution…");
    PostMessageW(v.graph, GraphSampleChanged, 0, 0);
    PostMessageW(v.memoryGraph, GraphSampleChanged, 0, 0);
}
void updateButtons(View &v)
{
    EnableWindow(v.settings, TRUE);
    EnableWindow(v.exportButton, !v.table().rows.empty());
}
void render(View &v)
{
    v.cpuPercentOfTotal = readSetting(L"CpuPercentOfTotal", 1) != 0;
    const double cpuDivisor =
        v.cpuPercentOfTotal && v.snapshot.is_object() ? std::max(1.0, v.snapshot.value("cpus", 1.0)) : 1.0;
    std::vector<Row> rows;
    const auto query = lower(windowText(v.search));
    if (v.page == 0 && v.snapshot.contains("processes"))
    {
        auto items = v.snapshot["processes"].get<std::vector<Json>>();
        std::map<int, int> depths;
        const bool tree = SendMessageW(v.tree, BM_GETCHECK, 0, 0) == BST_CHECKED;
        v.processes.setAncestryOrder(tree);
        // Preserve ancestry order before filtering; a missing or exiting parent
        // must not hide its surviving children. Table sorting is disabled here.
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
        }
        for (const auto &p : items)
        {
            auto key = processKey(p);
            std::wstring name = std::wstring(depths[p.value("pid", 0)] * 2, L' ') + text(p, "name");
            Row row{{name, text(p, "pid"), text(p, "user"), number(v.cpu[key] / cpuDivisor),
                     number(p.value("rss_bytes", 0ull) / 1048576.0), number(v.readRate[key] / 1024.0),
                     number(v.writeRate[key] / 1024.0), text(p, "state"), text(p, "threads"), text(p, "ppid"),
                     text(p, "command")},
                    p,
                    key};
            rows.push_back(std::move(row));
        }
        v.processes.replace(
            std::move(rows), [query](const Row &row) { return matches(row, query); },
            !v.snapshot.value("processes_truncated", false));
    }
    else if (v.page == 1 && v.sockets.contains("connections"))
    {
        bool onlyListeners = SendMessageW(v.listeners, BM_GETCHECK, 0, 0) == BST_CHECKED;
        for (const auto &c : v.sockets["connections"])
        {
            const auto protocol = text(c, "protocol"), state = text(c, "state");
            Row row{{protocol, text(c, "local_address"), text(c, "local_port"), text(c, "remote_address"),
                     text(c, "remote_port"), state, text(c, "pid"), text(c, "process"), text(c, "inode")},
                    c,
                    Json::array({c.value("protocol", ""), c.value("inode", 0ull), c.value("pid", 0),
                                 c.value("start_ticks", 0ull), c.value("local_address", ""),
                                 c.value("local_port", 0), c.value("remote_address", ""),
                                 c.value("remote_port", 0)})
                        .dump()};
            rows.push_back(std::move(row));
        }
        v.connections.replace(
            std::move(rows),
            [query, onlyListeners](const Row &row) {
                const auto protocol = text(row.data, "protocol"), state = text(row.data, "state");
                const bool listening =
                    state == L"LISTEN" || state == L"LISTENING" ||
                    (protocol.find(L"udp") != std::wstring::npos && row.data.value("remote_port", 0) == 0);
                return (!onlyListeners || listening) && matches(row, query);
            },
            !v.sockets.value("connections_truncated", false) &&
                v.sockets.value("inaccessible_processes", 0) == 0);
    }
    else if (v.page == 2 && v.units.contains("services"))
    {
        for (const auto &s : v.units["services"])
        {
            Row row{{text(s, "name"), text(s, "active"), text(s, "sub"), text(s, "enabled"), text(s, "load"),
                     text(s, "description")},
                    s,
                    s.value("name", "")};
            rows.push_back(std::move(row));
        }
        v.services.replace(
            std::move(rows), [query](const Row &row) { return matches(row, query); },
            v.units.value("available", true));
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
    v.previous = std::move(samples);
    v.previousTime = now;
    v.snapshot = data;
    double cpus = std::max(1.0, data.value("cpus", 1.0));
    if (elapsed > 0)
    {
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
