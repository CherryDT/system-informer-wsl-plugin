#include "capture_model.hpp"
#include <set>
#include <utility>

namespace wsl
{
namespace
{
std::string processKey(const Json &process)
{
    return process.at("pid").dump() + ":" + process.at("start_ticks").dump();
}
} // namespace

void updateCaptureSnapshot(CaptureModel &v, const Json &data, bool accumulateCpu, size_t historyCapacity)
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
        // Process identities are meaningful only within this guest boot.
        // Do not merge cached metadata after a restart or clock reset.
        v.snapshot = Json();
        v.bootId = boot;
    }
    // Store raw Linux CPU percentages (100% = one vCPU). The display setting
    // only scales rendered rows; history always measures total guest capacity.
    double elapsed = v.previousTime ? (now - v.previousTime) / 1000.0 : 0;
    double hz = data.value("clock_ticks", 100.0), total = 0;
    GraphSample graph;
    GetSystemTimeAsFileTime(&graph.timestamp);
    graph.interval = elapsed;
    historyCapacity = std::max<size_t>(1, historyCapacity);
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
                if (accumulateCpu)
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
}

void resetCaptureBaseline(CaptureModel &model)
{
    model.previous.clear();
    model.previousTime = 0;
    model.newProcess.clear();
}

void suspendCaptureModel(CaptureModel &model, ULONGLONG now)
{
    resetCaptureBaseline(model);
    model.lastGraphTick = now;
    // One empty slot separates adjacent traces even for a brief suspension.
    GraphSample missing;
    missing.missing = true;
    GetSystemTimeAsFileTime(&missing.timestamp);
    model.graphSamples.push_back(std::move(missing));
    ++model.graphSequence;
    if (model.graphSamples.size() > 120)
        model.graphSamples.pop_front();
}

void advanceCaptureGap(CaptureModel &model, ULONGLONG now, DWORD interval)
{
    if (!model.lastGraphTick || now < model.lastGraphTick)
        return;
    interval = std::max<DWORD>(1, interval);
    const auto count = (now - model.lastGraphTick) / interval;
    if (!count)
        return;
    FILETIME stamp{};
    GetSystemTimeAsFileTime(&stamp);
    ULARGE_INTEGER time{};
    time.LowPart = stamp.dwLowDateTime;
    time.HighPart = stamp.dwHighDateTime;
    // Old slots outside the visible history need no allocation, but still
    // advance the sequence so graph scrolling accounts for the whole gap.
    for (uint64_t i = std::min<uint64_t>(count, 120); i > 0; --i)
    {
        GraphSample missing;
        missing.missing = true;
        ULARGE_INTEGER slot = time;
        slot.QuadPart -= (i - 1) * interval * 10000ull;
        missing.timestamp = {slot.LowPart, slot.HighPart};
        model.graphSamples.push_back(std::move(missing));
        if (model.graphSamples.size() > 120)
            model.graphSamples.pop_front();
    }
    model.graphSequence += count;
    model.lastGraphTick += count * interval;
}
} // namespace wsl
