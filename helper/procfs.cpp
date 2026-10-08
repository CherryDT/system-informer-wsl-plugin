#include "observer.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
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
std::map<uid_t, std::string> local_user_names() {
    // A statically linked helper must not load distribution-specific NSS modules
    // or stall a snapshot on LDAP. Resolve local accounts and retain numeric UIDs
    // for accounts supplied by other name services.
    std::map<uid_t, std::string> users;
    std::istringstream lines(read_text("/etc/passwd", 1024 * 1024));
    std::string line;
    while (std::getline(lines, line)) {
        const auto first = line.find(':');
        if (first == std::string::npos) continue;
        const auto second = line.find(':', first + 1);
        if (second == std::string::npos) continue;
        const auto third = line.find(':', second + 1);
        if (third == std::string::npos) continue;
        try {
            const auto uid = unsigned_value(line.substr(second + 1, third - second - 1));
            if (uid <= std::numeric_limits<uid_t>::max())
                users.emplace(static_cast<uid_t>(uid), line.substr(0, first));
        } catch (const std::exception&) { }
    }
    return users;
}
std::string field_text(const std::string& text, const std::string& key) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.compare(0, key.size(), key) != 0) continue;
        const auto begin = line.find_first_not_of(" \t", key.size());
        if (begin == std::string::npos) return {};
        const auto end = line.find_last_not_of(" \t\r");
        return line.substr(begin, end - begin + 1);
    }
    return {};
}
uint64_t field_value(const std::string& text, const std::string& key) {
    uint64_t number = 0;
    std::istringstream(field_text(text, key)) >> number;
    return number;
}
struct MemoryMapping {
    uint64_t start = 0, end = 0, offset = 0, inode = 0;
    unsigned int device_major = 0, device_minor = 0;
    std::string start_text, end_text, offset_text, permissions, device, path;
};

struct MappedImage {
    MemoryMapping representative;
    uint64_t base = 0, end = 0, mapped_bytes = 0;
    bool executable = false, deleted = false;
};

enum class ImageKind { ElfImage, OtherFile, Unavailable };

bool deleted_mapping(const std::string& path) {
    constexpr const char* suffix = " (deleted)";
    constexpr size_t suffix_length = 10;
    return path.size() >= suffix_length && path.compare(path.size() - suffix_length, suffix_length, suffix) == 0;
}

ImageKind inspect_mapped_image(int pid, const MemoryMapping& mapping) {
    const auto map_file = proc_path(pid, ("map_files/" + mapping.start_text + "-" + mapping.end_text).c_str());
    int pinned = open(map_file.c_str(), O_PATH | O_CLOEXEC);
    if (pinned < 0 && !deleted_mapping(mapping.path) && !mapping.path.empty() && mapping.path.front() == '/') {
        // map_files may require privileges unavailable even to a container's
        // root. Resolve a live pathname in the target's mount namespace, then
        // insist that it still names exactly the device/inode in /proc/maps.
        pinned = open((proc_path(pid, "root") + mapping.path).c_str(), O_PATH | O_CLOEXEC);
    }
    if (pinned < 0) return ImageKind::Unavailable;
    struct stat info{};
    const bool matches = fstat(pinned, &info) == 0 && S_ISREG(info.st_mode) &&
        static_cast<uint64_t>(info.st_ino) == mapping.inode &&
        major(info.st_dev) == mapping.device_major && minor(info.st_dev) == mapping.device_minor;
    if (!matches) { close(pinned); return ImageKind::Unavailable; }

    // O_PATH follows the proc symlink without opening the underlying file for
    // I/O. Only after checking the pinned inode is a regular file do we reopen
    // our own descriptor for a tiny read. A pathname swap cannot turn this into
    // a FIFO/device open, and a deleted mapping cannot select a replacement file.
    const auto descriptor_path = "/proc/self/fd/" + std::to_string(pinned);
    const int readable = open(descriptor_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    close(pinned);
    if (readable < 0) return ImageKind::Unavailable;
    unsigned char header[EI_NIDENT + 2]{};
    ssize_t length;
    do { length = pread(readable, header, sizeof(header), 0); } while (length < 0 && errno == EINTR);
    close(readable);
    if (length < 0) return ImageKind::Unavailable;
    if (length != static_cast<ssize_t>(sizeof(header)) || std::memcmp(header, ELFMAG, SELFMAG) != 0 ||
        (header[EI_CLASS] != ELFCLASS32 && header[EI_CLASS] != ELFCLASS64) ||
        (header[EI_DATA] != ELFDATA2LSB && header[EI_DATA] != ELFDATA2MSB) || header[EI_VERSION] != EV_CURRENT)
        return ImageKind::OtherFile;
    const auto type = header[EI_DATA] == ELFDATA2LSB ? header[EI_NIDENT] | (header[EI_NIDENT + 1] << 8) :
        (header[EI_NIDENT] << 8) | header[EI_NIDENT + 1];
    return type == ET_EXEC || type == ET_DYN ? ImageKind::ElfImage : ImageKind::OtherFile;
}

std::string hex_address(uint64_t address) {
    std::ostringstream text;
    text << std::hex << address;
    return text.str();
}

Json process_json(const ProcessStat& stat, const std::map<uid_t, std::string>& users) {
    const auto status = read_text(proc_path(stat.pid, "status"));
    const auto uid = status.empty() ? std::numeric_limits<uid_t>::max() :
        static_cast<uid_t>(field_value(status, "Uid:"));
    const auto account = users.find(uid);
    const auto user = status.empty() ? "unknown" : account == users.end() ? std::to_string(uid) : account->second;
    auto command = read_text(proc_path(stat.pid, "cmdline"), 16 * 1024);
    std::replace(command.begin(), command.end(), '\0', ' ');
    if (!command.empty() && command.back() == ' ') command.pop_back();
    const auto io = read_text(proc_path(stat.pid, "io"));
    const auto cgroup = read_text(proc_path(stat.pid, "cgroup"), 4096);
    const auto executable = read_link(proc_path(stat.pid, "exe"));
    return {{"pid", stat.pid}, {"ppid", stat.ppid}, {"start_ticks", stat.start_ticks},
            {"name", stat.name}, {"state", stat.state}, {"user", user},
            {"status_accessible", !status.empty()}, {"tracer_pid", field_value(status, "TracerPid:")},
            {"is_service", cgroup.find("/system.slice/") != std::string::npos && cgroup.find(".service") != std::string::npos},
            {"uid", uid}, {"threads", stat.threads}, {"cpu_ticks", stat.cpu_ticks},
            {"rss_bytes", stat.rss_bytes}, {"virtual_bytes", stat.virtual_bytes},
            {"read_bytes", field_value(io, "read_bytes:")},
            {"write_bytes", field_value(io, "write_bytes:")},
            {"io_accessible", !io.empty()}, {"command", command},
            {"exe", executable}, {"runtime", runtime_for_executable(executable)}};
}
}

bool append_with_budget(Json& array, Json item, size_t& bytes_left) {
    const auto size = item.dump(-1, ' ', false, Json::error_handler_t::replace).size() + 1;
    if (size > bytes_left) return false;
    bytes_left -= size;
    array.push_back(std::move(item));
    return true;
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
    if (begin == std::string::npos || end == std::string::npos || end <= begin || end + 2 >= text.size())
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
            {"gdb", access("/usr/bin/gdb", X_OK) == 0 || access("/bin/gdb", X_OK) == 0},
            {"helper_version", "0.1.0"}};
}
Json snapshot() {
    Json processes = Json::array();
    const auto users = local_user_names();
    size_t bytes_left = 12 * 1024 * 1024;
    bool truncated = false;
    for (int pid : process_ids()) {
        try {
            const auto stat = process_stat(pid);
            auto process = process_json(stat, users);
            // Do not combine metadata from two occupants of a rapidly reused PID.
            if (process_stat(pid).start_ticks == stat.start_ticks &&
                !append_with_budget(processes, std::move(process), bytes_left)) { truncated = true; break; }
        } catch (const std::exception&) { /* Processes can disappear during a scan. */ }
    }
    const auto memory = read_text("/proc/meminfo");
    double uptime = 0;
    std::istringstream(read_text("/proc/uptime")) >> uptime;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return {{"processes", std::move(processes)}, {"processes_truncated", truncated},
            {"monotonic_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now).count()},
            {"uptime_seconds", uptime}, {"memory_total", field_value(memory, "MemTotal:") * 1024},
            {"memory_available", field_value(memory, "MemAvailable:") * 1024},
            {"cpus", sysconf(_SC_NPROCESSORS_ONLN)}, {"clock_ticks", sysconf(_SC_CLK_TCK)},
            {"boot_id", boot_id()},
            {"loadavg", read_text("/proc/loadavg", 256)},
            {"pressure", "CPU\n" + read_text("/proc/pressure/cpu", 4096) + "\nMemory\n" + read_text("/proc/pressure/memory", 4096) + "\nI/O\n" + read_text("/proc/pressure/io", 4096)}};
}
Json process_details(const Json& request) {
    const auto identity = request_identity(request);
    require_identity(identity);
    Json files = Json::array(), modules = Json::array(), memory = Json::array();
    size_t file_budget = 1024 * 1024, module_budget = 1024 * 1024, memory_budget = 1024 * 1024;
    bool files_truncated = false, modules_truncated = false, memory_truncated = false;
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
            if (!append_with_budget(files, {{"fd", std::stoi(name)},
                {"target", read_link(fd_path + "/" + name)}, {"flags", flags}}, file_budget)) {
                files_truncated = true; break;
            }
        }
        closedir(directory);
    }
    std::sort(files.begin(), files.end(), [](const Json& a, const Json& b) { return a["fd"].get<int>() < b["fd"].get<int>(); });
    const auto maps = read_text(proc_path(identity.pid, "maps"), 8 * 1024 * 1024);
    const bool maps_truncated = maps.size() == 8 * 1024 * 1024;
    std::istringstream lines(maps);
    std::string line;
    std::map<std::pair<std::string, uint64_t>, MappedImage> images;
    while (std::getline(lines, line)) {
        // Do not turn a cap-truncated final line into a misleading filename.
        if (maps_truncated && lines.eof()) break;
        std::istringstream fields(line);
        std::string range, inode_text;
        MemoryMapping mapping;
        if (!(fields >> range >> mapping.permissions >> mapping.offset_text >> mapping.device >> inode_text)) continue;
        std::getline(fields >> std::ws, mapping.path);
        const auto dash = range.find('-'), colon = mapping.device.find(':');
        if (dash == std::string::npos || colon == std::string::npos) continue;
        mapping.start_text = range.substr(0, dash);
        mapping.end_text = range.substr(dash + 1);
        try {
            mapping.start = std::stoull(mapping.start_text, nullptr, 16);
            mapping.end = std::stoull(mapping.end_text, nullptr, 16);
            mapping.offset = std::stoull(mapping.offset_text, nullptr, 16);
            mapping.inode = unsigned_value(inode_text);
            mapping.device_major = std::stoul(mapping.device.substr(0, colon), nullptr, 16);
            mapping.device_minor = std::stoul(mapping.device.substr(colon + 1), nullptr, 16);
        } catch (const std::exception&) { continue; }
        if (mapping.end <= mapping.start) continue;
        const auto length = mapping.end - mapping.start;
        if (!memory_truncated && !append_with_budget(memory, {
            {"start", mapping.start_text}, {"end", mapping.end_text}, {"size_bytes", length},
            {"permissions", mapping.permissions}, {"offset", mapping.offset_text},
            {"path", mapping.path}, {"device", mapping.device}, {"inode", mapping.inode},
            {"deleted", deleted_mapping(mapping.path)}}, memory_budget))
            memory_truncated = true;

        // Anonymous VMAs, JIT regions, [heap], [stack], and [vdso] belong in
        // Memory. Named data files remain there unless verified as an ELF image.
        if (mapping.inode == 0 || mapping.path.empty() || mapping.path.front() != '/') continue;
        const auto key = std::make_pair(mapping.device, mapping.inode);
        auto inserted = images.emplace(key, MappedImage{mapping, mapping.start, mapping.end, 0, false, false});
        auto& image = inserted.first->second;
        image.base = std::min(image.base, mapping.start);
        image.end = std::max(image.end, mapping.end);
        image.mapped_bytes += length;
        image.executable = image.executable || mapping.permissions.find('x') != std::string::npos;
        image.deleted = image.deleted || deleted_mapping(mapping.path);
        if (mapping.offset == 0) image.representative = mapping;
    }
    size_t modules_unverified = 0;
    std::vector<const MappedImage*> ordered_images;
    for (const auto& entry : images) if (entry.second.executable) ordered_images.push_back(&entry.second);
    std::sort(ordered_images.begin(), ordered_images.end(), [](const MappedImage* a, const MappedImage* b) {
        return a->base < b->base;
    });
    for (const auto* image : ordered_images) {
        const auto kind = inspect_mapped_image(identity.pid, image->representative);
        if (kind == ImageKind::Unavailable) { ++modules_unverified; continue; }
        if (kind != ImageKind::ElfImage) continue;
        const auto& mapping = image->representative;
        if (!append_with_budget(modules, {{"path", mapping.path}, {"base", hex_address(image->base)},
            {"end", hex_address(image->end)}, {"size_bytes", image->mapped_bytes},
            {"mapped_bytes", image->mapped_bytes}, {"device", mapping.device}, {"inode", mapping.inode},
            {"identity", mapping.device + ":" + std::to_string(mapping.inode)}, {"deleted", image->deleted}}, module_budget)) {
            modules_truncated = true;
            break;
        }
    }
    const std::string module_classification_note =
        "Modules are verified ELF executables/shared objects with an executable mapping, grouped by device and inode. "
        "Mapped size sums segments and excludes gaps. Anonymous executable regions, [vdso], and unverified files remain in Memory." +
        (modules_unverified ? " " + std::to_string(modules_unverified) + " executable mapped files could not be verified." : "");
    std::string namespace_text;
    for (const auto* name : {"cgroup", "ipc", "mnt", "net", "pid", "pid_for_children", "time", "user", "uts"})
        namespace_text += std::string(name) + ": " + read_link(proc_path(identity.pid, (std::string("ns/") + name).c_str())) + "\n";
    Json threads = Json::array();
    size_t thread_budget = 512 * 1024;
    bool threads_truncated = false;
    const auto task_path = proc_path(identity.pid, "task");
    DIR* tasks = opendir(task_path.c_str());
    if (tasks) {
        while (const auto* entry = readdir(tasks)) {
            const std::string tid(entry->d_name);
            if (tid.empty() || tid.find_first_not_of("0123456789") != std::string::npos) continue;
            const auto text = read_text(task_path + "/" + tid + "/stat", 16384);
            const auto begin = text.find('('), end = text.rfind(')');
            if (begin == std::string::npos || end == std::string::npos || end + 2 >= text.size()) continue;
            if (!append_with_budget(threads, {{"tid", std::stoi(tid)}, {"name", text.substr(begin + 1, end - begin - 1)},
                {"state", text.substr(end + 2, 1)}, {"wchan", read_text(task_path + "/" + tid + "/wchan", 4096)}}, thread_budget)) {
                threads_truncated = true; break;
            }
        }
        closedir(tasks);
    }
    auto summary = "Status\n" + read_text(proc_path(identity.pid, "status")) +
        "\nI/O counters\n" + read_text(proc_path(identity.pid, "io")) +
        "\nControl groups\n" + read_text(proc_path(identity.pid, "cgroup")) +
        "\nLimits\n" + read_text(proc_path(identity.pid, "limits")) +
        "\nNamespaces\n" + namespace_text +
        "\nExecutable: " + read_link(proc_path(identity.pid, "exe")) +
        "\nWorking directory: " + read_link(proc_path(identity.pid, "cwd")) + "\n";
    Json environment = Json::array();
    const auto raw_environment = read_text(proc_path(identity.pid, "environ"), 4 * 1024 * 1024);
    bool environment_truncated = raw_environment.size() == 4 * 1024 * 1024;
    size_t environment_budget = 1024 * 1024;
    size_t offset = 0;
    while (offset < raw_environment.size()) {
        const auto end = raw_environment.find('\0', offset);
        if (end == std::string::npos && environment_truncated) break;
        const auto entry = raw_environment.substr(offset, end == std::string::npos ? end : end - offset);
        const auto equals = entry.find('=');
        if (!append_with_budget(environment, {{"name", entry.substr(0, equals)},
            {"value", equals == std::string::npos ? "" : entry.substr(equals + 1)}}, environment_budget)) {
            environment_truncated = true; break;
        }
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    const bool summary_truncated = summary.size() > 256 * 1024;
    if (summary_truncated) summary.resize(256 * 1024);
    if (files_truncated || modules_truncated || memory_truncated || maps_truncated || environment_truncated || threads_truncated || summary_truncated)
        summary += "\nSome inspection data was truncated to keep the response within transport limits.\n";
    summary += "\nModule classification\n" + module_classification_note + "\n";
    auto overview = process_json(process_stat(identity.pid), local_user_names());
    const auto status = read_text(proc_path(identity.pid, "status"));
    overview["cwd"] = read_link(proc_path(identity.pid, "cwd"));
    overview["cgroup"] = read_text(proc_path(identity.pid, "cgroup"), 16 * 1024);
    overview["capabilities"] = "Effective: " + field_text(status, "CapEff:") +
        "\nPermitted: " + field_text(status, "CapPrm:") +
        "\nInheritable: " + field_text(status, "CapInh:") +
        "\nBounding: " + field_text(status, "CapBnd:") +
        "\nAmbient: " + field_text(status, "CapAmb:");
    const auto seccomp = field_text(status, "Seccomp:");
    overview["seccomp"] = seccomp == "0" ? "Disabled" : seccomp == "1" ? "Strict" :
        seccomp == "2" ? "Filter" : seccomp;
    const auto no_new_privs = field_text(status, "NoNewPrivs:");
    overview["no_new_privs"] = no_new_privs == "1" ? "Yes" : no_new_privs == "0" ? "No" : no_new_privs;
    require_identity(identity);
    return {{"overview", overview}, {"threads", threads}, {"environment", environment}, {"summary", summary}, {"files", files}, {"modules", modules}, {"memory", memory},
            {"files_accessible", files_accessible}, {"modules_accessible", !maps.empty()},
            {"memory_accessible", !maps.empty()}, {"modules_unverified", modules_unverified},
            {"module_classification_note", module_classification_note},
            {"files_truncated", files_truncated}, {"modules_truncated", maps_truncated || modules_truncated},
            {"memory_truncated", maps_truncated || memory_truncated},
            {"environment_truncated", environment_truncated}, {"threads_truncated", threads_truncated},
            {"summary_truncated", summary_truncated}};
}
Json process_stacks(const Json& request) {
    const auto identity = request_identity(request);
    if (identity.pid <= 1 || identity.pid == getpid())
        throw std::runtime_error("Debugger attachment to this process is protected");
    require_identity(identity);
    if (access("/usr/bin/gdb", X_OK) != 0 && access("/bin/gdb", X_OK) != 0)
        return {{"available", false}, {"text", ""},
            {"message", "GDB is not installed. Install gdb in this distribution to inspect user-space thread stacks."}};

    std::string backtrace = "thread apply all bt 64";
    if (request.contains("tid")) {
        const auto& requested_tid = request["tid"];
        if (!requested_tid.is_number_integer() || requested_tid.get<int64_t>() <= 0 ||
            requested_tid.get<int64_t>() > std::numeric_limits<int>::max())
            throw std::runtime_error("Invalid thread ID");
        const auto tid = requested_tid.get<int>();
        if (read_text(proc_path(identity.pid, ("task/" + std::to_string(tid) + "/stat").c_str()), 16384).empty())
            throw std::runtime_error("Thread exited or is not part of this process");
        // GDB's thread numbers differ from Linux TIDs. This is a fixed Python
        // expression with one validated integer, never a user-provided command.
        backtrace = "python t = next(t for t in gdb.selected_inferior().threads() if t.ptid[1] == " +
            std::to_string(tid) + "); t.switch(); gdb.execute('bt 64')";
    }
    auto capture = [&](bool minimal_symbols) {
        // --readnever skips DWARF while retaining ELF minimal symbols, including
        // exported functions. Shared-library loading stays enabled for libc and
        // other module names; --se supplies the main executable's ELF symbols.
        std::vector<std::string> arguments{"gdb", "--nx", "--nh", "--batch", "--quiet"};
        if (minimal_symbols) {
            arguments.push_back("--readnever");
            arguments.push_back("--se=" + proc_path(identity.pid, "exe"));
        }
        const std::vector<std::string> options{
            "-iex", "set auto-load off", "-iex", "set debuginfod enabled off",
            "-iex", "set index-cache enabled off", "-iex", "set libthread-db-search-path $sdir",
            "-iex", "maintenance set internal-error corefile no",
            "-iex", "maintenance set internal-error quit yes",
            "-iex", "maintenance set internal-warning corefile no",
            "-iex", "set exec-file-mismatch off",
            "-ex", "set pagination off", "-ex", "set confirm off",
            "-ex", "set print frame-arguments none", "-ex", "set print entry-values no",
            "-ex", "attach " + std::to_string(identity.pid), "-ex", backtrace};
        arguments.insert(arguments.end(), options.begin(), options.end());
        if (minimal_symbols) arguments.insert(arguments.end(), {"-ex", "info sharedlibrary"});
        arguments.insert(arguments.end(), {"-ex", "detach"});
        // GDB cannot attach through a pidfd. Recheck each numeric attachment,
        // including a retry, without claiming this closes the PID-reuse race.
        require_identity(identity);
        auto result = run_command(arguments, 15000, 256 * 1024);
        require_identity(identity);
        return result;
    };
    auto primary = capture(false);
    const std::vector<std::string> symbol_failures{
        "DWARF Error", "Dwarf Error", "dwarf2/", "DW_TAG_skeleton_unit",
        "Could not find DWO CU", "could not find DWO CU", "error reading .dwo",
        "Recursive internal problem", "internal-error:"};
    const bool needs_fallback = std::any_of(symbol_failures.begin(), symbol_failures.end(),
        [&](const std::string& marker) { return primary.output.find(marker) != std::string::npos; });
    auto result = primary;
    std::string message;
    std::string output = primary.output;
    if (needs_fallback) {
        result = capture(true);
        message = "GDB encountered a debug-information error. Retried with ELF minimal symbols; "
            "exported function and module names remain available, but source lines and reliable DWARF unwinding may be unavailable.";

        // Preserve useful module context even for stripped frames named '??'.
        // This is a mapped-file offset, not a promise about an ELF virtual RVA.
        struct Mapping { uint64_t start, end, offset; std::string path; };
        std::vector<Mapping> mappings;
        std::istringstream map_lines(read_text(proc_path(identity.pid, "maps"), 8 * 1024 * 1024));
        std::string line;
        while (std::getline(map_lines, line)) {
            std::istringstream fields(line);
            std::string range, permissions, offset, device, inode, path;
            if (!(fields >> range >> permissions >> offset >> device >> inode)) continue;
            std::getline(fields >> std::ws, path);
            const auto dash = range.find('-');
            if (path.empty() || path.front() != '/' || dash == std::string::npos) continue;
            try {
                mappings.push_back({std::stoull(range.substr(0, dash), nullptr, 16),
                    std::stoull(range.substr(dash + 1), nullptr, 16), std::stoull(offset, nullptr, 16), path});
            } catch (const std::exception&) { }
        }
        std::istringstream stack_lines(result.output);
        std::string annotated;
        while (std::getline(stack_lines, line)) {
            const auto address_start = line.find("0x");
            if (!line.empty() && line.front() == '#' && address_start != std::string::npos) {
                try {
                    const auto address = std::stoull(line.substr(address_start), nullptr, 16);
                    for (const auto& mapping : mappings) if (address >= mapping.start && address < mapping.end) {
                        std::ostringstream location;
                        location << " [" << mapping.path << " file+0x" << std::hex <<
                            (mapping.offset + address - mapping.start) << "]";
                        line += location.str();
                        break;
                    }
                } catch (const std::exception&) { }
            }
            if (annotated.size() + line.size() + 1 > 512 * 1024) {
                annotated += "[Annotated output truncated]\n";
                break;
            }
            annotated += line + "\n";
        }
        output = message + "\n\nMinimal-symbol backtrace\n" + annotated +
            "\nOriginal GDB diagnostics\n" + primary.output;
    }
    if (result.timed_out) {
        if (!message.empty()) message += " ";
        message += "GDB timed out after 15 seconds; the debugger was terminated.";
    } else if (result.exit_code != 0) {
        if (!message.empty()) message += " ";
        message += "GDB could not collect every requested stack. See its diagnostic output; ptrace permissions and debug symbols may be required.";
    }
    require_identity(identity);
    return {{"available", true}, {"text", output}, {"message", message},
        {"fallback", needs_fallback}, {"timed_out", result.timed_out},
        {"exit_code", result.exit_code}, {"primary_exit_code", primary.exit_code}};
}
Json send_signal(const Json& request) {
    const auto identity = request_identity(request);
    const auto& requested_signal = request.at("signal");
    if (!requested_signal.is_number_integer() || requested_signal.get<int64_t>() < 1 ||
        requested_signal.get<int64_t>() > 64)
        throw std::runtime_error("Invalid signal number");
    const int signal = requested_signal.get<int>();
    if (identity.pid <= 1 || identity.pid == getpid()) throw std::runtime_error("This process is protected");
    if (signal != SIGTERM && signal != SIGKILL && signal != SIGSTOP && signal != SIGCONT && signal != SIGUSR1 && signal != SIGUSR2 && signal != SIGHUP && signal != SIGWINCH)
        throw std::runtime_error("Unsupported signal; use TERM, KILL, STOP, CONT, USR1, USR2, HUP, or WINCH");
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
