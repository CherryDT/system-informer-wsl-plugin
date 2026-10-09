#include "thread_tools.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/ioprio.h>
#include <map>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace observer {
namespace {
struct File {
    int fd;
    explicit File(int descriptor) : fd(descriptor) {}
    ~File() { if (fd >= 0) close(fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
std::runtime_error error(const std::string& operation) {
    return std::runtime_error(operation + ": " + std::strerror(errno));
}
std::string read_at(int directory, const char* name, size_t limit = 65536) {
    File file(openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.fd < 0) throw error(std::string("Cannot read ") + name);
    std::string value;
    char buffer[4096];
    for (;;) {
        const auto size = read(file.fd, buffer, sizeof(buffer));
        if (size < 0) { if (errno == EINTR) continue; throw error(std::string("Cannot read ") + name); }
        if (!size) break;
        if (static_cast<size_t>(size) > limit - value.size()) throw std::runtime_error(std::string(name) + " exceeds the read limit");
        value.append(buffer, static_cast<size_t>(size));
    }
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return value;
}
std::string optional_read(int directory, const char* name) {
    try { auto value = read_at(directory, name); return value.empty() ? "(empty)" : value; }
    catch (const std::exception& e) { return e.what(); }
}
uint64_t number(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Invalid numeric thread field");
    size_t used = 0;
    const auto value = std::stoull(text, &used);
    if (used != text.size()) throw std::runtime_error("Invalid numeric thread field");
    return value;
}
std::vector<std::string> stat_fields(const std::string& text) {
    const auto end = text.rfind(')');
    if (text.find('(') == std::string::npos || end == std::string::npos || end + 2 >= text.size())
        throw std::runtime_error("Thread exited or its stat entry is inaccessible");
    std::istringstream input(text.substr(end + 2));
    std::vector<std::string> fields;
    std::string value;
    while (input >> value) fields.push_back(value);
    if (fields.size() < 39) throw std::runtime_error("Incomplete thread stat entry");
    return fields;
}
uint64_t requested_number(const Json& value, const char* label) {
    if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
        (value.is_number_integer() && value.get<int64_t>() < 0))
        throw std::runtime_error(std::string("Invalid ") + label);
    return value.get<uint64_t>();
}
std::string input(const Json& request, const char* key) {
    const auto& values = request.at("inputs");
    if (!values.is_object() || values.size() > 8 || !values.contains(key) || !values.at(key).is_string())
        throw std::runtime_error(std::string("Missing input: ") + key);
    const auto value = values.at(key).get<std::string>();
    const size_t limit = std::string(key) == "cpus" ? 65536 : 4096;
    if (value.empty() || value.size() > limit || value.find('\0') != std::string::npos)
        throw std::runtime_error(std::string("Invalid input: ") + key);
    return value;
}
int signed_input(const Json& request, const char* key, int minimum, int maximum) {
    const auto value = input(request, key);
    size_t first = value.front() == '-' ? 1 : 0;
    if (first == value.size() || value.find_first_not_of("0123456789", first) != std::string::npos)
        throw std::runtime_error(std::string("Invalid number: ") + key);
    const auto result = std::stoll(value);
    if (result < minimum || result > maximum) throw std::runtime_error(std::string("Value out of range: ") + key);
    return static_cast<int>(result);
}
std::string policy_name(int policy) {
    switch (policy & ~SCHED_RESET_ON_FORK) {
    case SCHED_OTHER: return "Normal (SCHED_OTHER)";
    case SCHED_BATCH: return "Batch (SCHED_BATCH)";
    case SCHED_IDLE: return "Idle (SCHED_IDLE)";
    case SCHED_FIFO: return "Real-time FIFO (SCHED_FIFO)";
    case SCHED_RR: return "Real-time round-robin (SCHED_RR)";
#ifdef SCHED_DEADLINE
    case SCHED_DEADLINE: return "Deadline (SCHED_DEADLINE)";
#endif
    default: return "Unknown (" + std::to_string(policy) + ")";
    }
}
// A generous fixed ceiling keeps malformed ranges cheap and covers large hosts.
constexpr int MaxCpus = 8192;
using CpuMask = std::vector<unsigned long>;
CpuMask empty_mask() { return CpuMask(MaxCpus / (8 * sizeof(unsigned long))); }
void add_cpu(CpuMask& mask, int cpu) { mask[cpu / (8 * sizeof(unsigned long))] |= 1UL << (cpu % (8 * sizeof(unsigned long))); }
bool has_cpu(const CpuMask& mask, int cpu) { return (mask[cpu / (8 * sizeof(unsigned long))] & (1UL << (cpu % (8 * sizeof(unsigned long))))) != 0; }
CpuMask parse_cpus(const std::string& text) {
    auto mask = empty_mask();
    if (text.empty() || text.size() > 65536 || text.back() == ',') throw std::runtime_error("Enter CPUs such as 0-3,5");
    std::istringstream parts(text);
    std::string part;
    while (std::getline(parts, part, ',')) {
        const auto dash = part.find('-');
        const auto first = number(part.substr(0, dash));
        const auto last = dash == std::string::npos ? first : number(part.substr(dash + 1));
        if (first > last || last >= MaxCpus) throw std::runtime_error("CPU range is invalid or exceeds 8191");
        for (auto cpu = first; cpu <= last; ++cpu) add_cpu(mask, static_cast<int>(cpu));
    }
    return mask;
}
std::string cpu_list(const CpuMask& mask) {
    std::string result;
    for (int cpu = 0; cpu < MaxCpus; ++cpu) {
        if (!has_cpu(mask, cpu)) continue;
        const int begin = cpu;
        while (cpu + 1 < MaxCpus && has_cpu(mask, cpu + 1)) ++cpu;
        if (!result.empty()) result += ',';
        result += std::to_string(begin);
        if (cpu != begin) result += '-' + std::to_string(cpu);
    }
    return result;
}
CpuMask affinity(int tid) {
    auto mask = empty_mask();
    if (sched_getaffinity(tid, mask.size() * sizeof(unsigned long), reinterpret_cast<cpu_set_t*>(mask.data())) != 0)
        throw error("Cannot read CPU affinity");
    return mask;
}
CpuMask online_cpus() {
    auto value = read_text("/sys/devices/system/cpu/online", 65536);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return parse_cpus(value);
}
std::string io_priority(int tid) {
#ifdef SYS_ioprio_get
    const int value = static_cast<int>(syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, tid));
    if (value < 0) return std::string("Cannot read I/O priority: ") + std::strerror(errno);
    switch (IOPRIO_PRIO_CLASS(value)) {
    case IOPRIO_CLASS_NONE: return "Default (derived from scheduling priority)";
    case IOPRIO_CLASS_IDLE: return "Idle";
    case IOPRIO_CLASS_BE: return "Best effort, level " + std::to_string(IOPRIO_PRIO_DATA(value));
    case IOPRIO_CLASS_RT: return "Real time, level " + std::to_string(IOPRIO_PRIO_DATA(value));
    default: return std::to_string(value);
    }
#else
    return "I/O priority is not supported by this helper build";
#endif
}
} // namespace

Json thread_snapshot(int pid, int tid, const std::string& text) {
    const auto fields = stat_fields(text);
    const auto begin = text.find('('), end = text.rfind(')');
    return {{"pid", pid}, {"tid", tid}, {"name", text.substr(begin + 1, end - begin - 1)},
        {"state", fields[0]}, {"start_ticks", number(fields[19])},
        {"nice", std::stoi(fields[16])}, {"priority", std::stoi(fields[15])},
        {"processor", std::stoi(fields[36])}, {"policy", std::stoi(fields[38])},
        {"user_ticks", number(fields[11])}, {"kernel_ticks", number(fields[12])}};
}

static void require_target(const ProcessIdentity& identity, const Json& request) {
    require_identity(identity);
    if (!request.contains("expected_exe")) return;
    const auto& supplied = request.at("expected_exe");
    if (!supplied.is_string()) throw std::runtime_error("Invalid expected executable path");
    const auto expected = supplied.get<std::string>();
    if (expected.empty() || expected.size() > 4096 || expected.front() != '/' || expected.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid expected executable path");
    if (read_link("/proc/" + std::to_string(identity.pid) + "/exe") != expected)
        throw std::runtime_error("The process executable changed or is inaccessible; the scheduling operation was not continued");
}

static Json thread_tool_one(const Json& request, bool diagnostics, bool& mutation_applied) {
    const auto identity = request_identity(request);
    const auto& row = request.at("row");
    const auto requested_tid = requested_number(row.at("tid"), "thread ID");
    const auto start = requested_number(row.at("start_ticks"), "thread start time");
    if (!requested_tid || requested_tid > std::numeric_limits<int>::max()) throw std::runtime_error("Invalid thread ID");
    const int tid = static_cast<int>(requested_tid);
    const std::string action = request.at("action").get<std::string>();
    File process(open(("/proc/" + std::to_string(identity.pid)).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (process.fd < 0) throw error("Cannot open process");
    File task(openat(process.fd, ("task/" + std::to_string(tid)).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (task.fd < 0) throw error("Cannot open thread");
    auto validate = [&]() {
        if (number(stat_fields(read_at(process.fd, "stat", 16384))[19]) != identity.start_ticks ||
            number(stat_fields(read_at(task.fd, "stat", 16384))[19]) != start)
            throw std::runtime_error("Process or thread identity changed; refresh before trying again");
        // Scheduling syscalls accept numeric TIDs, not proc fds or pidfds. Check
        // the currently named task as well as our pinned directory immediately
        // around mutations. Linux offers no atomic identity-safe variant: the
        // tiny exit/reuse race is the same constraint as taskset and renice.
        require_target(identity, request);
        const auto current = read_text("/proc/" + std::to_string(identity.pid) + "/task/" + std::to_string(tid) + "/stat", 16384);
        if (number(stat_fields(current)[19]) != start) throw std::runtime_error("Thread identity changed; refresh before trying again");
    };
    validate();
    std::string operation;
    std::string effective_affinity;
    bool affinity_restricted = false;
    if (action == "set_nice") {
        const int nice = signed_input(request, "nice", -20, 19);
        validate();
        if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) != 0) throw error("Cannot change thread niceness");
        mutation_applied = true;
        operation = "Thread niceness changed.";
    } else if (action == "set_affinity") {
        const auto wanted = parse_cpus(input(request, "cpus"));
        const auto allowed = online_cpus();
        for (int cpu = 0; cpu < MaxCpus; ++cpu)
            if (has_cpu(wanted, cpu) && !has_cpu(allowed, cpu)) throw std::runtime_error("CPU " + std::to_string(cpu) + " is not online");
        validate();
        if (sched_setaffinity(tid, wanted.size() * sizeof(unsigned long), reinterpret_cast<const cpu_set_t*>(wanted.data())) != 0)
            throw error("Cannot change thread CPU affinity");
        mutation_applied = true;
        validate();
        const auto effective = affinity(tid);
        effective_affinity = cpu_list(effective);
        affinity_restricted = effective != wanted;
        operation = "Thread CPU affinity changed. Effective CPU affinity: " + effective_affinity + ".";
        if (affinity_restricted)
            operation += " Linux restricted the requested mask, for example because of a cgroup cpuset restriction.";
    } else if (action == "set_policy") {
        const auto value = input(request, "policy");
        const int policy = value == "other" ? SCHED_OTHER : value == "batch" ? SCHED_BATCH : value == "idle" ? SCHED_IDLE : -1;
        if (policy < 0) throw std::runtime_error("Scheduling policy must be other, batch, or idle");
        validate();
        const int old = sched_getscheduler(tid);
        if (old < 0) throw error("Cannot read scheduling policy");
        const int basic = old & ~SCHED_RESET_ON_FORK;
        if (basic != SCHED_OTHER && basic != SCHED_BATCH && basic != SCHED_IDLE)
            throw std::runtime_error("This thread uses a real-time or deadline policy; changing it is not supported here");
        sched_param parameters{};
        validate();
        if (sched_setscheduler(tid, policy | (old & SCHED_RESET_ON_FORK), &parameters) != 0) throw error("Cannot change scheduling policy");
        mutation_applied = true;
        operation = "Thread scheduling policy changed.";
    } else if (action == "set_io_priority") {
#ifdef SYS_ioprio_set
        const auto value = input(request, "class");
        const int io_class = value == "none" ? IOPRIO_CLASS_NONE : value == "best-effort" ? IOPRIO_CLASS_BE : value == "idle" ? IOPRIO_CLASS_IDLE : -1;
        if (io_class < 0) throw std::runtime_error("I/O priority class must be none, best-effort, or idle");
        const int level = io_class == IOPRIO_CLASS_BE ? signed_input(request, "level", 0, 7) : 0;
        validate();
        if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, tid, (io_class << IOPRIO_CLASS_SHIFT) | level) != 0) throw error("Cannot change I/O priority");
        mutation_applied = true;
        operation = "Thread I/O priority changed. Its effect depends on the block-device scheduler.";
#else
        throw std::runtime_error("I/O priority is not supported by this helper build");
#endif
    } else if (action != "properties" && action != "kernel_stack") {
        throw std::runtime_error("Unsupported thread action");
    }
    validate();
    Json result{{"title", "Thread " + std::to_string(tid)}, {"fields", Json::array()}, {"text", operation},
        {"mutation_applied", mutation_applied}, {"complete", true}};
    if (!effective_affinity.empty()) {
        result["effective_affinity"] = effective_affinity;
        result["affinity_restricted"] = affinity_restricted;
    }
    if (!diagnostics) return result;
    if (action == "kernel_stack") {
        result["text"] = optional_read(task.fd, "stack");
        validate();
        return result;
    }
    const auto snapshot = thread_snapshot(identity.pid, tid, read_at(task.fd, "stat", 16384));
    auto field = [&](const char* name, const std::string& value) { result["fields"].push_back({{"name", name}, {"value", value}}); };
    field("Thread ID", std::to_string(tid));
    field("Name", snapshot.at("name").get<std::string>());
    field("Start time (ticks)", std::to_string(start));
    field("State", snapshot.at("state").get<std::string>());
    field("Nice", std::to_string(snapshot.at("nice").get<int>()));
    field("Priority", std::to_string(snapshot.at("priority").get<int>()));
    field("Scheduling policy", policy_name(snapshot.at("policy").get<int>()));
    field("Current CPU", std::to_string(snapshot.at("processor").get<int>()));
    try { field("CPU affinity", cpu_list(affinity(tid))); } catch (const std::exception& e) { field("CPU affinity", e.what()); }
    field("I/O priority", io_priority(tid));
    const auto hz = sysconf(_SC_CLK_TCK);
    auto time_text = [&](const char* name) {
        const auto ticks = snapshot.at(name).get<uint64_t>();
        std::ostringstream out;
        out.setf(std::ios::fixed); out.precision(3);
        out << (hz > 0 ? static_cast<double>(ticks) / hz : 0) << " s (" << ticks << " ticks)";
        return out.str();
    };
    field("User time", time_text("user_ticks"));
    field("Kernel time", time_text("kernel_ticks"));
    std::map<std::string, std::string> status;
    std::istringstream lines(read_at(task.fd, "status"));
    std::string line;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto first = line.find_first_not_of(" \t", colon + 1);
        status[line.substr(0, colon)] = first == std::string::npos ? "" : line.substr(first);
    }
    for (const auto& item : {std::pair<const char*, const char*>{"voluntary_ctxt_switches", "Voluntary context switches"},
        {"nonvoluntary_ctxt_switches", "Involuntary context switches"}, {"VmStk", "Process stack accounting (VmStk)"},
        {"TracerPid", "Debugger PID"}}) {
        const auto found = status.find(item.first);
        if (found != status.end()) field(item.second, found->second);
    }
    field("Wait channel", optional_read(task.fd, "wchan"));
    field("Current syscall", optional_read(task.fd, "syscall"));
    field("Kernel stack", optional_read(task.fd, "stack"));
    validate();
    return result;
}
namespace {
void validate_control_inputs(const Json& request) {
    const auto action = request.at("action").get<std::string>();
    if (action == "set_nice") signed_input(request, "nice", -20, 19);
    else if (action == "set_affinity") parse_cpus(input(request, "cpus"));
    else if (action == "set_policy") {
        const auto policy = input(request, "policy");
        if (policy != "other" && policy != "batch" && policy != "idle")
            throw std::runtime_error("Scheduling policy must be other, batch, or idle");
    } else if (action == "set_io_priority") {
        const auto io_class = input(request, "class");
        if (io_class != "none" && io_class != "best-effort" && io_class != "idle")
            throw std::runtime_error("I/O priority class must be none, best-effort, or idle");
        if (io_class == "best-effort") signed_input(request, "level", 0, 7);
    } else throw std::runtime_error("This operation cannot be applied to all process threads");
}

Json process_thread_tool(const Json& request) {
    const auto identity = request_identity(request);
    validate_control_inputs(request);
    require_target(identity, request);
    constexpr size_t ThreadLimit = 8192;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    struct TaskDirectory {
        DIR* value;
        ~TaskDirectory() { if (value) closedir(value); }
    } tasks{opendir(("/proc/" + std::to_string(identity.pid) + "/task").c_str())};
    if (!tasks.value) throw error("Cannot enumerate process threads");

    std::vector<std::pair<int, uint64_t>> targets;
    size_t considered = 0, updated = 0, failed = 0, applied = 0, restricted = 0;
    bool limited = false, report_truncated = false;
    std::string report, stopped;
    auto note = [&](int tid, const std::string& text) {
        const auto line = "Thread " + std::to_string(tid) + ": " + text + "\n";
        if (report.size() + line.size() <= 512 * 1024) report += line;
        else report_truncated = true;
    };
    // Snapshot once. Threads created afterward are outside this request; their
    // scheduler state follows Linux's normal inheritance rules.
    for (;;) {
        errno = 0;
        const auto* entry = readdir(tasks.value);
        if (!entry) {
            if (errno) stopped = std::string("Thread enumeration failed: ") + std::strerror(errno);
            break;
        }
        const std::string name(entry->d_name);
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
        if (considered >= ThreadLimit || std::chrono::steady_clock::now() >= deadline) {
            limited = true;
            stopped = considered >= ThreadLimit ? "Stopped after enumerating 8192 threads." : "The five-second operation limit was reached during enumeration.";
            break;
        }
        ++considered;
        int tid = 0;
        try {
            const auto numeric = number(name);
            if (!numeric || numeric > std::numeric_limits<int>::max()) throw std::runtime_error("Invalid task directory name");
            tid = static_cast<int>(numeric);
            File task(openat(dirfd(tasks.value), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (task.fd < 0) throw error("Cannot open thread");
            targets.emplace_back(tid, number(stat_fields(read_at(task.fd, "stat", 16384))[19]));
        } catch (const std::exception& e) { ++failed; note(tid, e.what()); }
    }
    // Reject a replaced/exited process before any of the snapshotted IDs can be
    // changed. Every setter independently revalidates both identities as well.
    bool process_available = true;
    try { require_target(identity, request); }
    catch (const std::exception& e) {
        process_available = false;
        stopped = std::string("Process no longer matches this request: ") + e.what();
    }

    size_t attempted = 0;
    if (process_available && (stopped.empty() || limited)) {
        Json one = request;
        one.erase("all_threads");
        for (const auto& target : targets) {
            if (std::chrono::steady_clock::now() >= deadline) {
                limited = true;
                stopped = "The five-second operation limit was reached; remaining threads were not changed.";
                break;
            }
            one["row"] = {{"tid", target.first}, {"start_ticks", target.second}};
            bool changed = false;
            ++attempted;
            try {
                const auto result = thread_tool_one(one, false, changed);
                ++updated;
                if (result.value("affinity_restricted", false)) {
                    ++restricted;
                    note(target.first, "Linux restricted CPU affinity to " + result.at("effective_affinity").get<std::string>() + ".");
                }
            } catch (const std::exception& e) {
                ++failed;
                note(target.first, (changed ? std::string("Change applied, but its final state could not be verified: ") : std::string{}) + e.what());
            }
            if (changed) ++applied;
            // A vanished process makes every remaining target invalid. Stop
            // instead of spending the rest of the budget repeating that error.
            try { require_target(identity, request); }
            catch (const std::exception& e) { stopped = std::string("Process no longer matches this request: ") + e.what(); break; }
        }
    }
    try { require_target(identity, request); }
    catch (const std::exception& e) { stopped = std::string("Process no longer matches this request: ") + e.what(); }
    const size_t skipped = targets.size() - attempted;
    const bool complete = failed == 0 && skipped == 0 && !limited && stopped.empty();
    std::string summary = std::to_string(updated) + " thread(s) updated, " + std::to_string(failed) +
        " failed, " + std::to_string(skipped) + " not attempted.\n";
    if (updated == 0) summary += applied ? "Some changes were applied, but none could be fully verified.\n" : "No thread changes were applied.\n";
    summary += "This request applies separately to the threads captured by its initial enumeration; it is not atomic. "
        "Threads created afterward follow normal Linux inheritance.\n";
    if (!stopped.empty()) summary += stopped + "\n";
    if (!report.empty()) summary += "\n" + report;
    if (report_truncated) summary += "\nAdditional diagnostics omitted at the output limit.\n";
    Json result{{"title", "Process scheduling: " + std::to_string(identity.pid)}, {"fields", Json::array()},
        {"text", summary}, {"mutation_applied", applied != 0}, {"complete", complete},
        {"truncated", limited || report_truncated}, {"updated", updated}, {"failed", failed}, {"not_attempted", skipped}};
    auto field = [&](const char* name, size_t value) { result["fields"].push_back({{"name", name}, {"value", std::to_string(value)}}); };
    field("Threads considered", considered);
    field("Updated", updated);
    field("Failed", failed);
    field("Not attempted", skipped);
    field("Changes applied", applied);
    if (restricted) field("Affinity restricted by Linux", restricted);
    return result;
}
} // namespace

// Affinity controls need the actual online CPU IDs, not an assumed 0..N-1
// range. This query runs only when opening the editor and never changes a task.
static Json affinity_settings(const Json& request) {
    const auto identity = request_identity(request);
    require_target(identity, request);
    const auto online = online_cpus();
    auto common = empty_mask(), any = empty_mask();
    size_t observed = 0;
    auto collect = [&](int tid, std::optional<uint64_t> expected) {
        File task(open(("/proc/" + std::to_string(identity.pid) + "/task/" + std::to_string(tid)).c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (task.fd < 0) throw error("Cannot open thread");
        const auto start = number(stat_fields(read_at(task.fd, "stat", 16384))[19]);
        if (expected && start != *expected) throw std::runtime_error("Thread identity changed; reopen the affinity editor");
        const auto mask = affinity(tid);
        if (number(stat_fields(read_at(task.fd, "stat", 16384))[19]) != start)
            throw std::runtime_error("Thread identity changed while reading CPU affinity");
        if (!observed) common = mask;
        for (size_t i = 0; i < mask.size(); ++i) { common[i] &= mask[i]; any[i] |= mask[i]; }
        ++observed;
    };
    if (request.value("all_threads", false)) {
        struct Tasks { DIR* value; ~Tasks() { if (value) closedir(value); } } tasks{
            opendir(("/proc/" + std::to_string(identity.pid) + "/task").c_str())};
        if (!tasks.value) throw error("Cannot enumerate process threads");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (const auto* entry = readdir(tasks.value)) {
            const std::string name(entry->d_name);
            if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
            if (observed >= 8192 || std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Reading process CPU affinity exceeded the collection limit");
            const auto tid = number(name);
            if (tid > static_cast<uint64_t>(std::numeric_limits<int>::max())) continue;
            // Threads that exit during enumeration do not make the remaining
            // live masks ambiguous. Other failures are reported by collect.
            if (access(("/proc/" + std::to_string(identity.pid) + "/task/" + name).c_str(), F_OK) != 0) continue;
            try { collect(static_cast<int>(tid), std::nullopt); }
            catch (const std::exception&) {
                if (access(("/proc/" + std::to_string(identity.pid) + "/task/" + name).c_str(), F_OK) == 0) throw;
            }
        }
    } else {
        const auto& row = request.at("row");
        const auto tid = requested_number(row.at("tid"), "thread ID");
        if (!tid || tid > static_cast<uint64_t>(std::numeric_limits<int>::max())) throw std::runtime_error("Invalid thread ID");
        collect(static_cast<int>(tid), requested_number(row.at("start_ticks"), "thread start time"));
    }
    if (!observed) throw std::runtime_error("No live threads were available to inspect");
    require_target(identity, request);
    Json result{{"online_cpus", Json::array()}, {"affinity_cpus", Json::array()}, {"mixed_cpus", Json::array()}};
    for (int cpu = 0; cpu < MaxCpus; ++cpu) {
        if (!has_cpu(online, cpu)) continue;
        result["online_cpus"].push_back(cpu);
        if (has_cpu(common, cpu)) result["affinity_cpus"].push_back(cpu);
        else if (has_cpu(any, cpu)) result["mixed_cpus"].push_back(cpu);
    }
    return result;
}

Json thread_tool(const Json& request) {
    if (request.value("action", "") == "settings") return affinity_settings(request);
    if (request.value("all_threads", false)) return process_thread_tool(request);
    bool changed = false;
    try { return thread_tool_one(request, true, changed); }
    catch (const std::exception& e) {
        if (!changed) throw;
        // A successful syscall followed by a disappearing target is not a
        // failed mutation. Preserve that distinction for UI refresh/results.
        return {{"title", "Thread scheduling result"}, {"fields", Json::array()},
            {"mutation_applied", true}, {"complete", false},
            {"text", std::string("The change was applied, but the final state could not be verified: ") + e.what()}};
    }
}
} // namespace observer
