#pragma once

#include "common.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <optional>

namespace wsl
{
struct ProcessSample
{
    uint64_t ticks = 0, read = 0, written = 0;
    bool hasIo = false;
};
// Raw per-vCPU percentages, independent of the current display scale. Keeping
// the sum makes each update constant-time once the history window is full.
struct CpuHistory
{
    std::deque<double> samples;
    double sum = 0;

    void append(double usage, size_t capacity)
    {
        samples.push_back(usage);
        sum += usage;
        while (samples.size() > capacity)
        {
            sum -= samples.front();
            samples.pop_front();
        }
    }
    double average() const
    {
        return samples.empty() ? 0 : std::max(0.0, sum / samples.size());
    }
};
struct GraphSample
{
    FILETIME timestamp{};
    bool missing = false;
    double cpu = 0, topCpu = 0, interval = 0;
    unsigned cpus = 1;
    uint64_t memoryTotal = 0, memoryAvailable = 0, largestRss = 0;
    size_t processCount = 0;
    int topPid = 0, largestRssPid = 0;
    std::wstring topName, largestRssName;
};
// One distribution's capture state, independent of any view lifetime. All
// access is serialized on the host UI thread by the capture coordinator.
struct CaptureModel
{
    Json snapshot;
    std::map<std::string, ProcessSample> previous;
    std::map<std::string, double> cpu, readRate, writeRate;
    std::map<std::string, CpuHistory> cpuHistory;
    std::deque<GraphSample> graphSamples;
    uint64_t graphSequence = 0;
    uint64_t previousTime = 0;
    std::string bootId;
    std::optional<uint32_t> defaultUid;
    std::wstring statistics;
    std::string newProcess;
    ULONGLONG lastGraphTick = 0;
    bool running = true, pending = false, failed = false, componentMissing = false, suspended = false;
    std::wstring error;
    uint64_t revision = 0;
};

// Status flags and revision belong to the coordinator; these helpers only
// maintain sample data, interval baselines and history.
void updateCaptureSnapshot(CaptureModel &model, const Json &data, bool accumulateCpu, size_t historyCapacity);
void resetCaptureBaseline(CaptureModel &model);
void suspendCaptureModel(CaptureModel &model, ULONGLONG now);
void advanceCaptureGap(CaptureModel &model, ULONGLONG now, DWORD interval);
} // namespace wsl
