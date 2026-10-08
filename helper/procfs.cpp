#include "observer.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <sys/syscall.h>
#include <unistd.h>

namespace observer {
namespace {
std::string proc_path(int pid, const char* leaf) {
    return "/proc/" + std::to_string(pid) + "/" + leaf;
}
uint64_t unsigned_value(const std::string& value) {
    size_t used = 0;
    const auto result = std::stoull(value, &used);
    if (used != value.size()) throw std::runtime_error("Invalid numeric field in /proc");
    return result;
}
std::string user_name(uid_t uid) {
    // Avoid retaining pointers into libc's shared passwd buffer.
    std::vector<char> buffer(16384);
    passwd entry{}, *result = nullptr;
    if (getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &result) == 0 && result)
        return entry.pw_name;
    return std::to_string(uid);
}
uint64_t field_value(const std::string& text, const std::string& key) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.compare(0, key.size(), key) == 0) {
            std::istringstream value(line.substr(key.size()));
            uint64_t number = 0;
            value >> number;
            return number;
        }
    }
    return 0;
}
Json process_json(const ProcessStat& stat) {
    const auto status = read_text(proc_path(stat.pid, "status"));
    const auto uid = status.empty() ? std::numeric_limits<uid_t>::max() :
        static_cast<uid_t>(field_value(status, "Uid:"));
    auto command = read_text(proc_path(stat.pid, "cmdline"), 256 * 1024);
    std::replace(command.begin(), command.end(), '\0', ' ');
    if (!command.empty() && command.back() == ' ') command.pop_back();
    const auto io = read_text(proc_path(stat.pid, "io"));
    return {{"pid", stat.pid}, {"ppid", stat.ppid}, {"start_ticks", stat.start_ticks},
            {"name", stat.name}, {"state", stat.state}, {"user", status.empty() ? "unknown" : user_name(uid)},
            {"status_accessible", !status.empty()},
            {"uid", uid}, {"threads", stat.threads}, {"cpu_ticks", stat.cpu_ticks},
            {"rss_bytes", stat.rss_bytes}, {"virtual_bytes", stat.virtual_bytes},
            {"read_bytes", field_value(io, "read_bytes:")},
            {"write_bytes", field_value(io, "write_bytes:")},
            {"io_accessible", !io.empty()}, {"command", command},
            {"exe", read_link(proc_path(stat.pid, "exe"))}};
}
}

std::string read_text(const std::string& path, size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string result;
    char buffer[8192];
    while (input && result.size() < limit) {
        input.read(buffer, std::min(sizeof(buffer), limit - result.size()));
        result.append(buffer, static_cast<size_t>(input.gcount()));
    }
    return result;
}
std::string read_link(const std::string& path) {
    std::vector<char> buffer(4096);
    for (;;) {
        const auto length = readlink(path.c_str(), buffer.data(), buffer.size());
        if (length < 0) return {};
        if (static_cast<size_t>(length) < buffer.size()) return {buffer.data(), static_cast<size_t>(length)};
        if (buffer.size() >= 1024 * 1024) return {};
        buffer.resize(buffer.size() * 2);
    }
}
std::vector<int> process_ids() {
    DIR* directory = opendir("/proc");
    if (!directory) throw std::runtime_error("Cannot read /proc");
    std::vector<int> ids;
    while (const auto* entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) {
            try { ids.push_back(std::stoi(name)); } catch (...) { }
        }
    }
    closedir(directory);
    std::sort(ids.begin(), ids.end());
    return ids;
}
ProcessStat process_stat(int pid) {
    const auto text = read_text(proc_path(pid, "stat"), 16384);
    // comm can contain spaces, newlines, and parentheses. The final ')' ends it;
    // splitting the entire line on whitespace silently corrupts later fields.
    const auto begin = text.find('('), end = text.rfind(')');
    if (begin == std::string::npos || end == std::string::npos || end <= begin)
        throw std::runtime_error("Process exited or its /proc entry is inaccessible");
    std::istringstream input(text.substr(end + 2));
    std::vector<std::string> fields;
    std::string field;
    while (input >> field) fields.push_back(field);
    if (fields.size() < 22) throw std::runtime_error("Incomplete process stat entry");
    ProcessStat result;
    result.pid = pid;
    result.name = text.substr(begin + 1, end - begin - 1);
    result.state = fields[0];
    result.ppid = std::stoi(fields[1]);
    result.cpu_ticks = unsigned_value(fields[11]) + unsigned_value(fields[12]);
    result.threads = std::stoi(fields[17]);
    result.start_ticks = unsigned_value(fields[19]);
    result.virtual_bytes = unsigned_value(fields[20]);
    const auto rss_pages = std::stoll(fields[21]);
    result.rss_bytes = static_cast<uint64_t>(std::max<int64_t>(0, rss_pages)) * sysconf(_SC_PAGESIZE);
    return result;
}
ProcessIdentity request_identity(const Json& request) {
    const auto& requested_pid = request.at("pid");
    if (!requested_pid.is_number_integer() || requested_pid.get<int64_t>() <= 0 ||
        requested_pid.get<int64_t>() > std::numeric_limits<int>::max())
        throw std::runtime_error("Invalid process ID");
    const int pid = requested_pid.get<int>();
    const auto& start = request.at("start_ticks");
    if (pid <= 0 || (!start.is_number_unsigned() && !start.is_number_integer()) ||
        (start.is_number_integer() && start.get<int64_t>() < 0))
        throw std::runtime_error("Invalid process identity");
    return {pid, start.get<uint64_t>()};
}
void require_identity(const ProcessIdentity& identity) {
    if (process_stat(identity.pid).start_ticks != identity.start_ticks)
        throw std::runtime_error("Process identity changed; refresh before trying again");
}
std::string boot_id() {
    auto value = read_text("/proc/sys/kernel/random/boot_id", 128);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return value;
}
Json hello() {
    return {{"protocol", 1}, {"boot_id", boot_id()}, {"uid", getuid()},
            {"cpus", sysconf(_SC_NPROCESSORS_ONLN)}, {"clock_ticks", sysconf(_SC_CLK_TCK)},
            {"systemd", access("/run/systemd/system", F_OK) == 0},
            {"helper_version", "0.1.0"}};
}
Json snapshot() {
    Json processes = Json::array();
    for (int pid : process_ids()) {
        try {
            const auto stat = process_stat(pid);
            auto process = process_json(stat);
            // Do not combine metadata from two occupants of a rapidly reused PID.
            if (process_stat(pid).start_ticks == stat.start_ticks) processes.push_back(std::move(process));
        } catch (const std::exception&) { /* Processes can disappear during a scan. */ }
    }
    const auto memory = read_text("/proc/meminfo");
    double uptime = 0;
    std::istringstream(read_text("/proc/uptime")) >> uptime;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return {{"processes", std::move(processes)},
            {"monotonic_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now).count()},
            {"uptime_seconds", uptime}, {"memory_total", field_value(memory, "MemTotal:") * 1024},
            {"memory_available", field_value(memory, "MemAvailable:") * 1024},
            {"cpus", sysconf(_SC_NPROCESSORS_ONLN)}, {"boot_id", boot_id()},
            {"loadavg", read_text("/proc/loadavg", 256)},
            {"pressure", "CPU\n" + read_text("/proc/pressure/cpu", 4096) + "\nMemory\n" + read_text("/proc/pressure/memory", 4096) + "\nI/O\n" + read_text("/proc/pressure/io", 4096)}};
}
Json process_details(const Json& request) {
    const auto identity = request_identity(request);
    require_identity(identity);
    Json files = Json::array(), modules = Json::array();
    const auto fd_path = proc_path(identity.pid, "fd");
    DIR* directory = opendir(fd_path.c_str());
    const bool files_accessible = directory != nullptr;
    if (directory) {
        while (const auto* entry = readdir(directory)) {
            const std::string name(entry->d_name);
            if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
            const auto info = read_text(proc_path(identity.pid, ("fdinfo/" + name).c_str()), 16384);
            std::string flags;
            std::istringstream lines(info);
            std::string line;
            while (std::getline(lines, line)) if (line.rfind("flags:", 0) == 0) {
                std::istringstream(line.substr(6)) >> flags;
            }
            files.push_back({{"fd", std::stoi(name)}, {"target", read_link(fd_path + "/" + name)}, {"flags", flags}});
        }
        closedir(directory);
    }
    std::sort(files.begin(), files.end(), [](const Json& a, const Json& b) { return a["fd"].get<int>() < b["fd"].get<int>(); });
    const auto maps = read_text(proc_path(identity.pid, "maps"), 8 * 1024 * 1024);
    std::istringstream lines(maps);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string range, permissions, offset, device, inode, path;
        if (!(fields >> range >> permissions >> offset >> device >> inode)) continue;
        std::getline(fields >> std::ws, path);
        const auto dash = range.find('-');
        if (path.empty() || dash == std::string::npos) continue;
        modules.push_back({{"path", path}, {"start", range.substr(0, dash)},
                           {"end", range.substr(dash + 1)}, {"permissions", permissions}});
    }
    std::string namespace_text;
    for (const auto* name : {"cgroup", "ipc", "mnt", "net", "pid", "pid_for_children", "time", "user", "uts"})
        namespace_text += std::string(name) + ": " + read_link(proc_path(identity.pid, (std::string("ns/") + name).c_str())) + "\n";
    Json threads = Json::array();
    const auto task_path = proc_path(identity.pid, "task");
    DIR* tasks = opendir(task_path.c_str());
    if (tasks) {
        while (const auto* entry = readdir(tasks)) {
            const std::string tid(entry->d_name);
            if (tid.empty() || tid.find_first_not_of("0123456789") != std::string::npos) continue;
            const auto text = read_text(task_path + "/" + tid + "/stat", 16384);
            const auto begin = text.find('('), end = text.rfind(')');
            if (begin == std::string::npos || end == std::string::npos || end + 2 >= text.size()) continue;
            threads.push_back({{"tid", std::stoi(tid)}, {"name", text.substr(begin + 1, end - begin - 1)},
                {"state", text.substr(end + 2, 1)}, {"wchan", read_text(task_path + "/" + tid + "/wchan", 4096)}});
        }
        closedir(tasks);
    }
    const auto summary = "Status\n" + read_text(proc_path(identity.pid, "status")) +
        "\nI/O counters\n" + read_text(proc_path(identity.pid, "io")) +
        "\nControl groups\n" + read_text(proc_path(identity.pid, "cgroup")) +
        "\nLimits\n" + read_text(proc_path(identity.pid, "limits")) +
        "\nNamespaces\n" + namespace_text +
        "\nExecutable: " + read_link(proc_path(identity.pid, "exe")) +
        "\nWorking directory: " + read_link(proc_path(identity.pid, "cwd")) + "\n";
    Json environment = Json::array();
    const auto raw_environment = read_text(proc_path(identity.pid, "environ"), 4 * 1024 * 1024);
    size_t offset = 0;
    while (offset < raw_environment.size()) {
        const auto end = raw_environment.find('\0', offset);
        const auto entry = raw_environment.substr(offset, end == std::string::npos ? end : end - offset);
        const auto equals = entry.find('=');
        environment.push_back({{"name", entry.substr(0, equals)},
            {"value", equals == std::string::npos ? "" : entry.substr(equals + 1)}});
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    require_identity(identity);
    return {{"threads", threads}, {"environment", environment}, {"summary", summary}, {"files", files}, {"modules", modules},
            {"files_accessible", files_accessible}, {"modules_accessible", !maps.empty()}};
}
Json send_signal(const Json& request) {
    const auto identity = request_identity(request);
    const auto& requested_signal = request.at("signal");
    if (!requested_signal.is_number_integer() || requested_signal.get<int64_t>() < 1 ||
        requested_signal.get<int64_t>() > 64)
        throw std::runtime_error("Invalid signal number");
    const int signal = requested_signal.get<int>();
    if (identity.pid <= 1 || identity.pid == getpid()) throw std::runtime_error("This process is protected");
    if (signal != SIGTERM && signal != SIGKILL && signal != SIGSTOP && signal != SIGCONT && signal != SIGUSR1 && signal != SIGUSR2)
        throw std::runtime_error("Unsupported signal; use TERM, KILL, STOP, CONT, USR1, or USR2");
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const int fd = static_cast<int>(syscall(SYS_pidfd_open, identity.pid, 0));
    if (fd < 0) throw std::runtime_error(std::string("Cannot open process identity: ") + std::strerror(errno));
    try {
        // Open first, validate second. A retained pidfd cannot retarget a new PID
        // occupant if the original process exits between validation and signaling.
        require_identity(identity);
        if (syscall(SYS_pidfd_send_signal, fd, signal, nullptr, 0) != 0)
            throw std::runtime_error(std::string("Cannot signal process: ") + std::strerror(errno));
    } catch (...) { close(fd); throw; }
    close(fd);
    return {{"sent", true}};
#else
    throw std::runtime_error("This helper was built without pidfd support; refusing unsafe PID-only signaling");
#endif
}
} // namespace observer
