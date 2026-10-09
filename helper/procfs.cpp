#include "observer.hpp"
#include "thread_tools.hpp"

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
#include <set>
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

// A snapshot explicitly requests only the metadata needed by visible columns
// and enabled highlighting. An omitted field list retains protocol-1 behavior.
struct ProcessFields {
    bool all = true;
    bool detect_32bit = false;
    int64_t default_uid = -1;
    std::set<std::string> names;
    explicit ProcessFields(const Json& request = Json::object()) {
        all = !request.contains("fields");
        if (!all && request["fields"].is_array())
            for (const auto& field : request["fields"])
                if (field.is_string()) names.insert(field.get<std::string>());
        detect_32bit = request.value("detect_32bit", false);
        default_uid = request.value("default_uid", int64_t{-1});
    }
    bool wants(std::initializer_list<const char*> fields) const {
        if (all) return true;
        for (const auto* field : fields) if (names.count(field)) return true;
        return false;
    }
};

std::string tty_name(int64_t number) {
    if (!number) return {};
    const auto device = static_cast<uint32_t>(number);
    const auto device_major = (device >> 8) & 0xfff;
    const auto device_minor = (device & 0xff) | ((device >> 12) & 0xfff00);
    if (device_major >= 136 && device_major <= 143)
        return "pts/" + std::to_string((device_major - 136) * 256 + device_minor);
    if (device_major == 4 && device_minor < 64) return "tty" + std::to_string(device_minor);
    if (device_major == 4) return "ttyS" + std::to_string(device_minor - 64);
    return std::to_string(device_major) + ":" + std::to_string(device_minor);
}

std::optional<bool> executable_is_32bit(int pid) {
    // Pin and validate before opening: an executable symlink must never cause
    // a special-device/FIFO read, including during an exec or process exit.
    const int pinned = open(proc_path(pid, "exe").c_str(), O_PATH | O_CLOEXEC);
    if (pinned < 0) return std::nullopt;
    struct stat info{};
    int file = -1;
    if (fstat(pinned, &info) == 0 && S_ISREG(info.st_mode))
        file = open(("/proc/self/fd/" + std::to_string(pinned)).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    close(pinned);
    if (file < 0) return std::nullopt;
    unsigned char header[EI_CLASS + 1]{};
    const auto length = pread(file, header, sizeof(header), 0);
    close(file);
    if (length != sizeof(header) || std::memcmp(header, ELFMAG, SELFMAG) != 0 ||
        (header[EI_CLASS] != ELFCLASS32 && header[EI_CLASS] != ELFCLASS64)) return std::nullopt;
    return header[EI_CLASS] == ELFCLASS32;
}

void suspension_state(Json& result, const ProcessStat& stat) {
    // A single-threaded process already supplies its only thread's state.
    // Avoid opening the task directory and rereading stat on every sample.
    if (stat.threads == 1) {
        const bool stopped = stat.state == "T" || stat.state == "t";
        result["stopped_threads"] = stopped ? 1 : 0;
        result["is_suspended"] = stopped;
        result["is_partially_suspended"] = false;
        return;
    }
    const auto path = proc_path(stat.pid, "task");
    DIR* tasks = opendir(path.c_str());
    if (!tasks) return;
    unsigned stopped = 0, observed = 0;
    bool incomplete = false;
    while (const auto* entry = readdir(tasks)) {
        const std::string tid(entry->d_name);
        if (tid.empty() || tid.find_first_not_of("0123456789") != std::string::npos) continue;
        const auto text = read_text(path + "/" + tid + "/stat", 16384);
        const auto end = text.rfind(')');
        if (end == std::string::npos || end + 2 >= text.size()) { incomplete = true; continue; }
        ++observed;
        if (text[end + 2] == 'T' || text[end + 2] == 't') ++stopped;
    }
    closedir(tasks);
    result["stopped_threads"] = stopped;
    // Thread lists are not atomic. Never label a process fully stopped when a
    // disappearing/unreadable thread or a changing count makes that uncertain.
    result["is_suspended"] = observed && !incomplete && observed == static_cast<unsigned>(stat.threads) && stopped == observed;
    result["is_partially_suspended"] = stopped > 0 && stopped < observed;
}

std::string descriptor_flags(uint64_t flags) {
    std::string text;
    const auto add = [&](const char* name) { if (!text.empty()) text += " | "; text += name; };
    if (flags & O_PATH) add("O_PATH");
    else switch (flags & O_ACCMODE) {
        case O_WRONLY: add("O_WRONLY"); break;
        case O_RDWR: add("O_RDWR"); break;
        default: add("O_RDONLY"); break;
    }
    for (const auto& flag : {std::pair<uint64_t, const char*>{O_APPEND, "O_APPEND"},
        {O_NONBLOCK, "O_NONBLOCK"}, {O_CLOEXEC, "O_CLOEXEC"}, {O_DIRECT, "O_DIRECT"},
        {O_NOATIME, "O_NOATIME"}, {O_DIRECTORY, "O_DIRECTORY"}, {O_NOFOLLOW, "O_NOFOLLOW"},
        {O_ASYNC, "O_ASYNC"}})
        if (flags & flag.first) add(flag.second);
    if ((flags & O_SYNC) == O_SYNC) add("O_SYNC");
    else if (flags & O_DSYNC) add("O_DSYNC");
    std::ostringstream raw;
    raw << " (0x" << std::hex << flags << ")";
    return text + raw.str();
}

std::string thread_wait_kind(const std::string& state, const std::string& wchan) {
    if (state == "T" || state == "t") return "suspended";
    if (state != "S" && state != "D" && state != "I") return {};
    // These are descriptive analogues, not Windows wait-reason identifiers.
    // Unknown kernel wait sites stay unclassified rather than guessing.
    if (wchan.find("nanosleep") != std::string::npos || wchan == "hrtimer_nanosleep") return "delay";
    if (wchan.find("futex") != std::string::npos) return "alert";
    if (wchan.find("ep_poll") != std::string::npos || wchan.find("epoll") != std::string::npos ||
        wchan.find("io_cqring_wait") != std::string::npos || wchan.find("do_mq_timed") != std::string::npos) return "queue";
    if (wchan.find("wait_for_completion") != std::string::npos || wchan.find("wait_on_page") != std::string::npos ||
        wchan.find("folio_wait") != std::string::npos || wchan.find("io_schedule") != std::string::npos) return "executive";
    if (wchan.find("sigtimedwait") != std::string::npos || wchan.find("sigsuspend") != std::string::npos ||
        wchan.find("do_wait") != std::string::npos || wchan.find("pipe_read") != std::string::npos ||
        wchan.find("unix_stream_read") != std::string::npos || wchan.find("do_select") != std::string::npos ||
        wchan.find("poll_schedule_timeout") != std::string::npos) return "user_request";
    return {};
}

std::string read_regular_file(const std::string& path, size_t limit) {
    const int pinned = open(path.c_str(), O_PATH | O_CLOEXEC);
    if (pinned < 0) return {};
    struct stat info{};
    int file = -1;
    if (fstat(pinned, &info) == 0 && S_ISREG(info.st_mode))
        file = open(("/proc/self/fd/" + std::to_string(pinned)).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    close(pinned);
    if (file < 0) return {};
    std::string result;
    char buffer[4096];
    while (result.size() < limit) {
        const auto count = read(file, buffer, std::min(sizeof(buffer), limit - result.size()));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        result.append(buffer, static_cast<size_t>(count));
    }
    close(file);
    return result;
}

std::map<std::string, std::string> literal_environment(const std::string& text) {
    std::map<std::string, std::string> result;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto equals = line.find('=', first);
        if (equals == std::string::npos) continue;
        const auto name = line.substr(first, equals - first);
        if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) continue;
        auto value = line.substr(equals + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
            value = value.substr(1, value.size() - 2);
        // Expansion and escapes depend on the login mechanism; avoid pretending
        // a literal parser can reconstruct those values reliably.
        if (value.find_first_of("$`\\") != std::string::npos) continue;
        result[name] = value;
    }
    return result;
}

std::map<std::string, std::string> account_environment(int pid, uint32_t uid) {
    std::istringstream lines(read_regular_file(proc_path(pid, "root/etc/passwd"), 1024 * 1024));
    std::string line;
    while (std::getline(lines, line)) {
        std::vector<std::string> fields;
        std::istringstream entry(line);
        std::string field;
        while (std::getline(entry, field, ':')) fields.push_back(field);
        if (fields.size() < 7) continue;
        try {
            if (unsigned_value(fields[2]) == uid)
                return {{"USER", fields[0]}, {"LOGNAME", fields[0]}, {"HOME", fields[5]}, {"SHELL", fields[6]}};
        } catch (...) { }
    }
    return {};
}

bool standard_library_path(const std::string& path) {
    for (const auto* prefix : {"/lib/", "/lib32/", "/lib64/", "/usr/lib/", "/usr/lib32/", "/usr/lib64/"})
        if (path.rfind(prefix, 0) == 0) return true;
    return false;
}

Json process_json(const ProcessStat& stat, const std::map<uid_t, std::string>& users,
    const ProcessFields& fields = ProcessFields{}) {
    Json result{{"pid", stat.pid}, {"ppid", stat.ppid}, {"start_ticks", stat.start_ticks},
        {"name", stat.name}, {"state", stat.state}, {"threads", stat.threads}, {"cpu_ticks", stat.cpu_ticks},
        {"rss_bytes", stat.rss_bytes}, {"virtual_bytes", stat.virtual_bytes},
        {"nice", stat.nice}, {"priority", stat.priority}, {"pgrp", stat.pgrp}, {"session", stat.session},
        {"tty_nr", stat.tty_nr}, {"tty", tty_name(stat.tty_nr)}, {"no_tty", stat.tty_nr == 0},
        {"minor_faults", stat.minor_faults}, {"major_faults", stat.major_faults},
        {"processor", stat.processor}, {"policy", stat.policy},
        {"user_ticks", stat.user_ticks}, {"kernel_ticks", stat.kernel_ticks}};
    const bool sudo_requested = fields.wants({"sudo", "sudo_root", "sudo_user", "sudo_uid", "sudo_gid", "sudo_command"});
    const bool user_requested = fields.wants({"user"});
    if (user_requested || sudo_requested || fields.wants({"status", "uid", "euid", "gid", "egid", "tracer_pid",
        "voluntary_switches", "involuntary_switches", "seccomp", "no_new_privs", "capabilities", "swap_bytes", "is_own", "security"})) {
        const auto status = read_text(proc_path(stat.pid, "status"), 64 * 1024);
        result["status_accessible"] = !status.empty();
        uint32_t uid = UINT32_MAX, euid = UINT32_MAX, gid = UINT32_MAX, egid = UINT32_MAX;
        std::istringstream(field_text(status, "Uid:")) >> uid >> euid;
        std::istringstream(field_text(status, "Gid:")) >> gid >> egid;
        result["uid"] = uid; result["euid"] = euid; result["gid"] = gid; result["egid"] = egid;
        result["is_own"] = fields.default_uid >= 0 && euid == static_cast<uint64_t>(fields.default_uid);
        result["tracer_pid"] = field_value(status, "TracerPid:");
        result["voluntary_switches"] = field_value(status, "voluntary_ctxt_switches:");
        result["involuntary_switches"] = field_value(status, "nonvoluntary_ctxt_switches:");
        result["swap_bytes"] = field_value(status, "VmSwap:") * 1024;
        result["seccomp"] = field_text(status, "Seccomp:");
        result["no_new_privs"] = field_text(status, "NoNewPrivs:") == "1";
        result["capabilities"] = field_text(status, "CapEff:");
        if (user_requested) {
            const auto account = users.find(euid);
            result["user"] = status.empty() ? "unknown" : account == users.end() ? std::to_string(euid) : account->second;
        }
        if (sudo_requested) {
            bool sudo_root = false;
            Json sudo_metadata = Json::object();
            if (euid == 0) {
                const auto environment = read_text(proc_path(stat.pid, "environ"), 256 * 1024);
                for (size_t begin = 0; begin < environment.size();) {
                    const auto end = environment.find('\0', begin);
                    if (end == std::string::npos) break; // Ignore a truncated final entry.
                    if (environment.compare(begin, 5, "SUDO_") != 0) {
                        begin = end + 1;
                        continue;
                    }
                    const auto entry = environment.substr(begin, end - begin);
                    const auto equals = entry.find('=');
                    if (equals != std::string::npos) {
                        const auto name = entry.substr(0, equals);
                        const auto value = entry.substr(equals + 1);
                        if (name == "SUDO_UID" || name == "SUDO_GID") {
                            if (!value.empty() && value.find_first_not_of("0123456789") == std::string::npos) {
                                try {
                                    const auto id = unsigned_value(value);
                                    sudo_metadata[name == "SUDO_UID" ? "sudo_uid" : "sudo_gid"] = id;
                                    if (name == "SUDO_UID") sudo_root = id != 0;
                                } catch (...) { }
                            }
                        } else if (name == "SUDO_USER") {
                            sudo_metadata["sudo_user"] = value.substr(0, 256);
                        } else if (name == "SUDO_COMMAND") {
                            sudo_metadata["sudo_command"] = value.substr(0, 16384);
                        }
                    }
                    begin = end + 1;
                }
            }
            // These inherited environment values are an origin hint, not a
            // verified ancestry record. Never expose them for ordinary root
            // processes (including sudo invoked by UID 0).
            if (sudo_root) result.update(sudo_metadata);
            result["sudo_root"] = sudo_root;
        }
    }
    if (fields.wants({"command"})) {
        auto command = read_text(proc_path(stat.pid, "cmdline"), 16 * 1024);
        std::replace(command.begin(), command.end(), '\0', ' ');
        if (!command.empty() && command.back() == ' ') command.pop_back();
        result["command"] = command;
    }
    if (fields.wants({"io", "read_bytes", "write_bytes", "read_chars", "write_chars", "syscr", "syscw", "cancelled_write_bytes"})) {
        const auto io = read_text(proc_path(stat.pid, "io"), 16384);
        result["io_accessible"] = !io.empty();
        for (const auto* key : {"read_bytes", "write_bytes", "syscr", "syscw", "cancelled_write_bytes"})
            result[key] = field_value(io, std::string(key) + ":");
        result["read_chars"] = field_value(io, "rchar:");
        result["write_chars"] = field_value(io, "wchar:");
    }
    if (fields.wants({"cgroup", "is_service", "service_unit", "service_scope"})) {
        const auto cgroup = read_text(proc_path(stat.pid, "cgroup"), 16384);
        result["cgroup"] = cgroup;
        // Use the innermost service component. A user service can be nested
        // below user@UID.service; reporting only that manager loses its identity.
        std::string unit, scope;
        size_t deepest = 0;
        std::istringstream lines(cgroup);
        std::string line;
        while (std::getline(lines, line)) {
            const auto controller = line.find(':');
            const auto path_begin = controller == std::string::npos ? std::string::npos : line.find(':', controller + 1);
            if (path_begin == std::string::npos) continue;
            std::istringstream components(line.substr(path_begin + 1));
            std::string component;
            bool within_user_manager = false;
            size_t depth = 0;
            while (std::getline(components, component, '/')) {
                ++depth;
                if (component.size() < 8 || component.compare(component.size() - 8, 8, ".service") != 0)
                    continue;
                if (depth >= deepest) {
                    deepest = depth;
                    unit = component;
                    scope = within_user_manager ? "user" : "system";
                }
                if (component.rfind("user@", 0) == 0)
                    within_user_manager = true;
            }
        }
        result["is_service"] = !unit.empty();
        result["service_unit"] = unit;
        result["service_scope"] = scope;
    }
    if (fields.wants({"exe", "runtime"})) {
        const auto executable = read_link(proc_path(stat.pid, "exe"));
        result["exe"] = executable;
        result["runtime"] = runtime_for_executable(executable);
    }
    if (fields.wants({"cwd"})) result["cwd"] = read_link(proc_path(stat.pid, "cwd"));
    if (fields.wants({"suspension", "is_suspended", "is_partially_suspended", "stopped_threads"})) suspension_state(result, stat);
    if (fields.detect_32bit && fields.wants({"elf32", "is_32bit"})) {
        const auto elf32 = executable_is_32bit(stat.pid);
        if (elf32) result["is_32bit"] = *elf32;
    }
    return result;
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
    result.pgrp = std::stoi(fields[2]);
    result.session = std::stoi(fields[3]);
    result.tty_nr = std::stoll(fields[4]);
    result.minor_faults = unsigned_value(fields[7]);
    result.major_faults = unsigned_value(fields[9]);
    result.user_ticks = unsigned_value(fields[11]);
    result.kernel_ticks = unsigned_value(fields[12]);
    result.priority = std::stoi(fields[15]);
    result.nice = std::stoi(fields[16]);
    if (fields.size() > 36) result.processor = std::stoi(fields[36]);
    if (fields.size() > 38) result.policy = std::stoi(fields[38]);
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
Json start_time_clock() {
    // /proc/<pid>/stat and /proc/<tid>/stat report start time as clock ticks
    // since boot. Linux's btime plus /proc/uptime lets the UI derive both the
    // wall-clock start time and the elapsed duration from one late sample.
    // Stream /proc/stat so this remains bounded even if it gains more rows.
    std::ifstream stat("/proc/stat");
    std::string line;
    uint64_t boot_time = 0;
    bool have_boot_time = false;
    while (std::getline(stat, line)) {
        if (line.rfind("btime ", 0) != 0) continue;
        std::istringstream value(line.substr(6));
        std::string trailing;
        if (!(value >> boot_time) || (value >> trailing)) return Json::object();
        have_boot_time = true;
        break;
    }
    const long ticks = sysconf(_SC_CLK_TCK);
    double uptime = 0;
    std::istringstream uptime_input(read_text("/proc/uptime", 128));
    if (!have_boot_time || ticks <= 0 || !(uptime_input >> uptime) || uptime < 0)
        return Json::object();
    return {{"boot_time_unix", boot_time}, {"clock_ticks", ticks}, {"uptime_seconds", uptime}};
}
Json hello() {
    return {{"protocol", 1}, {"boot_id", boot_id()}, {"uid", getuid()},
            {"cpus", sysconf(_SC_NPROCESSORS_ONLN)}, {"clock_ticks", sysconf(_SC_CLK_TCK)},
            {"systemd", access("/run/systemd/system", F_OK) == 0},
            {"gdb", access("/usr/bin/gdb", X_OK) == 0 || access("/bin/gdb", X_OK) == 0},
            {"helper_version", "0.1.0"}};
}
Json snapshot(const Json& request) {
    Json processes = Json::array();
    const ProcessFields fields(request);
    const auto users = fields.wants({"user"}) ? local_user_names() : std::map<uid_t, std::string>{};
    size_t bytes_left = 12 * 1024 * 1024;
    bool truncated = false;
    std::optional<ProcessIdentity> selected;
    if (request.contains("pid")) {
        selected = request_identity(request);
        require_identity(*selected);
    }
    const auto ids = selected ? std::vector<int>{selected->pid} : process_ids();
    for (int pid : ids) {
        try {
            const auto stat = process_stat(pid);
            auto process = process_json(stat, users, fields);
            // Do not combine metadata from two occupants of a rapidly reused PID.
            if (process_stat(pid).start_ticks == stat.start_ticks &&
                !append_with_budget(processes, std::move(process), bytes_left)) { truncated = true; break; }
        } catch (const std::exception&) {
            if (selected) throw;
            // Processes can disappear during an unrestricted scan.
        }
    }
    if (selected) require_identity(*selected);
    const auto memory = read_text("/proc/meminfo");
    double uptime = 0;
    std::istringstream(read_text("/proc/uptime")) >> uptime;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    Json result{{"processes", std::move(processes)}, {"processes_truncated", truncated},
            {"monotonic_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now).count()},
            {"uptime_seconds", uptime}, {"memory_total", field_value(memory, "MemTotal:") * 1024},
            {"memory_available", field_value(memory, "MemAvailable:") * 1024},
            {"cpus", sysconf(_SC_NPROCESSORS_ONLN)}, {"clock_ticks", sysconf(_SC_CLK_TCK)},
            {"boot_id", boot_id()}};
    if (request.contains("fields")) result["fields"] = request["fields"];
    if (fields.wants({"loadavg"})) result["loadavg"] = read_text("/proc/loadavg", 256);
    if (fields.wants({"pressure"}))
        result["pressure"] = "CPU\n" + read_text("/proc/pressure/cpu", 4096) + "\nMemory\n" +
            read_text("/proc/pressure/memory", 4096) + "\nI/O\n" + read_text("/proc/pressure/io", 4096);
    return result;
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
            uint64_t numeric_flags = 0;
            bool flags_known = false;
            try {
                size_t used = 0;
                numeric_flags = std::stoull(flags, &used, 8);
                flags_known = used == flags.size();
            } catch (...) { }
            if (!append_with_budget(files, {{"fd", std::stoi(name)},
                {"target", read_link(fd_path + "/" + name)}, {"flags", flags},
                {"flags_text", flags_known ? descriptor_flags(numeric_flags) : "Unknown"},
                {"position", field_value(info, "pos:")}, {"mount_id", field_value(info, "mnt_id:")},
                {"inode", field_value(info, "ino:")},
                {"inherited", flags_known && !(numeric_flags & O_CLOEXEC)}}, file_budget)) {
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
            {"deleted", deleted_mapping(mapping.path)},
            {"private_pages", mapping.permissions.find('p') != std::string::npos && mapping.inode == 0 &&
                (mapping.path.empty() || mapping.path == "[heap]" || mapping.path.rfind("[stack", 0) == 0 || mapping.path.rfind("[anon:", 0) == 0)},
            {"system_pages", mapping.path == "[vdso]" || mapping.path == "[vvar]" || mapping.path == "[vsyscall]" || mapping.path == "[vvar_vclock]"},
            {"execute_pages", mapping.permissions.find('x') != std::string::npos}}, memory_budget))
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
    struct stat executable_info{};
    const bool executable_known = stat(proc_path(identity.pid, "exe").c_str(), &executable_info) == 0;
    for (const auto* image : ordered_images) {
        const auto kind = inspect_mapped_image(identity.pid, image->representative);
        if (kind == ImageKind::Unavailable) { ++modules_unverified; continue; }
        if (kind != ImageKind::ElfImage) continue;
        const auto& mapping = image->representative;
        const auto basename = mapping.path.substr(mapping.path.find_last_of('/') + 1);
        const bool native_loader = standard_library_path(mapping.path) &&
            (basename.rfind("ld-linux", 0) == 0 || basename.rfind("ld-musl-", 0) == 0 || basename == "ld.so" || basename == "ld.so.1");
        const bool main_module = executable_known && static_cast<uint64_t>(executable_info.st_ino) == mapping.inode &&
            major(executable_info.st_dev) == mapping.device_major && minor(executable_info.st_dev) == mapping.device_minor;
        if (!append_with_budget(modules, {{"path", mapping.path}, {"base", hex_address(image->base)},
            {"end", hex_address(image->end)}, {"size_bytes", image->mapped_bytes},
            {"mapped_bytes", image->mapped_bytes}, {"device", mapping.device}, {"inode", mapping.inode},
            {"identity", mapping.device + ":" + std::to_string(mapping.inode)}, {"deleted", image->deleted},
            {"main_module", main_module}, {"native_module", native_loader},
            {"known_library", !main_module && !native_loader && standard_library_path(mapping.path)},
            {"mapped_module", false}}, module_budget)) {
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
            auto wchan = read_text(task_path + "/" + tid + "/wchan", 4096);
            while (!wchan.empty() && (wchan.back() == '\n' || wchan.back() == '\r')) wchan.pop_back();
            const auto state = text.substr(end + 2, 1);
            Json thread;
            try { thread = thread_snapshot(identity.pid, std::stoi(tid), text); }
            catch (const std::exception&) { continue; } // A racing task must not fail the whole inspector.
            thread["wchan"] = wchan;
            thread["wait_kind"] = thread_wait_kind(state, wchan);
            if (!append_with_budget(threads, std::move(thread), thread_budget)) {
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
    const auto process_status = read_text(proc_path(identity.pid, "status"), 64 * 1024);
    uint32_t environment_uid = UINT32_MAX, environment_euid = UINT32_MAX;
    std::istringstream(field_text(process_status, "Uid:")) >> environment_uid >> environment_euid;
    const auto user_environment = account_environment(identity.pid, environment_euid);
    const auto system_environment = literal_environment(read_regular_file(proc_path(identity.pid, "root/etc/environment"), 64 * 1024));
    const auto raw_environment = read_text(proc_path(identity.pid, "environ"), 4 * 1024 * 1024);
    bool environment_truncated = raw_environment.size() == 4 * 1024 * 1024;
    size_t environment_budget = 1024 * 1024;
    size_t offset = 0;
    while (offset < raw_environment.size()) {
        const auto end = raw_environment.find('\0', offset);
        if (end == std::string::npos && environment_truncated) break;
        const auto entry = raw_environment.substr(offset, end == std::string::npos ? end : end - offset);
        const auto equals = entry.find('=');
        const auto name = entry.substr(0, equals);
        const auto value = equals == std::string::npos ? "" : entry.substr(equals + 1);
        const auto user_value = user_environment.find(name), system_value = system_environment.find(name);
        const char* scope = "process";
        if (system_value != system_environment.end() && system_value->second == value) scope = "system";
        else if (user_value != user_environment.end() && user_value->second == value) scope = "user";
        if (!append_with_budget(environment, {{"name", name}, {"value", value}, {"scope", scope}}, environment_budget)) {
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
    summary += "\nEnvironment colors compare exact values with /etc/environment and the account's USER, LOGNAME, HOME and SHELL. "
        "They describe matching baselines, not a proven inheritance history. Other variables are process-specific.\n";
    summary += "\nHandle inheritance means close-on-exec is not set; Linux descriptors are normally inherited across fork regardless.\n";
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
    const auto clock = start_time_clock();
    return {{"overview", overview}, {"threads", threads}, {"environment", environment}, {"summary", summary}, {"files", files}, {"modules", modules}, {"memory", memory},
            {"clock", clock},
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
    auto annotate_frames = [](const std::string& output, const std::string& maps_before,
                              const std::string& maps_after) {
        // Keep GDB's source and ELF/minimal-symbol names. Where symbols are
        // absent, add the mapped file and a file offset (not an ELF virtual RVA).
        // Only use mappings unchanged across attachment: the process resumes
        // before the second snapshot, so a newly mapped file may be unrelated
        // to the captured PC. This also avoids attributing frames after exec.
        std::set<std::string> previous_mappings;
        std::istringstream previous_lines(maps_before);
        std::string line;
        while (std::getline(previous_lines, line)) previous_mappings.insert(line);

        struct Mapping { uint64_t start, end, offset; std::string path; };
        std::vector<Mapping> mappings;
        std::istringstream map_lines(maps_after);
        while (std::getline(map_lines, line)) {
            if (!previous_mappings.count(line)) continue;
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

        std::istringstream stack_lines(output);
        std::string annotated;
        while (std::getline(stack_lines, line)) {
            // Parse only a backtrace's leading frame number and PC. Searching
            // arbitrary hexadecimal text could mistake an argument or warning
            // address for the instruction pointer.
            const auto frame_end = line.find_first_not_of("0123456789", 1);
            if (line.size() > 1 && line.front() == '#' && frame_end > 1 &&
                frame_end != std::string::npos && (line[frame_end] == ' ' || line[frame_end] == '\t')) {
                const auto address_start = line.find_first_not_of(" \t", frame_end);
                if (address_start != std::string::npos && line.compare(address_start, 2, "0x") == 0) {
                    try {
                        size_t consumed = 0;
                        const auto address = std::stoull(line.substr(address_start), &consumed, 16);
                        const auto address_end = address_start + consumed;
                        const bool complete_address = consumed > 2 && (address_end == line.size() ||
                            line[address_end] == ' ' || line[address_end] == '\t');
                        for (const auto& mapping : mappings) {
                            if (!complete_address || address < mapping.start || address >= mapping.end ||
                                mapping.offset > std::numeric_limits<uint64_t>::max() - (address - mapping.start))
                                continue;
                            std::ostringstream location;
                            location << "[" << mapping.path << " file+0x" << std::hex <<
                                (mapping.offset + address - mapping.start) << "]";
                            const auto function_start = line.find_first_not_of(" \t", address_end);
                            if (function_start != std::string::npos && line.compare(function_start, 5, "in ??") == 0 &&
                                (function_start + 5 == line.size() || line[function_start + 5] == ' ' ||
                                 line[function_start + 5] == '('))
                                line.replace(function_start + 3, 2, location.str());
                            else
                                line += " " + location.str();
                            break;
                        }
                    } catch (const std::exception&) { }
                }
            }
            if (annotated.size() + line.size() + 1 > 512 * 1024) {
                annotated += "[Annotated output truncated]\n";
                break;
            }
            annotated += line + "\n";
        }
        return annotated;
    };
    auto stack_first = [](const std::string& output) {
        std::istringstream lines(output);
        std::string line, frames, diagnostics;
        bool found_frames = false;
        while (std::getline(lines, line)) {
            const bool frame = line.size() > 1 && line[0] == '#' &&
                line[1] >= '0' && line[1] <= '9';
            const bool thread = line.rfind("Thread ", 0) == 0 && line.size() > 7 &&
                line[7] >= '0' && line[7] <= '9';
            if (frame || thread) found_frames = true;
            // These are attach/detach announcements, not capture failures.
            if (line.rfind("[New LWP ", 0) == 0 ||
                line.rfind("[Thread debugging using libthread_db enabled]", 0) == 0 ||
                line.rfind("Using host libthread_db library ", 0) == 0 ||
                (line.rfind("[Inferior ", 0) == 0 && line.find(" detached]") != std::string::npos))
                continue;
            if (found_frames) frames += line + "\n";
            else if (!line.empty() && line.rfind("0x", 0) != 0)
                diagnostics += line + "\n";
        }
        // Preserve a failed attach verbatim: never disguise diagnostics as an
        // empty successful capture. Source/symbol warnings remain available.
        if (!found_frames) return output;
        if (!diagnostics.empty()) frames += "\nGDB diagnostics\n" + diagnostics;
        return frames;
    };
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
            "-iex", "set print thread-events off", "-iex", "set print inferior-events off",
            "-ex", "set print frame-arguments none", "-ex", "set print entry-values no",
            "-ex", "attach " + std::to_string(identity.pid), "-ex", backtrace};
        arguments.insert(arguments.end(), options.begin(), options.end());
        if (minimal_symbols) arguments.insert(arguments.end(), {"-ex", "info sharedlibrary"});
        arguments.insert(arguments.end(), {"-ex", "detach"});
        // GDB cannot attach through a pidfd. Recheck each numeric attachment,
        // including a retry, without claiming this closes the PID-reuse race.
        require_identity(identity);
        const auto maps_before = read_text(proc_path(identity.pid, "maps"), 8 * 1024 * 1024);
        auto result = run_command(arguments, 15000, 256 * 1024);
        require_identity(identity);
        const auto maps_after = read_text(proc_path(identity.pid, "maps"), 8 * 1024 * 1024);
        require_identity(identity);
        result.output = stack_first(annotate_frames(result.output, maps_before, maps_after));
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

        output = message + "\n\nMinimal-symbol backtrace\n" + result.output +
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
    if (signal != SIGTERM && signal != SIGINT && signal != SIGKILL && signal != SIGSTOP && signal != SIGCONT && signal != SIGUSR1 && signal != SIGUSR2 && signal != SIGHUP && signal != SIGWINCH)
        throw std::runtime_error("Unsupported signal; use TERM, INT, KILL, STOP, CONT, USR1, USR2, HUP, or WINCH");
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
