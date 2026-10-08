#include "resource_tools.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <unistd.h>

namespace observer {
namespace {
constexpr size_t MapsLimit = 8 * 1024 * 1024;
constexpr size_t TextLimit = 2 * 1024 * 1024;
constexpr size_t MemoryReadLimit = 1024 * 1024;
constexpr size_t StringScanLimit = 16 * 1024 * 1024;

struct Descriptor {
    int value;
    explicit Descriptor(int fd) : value(fd) {}
    ~Descriptor() { if (value >= 0) close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};

std::string proc_path(int pid, const std::string& leaf) {
    return "/proc/" + std::to_string(pid) + "/" + leaf;
}
std::string hex(uint64_t value) {
    std::ostringstream result;
    result << "0x" << std::hex << value;
    return result.str();
}
std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
uint64_t number(const std::string& text, unsigned base, const std::string& label) {
    size_t position = 0;
    if (base == 16 && text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) position = 2;
    if (position == text.size()) throw std::runtime_error(label + " must be an unsigned number");
    uint64_t result = 0;
    for (; position < text.size(); ++position) {
        const auto ch = text[position];
        const unsigned digit = ch >= '0' && ch <= '9' ? static_cast<unsigned>(ch - '0') :
            ch >= 'a' && ch <= 'f' ? static_cast<unsigned>(ch - 'a' + 10) :
            ch >= 'A' && ch <= 'F' ? static_cast<unsigned>(ch - 'A' + 10) : 16;
        if (digit >= base || result > (std::numeric_limits<uint64_t>::max() - digit) / base)
            throw std::runtime_error(label + " is not a valid unsigned number");
        result = result * base + digit;
    }
    return result;
}
uint64_t input_number(const Json& inputs, const char* name, uint64_t fallback, bool hexadecimal = false) {
    const auto text = trim(inputs.value(name, std::string{}));
    if (text.empty()) return fallback;
    const unsigned base = hexadecimal || text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0 ? 16 : 10;
    return number(text, base, name);
}
void field(Json& result, const std::string& name, const std::string& value) {
    result["fields"].push_back({{"name", name}, {"value", value}});
}
Json response(const std::string& title) {
    return {{"title", title}, {"fields", Json::array()}, {"text", ""}, {"truncated", false}};
}
bool deleted_path(const std::string& path) {
    return path.size() >= 10 && path.compare(path.size() - 10, 10, " (deleted)") == 0;
}

struct Mapping {
    uint64_t start = 0, end = 0, offset = 0, inode = 0;
    std::string permissions, device, path, range;
};
bool parse_mapping(const std::string& line, Mapping& mapping) {
    std::istringstream fields(line);
    std::string offset, inode;
    if (!(fields >> mapping.range >> mapping.permissions >> offset >> mapping.device >> inode)) return false;
    const auto dash = mapping.range.find('-');
    if (dash == std::string::npos || mapping.device.find(':') == std::string::npos) return false;
    try {
        mapping.start = number(mapping.range.substr(0, dash), 16, "mapping start");
        mapping.end = number(mapping.range.substr(dash + 1), 16, "mapping end");
        mapping.offset = number(offset, 16, "mapping offset");
        mapping.inode = number(inode, 10, "mapping inode");
    } catch (const std::exception&) { return false; }
    std::getline(fields >> std::ws, mapping.path);
    return mapping.end > mapping.start && mapping.permissions.size() == 4;
}
std::vector<Mapping> mappings(const ProcessIdentity& identity) {
    require_identity(identity);
    const auto text = read_text(proc_path(identity.pid, "maps"), MapsLimit);
    if (text.empty()) throw std::runtime_error("Cannot read this process's memory mappings");
    if (text.size() == MapsLimit) throw std::runtime_error("Memory mapping list exceeds the inspection limit");
    std::istringstream lines(text);
    std::string line;
    std::vector<Mapping> result;
    while (std::getline(lines, line)) {
        Mapping mapping;
        if (parse_mapping(line, mapping)) result.push_back(std::move(mapping));
    }
    require_identity(identity);
    return result;
}
bool same_mapping(const Mapping& left, const Mapping& right) {
    return left.start == right.start && left.end == right.end && left.offset == right.offset &&
        left.inode == right.inode && left.device == right.device && left.permissions == right.permissions &&
        left.path == right.path;
}
Mapping selected_mapping(const ProcessIdentity& identity, const Json& row) {
    Mapping selected;
    selected.start = number(row.at("start").get<std::string>(), 16, "mapping start");
    selected.end = number(row.at("end").get<std::string>(), 16, "mapping end");
    selected.offset = number(row.at("offset").get<std::string>(), 16, "mapping offset");
    selected.inode = row.at("inode").get<uint64_t>();
    selected.permissions = row.at("permissions").get<std::string>();
    selected.device = row.at("device").get<std::string>();
    selected.path = row.at("path").get<std::string>();
    for (const auto& current : mappings(identity)) if (same_mapping(selected, current)) return current;
    throw std::runtime_error("The selected mapping changed or disappeared. Refresh the Memory tab and try again.");
}
void require_mapping(const ProcessIdentity& identity, const Mapping& mapping) {
    for (const auto& current : mappings(identity)) if (same_mapping(current, mapping)) return;
    throw std::runtime_error("The memory mapping changed during inspection. Results were discarded; refresh and try again.");
}
void mapping_fields(Json& result, const Mapping& mapping) {
    field(result, "Address range", hex(mapping.start) + " - " + hex(mapping.end));
    field(result, "Size", std::to_string(mapping.end - mapping.start) + " bytes");
    field(result, "Protection", mapping.permissions);
    field(result, "Mapped file", mapping.path.empty() ? "(anonymous)" : mapping.path);
    field(result, "File offset", hex(mapping.offset));
    field(result, "Device / inode", mapping.device + " / " + std::to_string(mapping.inode));
}
std::string vm_flag_meanings(const std::string& flags) {
    static const std::map<std::string, std::string> meanings{
        {"rd", "readable"}, {"wr", "writable"}, {"ex", "executable"}, {"sh", "shared"},
        {"mr", "may read"}, {"mw", "may write"}, {"me", "may execute"}, {"ms", "may share"},
        {"gd", "grows downward"}, {"pf", "pure PFN range"}, {"dw", "writes to mapped file denied"},
        {"lo", "locked in RAM"}, {"io", "I/O mapping"}, {"sr", "sequential read advice"},
        {"rr", "random read advice"}, {"dc", "not copied on fork"}, {"de", "cannot expand"},
        {"ac", "accountable"}, {"nr", "swap not reserved"}, {"ht", "hugetlb pages"},
        {"sf", "synchronous page fault"}, {"ar", "architecture-specific"}, {"wf", "wipe on fork"},
        {"dd", "excluded from core dumps"}, {"sd", "soft-dirty tracking"}, {"mm", "mixed mapped pages"},
        {"hg", "huge-page advice"}, {"nh", "no huge-page advice"}, {"mg", "mergeable advice"},
        {"um", "userfaultfd missing-page tracking"}, {"uw", "userfaultfd write-protect tracking"},
        {"ss", "shadow stack"}, {"lf", "lock on fault"}};
    std::istringstream words(flags);
    std::string word, result;
    while (words >> word) {
        if (!result.empty()) result += '\n';
        const auto found = meanings.find(word);
        result += word + " — " + (found == meanings.end() ? "kernel-specific flag" : found->second);
    }
    return result;
}
Json memory_properties(const ProcessIdentity& identity, const Mapping& mapping) {
    auto result = response("Memory region properties");
    mapping_fields(result, mapping);
    // Read only the selected record from smaps. The file can be much larger than
    // maps; accumulating all of it would waste memory for a single-row dialog.
    const Descriptor descriptor(open(proc_path(identity.pid, "smaps").c_str(), O_RDONLY | O_CLOEXEC));
    if (descriptor.value < 0) throw std::runtime_error("Cannot read smaps: " + std::string(std::strerror(errno)));
    std::string pending, record, line;
    bool selected = false, done = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    size_t scanned = 0;
    while (!done) {
        char bytes[16384];
        const auto count = read(descriptor.value, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::runtime_error("Cannot read smaps: " + std::string(std::strerror(errno)));
        if (count == 0) break;
        scanned += static_cast<size_t>(count);
        if (scanned > 64 * 1024 * 1024 || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Reading smaps exceeded the inspection limit");
        pending.append(bytes, static_cast<size_t>(count));
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            line = pending.substr(0, end);
            pending.erase(0, end + 1);
            Mapping current;
            if (parse_mapping(line, current)) {
                if (selected) { done = true; break; }
                selected = same_mapping(current, mapping);
            }
            if (selected) {
                if (record.size() + line.size() > 64 * 1024) throw std::runtime_error("Selected smaps record exceeds the inspection limit");
                record += line + '\n';
            }
        }
        if (pending.size() > 65536) throw std::runtime_error("Unexpected oversized smaps line");
    }
    if (!selected) throw std::runtime_error("Selected mapping no longer has a smaps record");
    std::istringstream lines(record);
    std::getline(lines, line); // The first line is the map header shown above.
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto key = line.substr(0, colon), value = trim(line.substr(colon + 1));
        field(result, key, value);
        if (key == "VmFlags") field(result, "VM flag meanings", vm_flag_meanings(value));
    }
    result["text"] = record;
    require_mapping(identity, mapping);
    return result;
}

struct MemoryBytes {
    std::vector<unsigned char> bytes;
    std::string note;
};
MemoryBytes read_memory(const ProcessIdentity& identity, const Mapping& mapping, uint64_t address, size_t length) {
    if (mapping.permissions.front() != 'r') throw std::runtime_error("This mapping is not readable");
    if (address < mapping.start || address >= mapping.end || length > mapping.end - address)
        throw std::runtime_error("The requested address range must stay inside the selected mapping");
    MemoryBytes result;
    result.bytes.resize(length);
    size_t copied = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    // process_vm_readv respects the VMA's access permissions. /proc/PID/mem can
    // bypass them with FOLL_FORCE and is deliberately not used here.
    while (copied < length) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.note = "Reading stopped at the three-second limit.";
            break;
        }
        require_identity(identity);
        const auto chunk = std::min<size_t>(65536, length - copied);
        iovec local{result.bytes.data() + copied, chunk};
        iovec remote{reinterpret_cast<void*>(static_cast<uintptr_t>(address + copied)), chunk};
        const auto count = process_vm_readv(identity.pid, &local, 1, &remote, 1, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            result.note = "Reading stopped at " + hex(address + copied) + ": " +
                (count < 0 ? std::string(std::strerror(errno)) : "no bytes returned") + ".";
            break;
        }
        copied += static_cast<size_t>(count);
        if (static_cast<size_t>(count) != chunk) {
            result.note = "Only part of the requested memory was readable; stopped at " + hex(address + copied) + ".";
            break;
        }
    }
    result.bytes.resize(copied);
    require_mapping(identity, mapping);
    if (copied == 0) throw std::runtime_error(result.note);
    return result;
}
Json memory_read(const ProcessIdentity& identity, const Mapping& mapping, const Json& inputs) {
    const auto address = input_number(inputs, "address", mapping.start, true);
    const auto requested = input_number(inputs, "length", std::min<uint64_t>(4096, mapping.end - mapping.start));
    if (!requested || requested > MemoryReadLimit) throw std::runtime_error("Length must be between 1 and 1048576 bytes");
    const auto memory = read_memory(identity, mapping, address, static_cast<size_t>(requested));
    auto result = response("Memory contents");
    mapping_fields(result, mapping);
    field(result, "Read address", hex(address));
    field(result, "Bytes read", std::to_string(memory.bytes.size()) + " / " + std::to_string(requested));
    field(result, "Consistency", "Live memory; the process was not suspended and contents may change while reading.");
    if (!memory.note.empty()) field(result, "Read result", memory.note);
    constexpr char digits[] = "0123456789abcdef";
    std::string data_hex;
    data_hex.reserve(memory.bytes.size() * 2);
    std::ostringstream text;
    for (size_t offset = 0; offset < memory.bytes.size(); offset += 16) {
        text << std::hex << std::setfill('0') << std::setw(16) << address + offset << "  ";
        for (size_t column = 0; column < 16; ++column) {
            if (offset + column < memory.bytes.size()) {
                const auto byte = memory.bytes[offset + column];
                text << digits[byte >> 4] << digits[byte & 15] << ' ';
                data_hex.push_back(digits[byte >> 4]); data_hex.push_back(digits[byte & 15]);
            } else text << "   ";
        }
        text << " |";
        for (size_t column = 0; column < 16 && offset + column < memory.bytes.size(); ++column) {
            const auto byte = memory.bytes[offset + column];
            text << (byte >= 32 && byte <= 126 ? static_cast<char>(byte) : '.');
        }
        text << "|\n";
    }
    if (!memory.note.empty()) text << '\n' << memory.note << '\n';
    result["text"] = text.str();
    result["data_hex"] = std::move(data_hex);
    result["suggested_filename"] = "memory-" + std::to_string(identity.pid) + "-" + hex(address) + ".bin";
    result["truncated"] = memory.bytes.size() < requested;
    return result;
}
Json memory_strings(const ProcessIdentity& identity, const Mapping& mapping, const Json& inputs) {
    const auto minimum = input_number(inputs, "minimum_length", 4);
    const auto limit = input_number(inputs, "maximum_bytes", MemoryReadLimit);
    if (minimum < 2 || minimum > 4096) throw std::runtime_error("Minimum string length must be between 2 and 4096 characters");
    if (!limit || limit > StringScanLimit) throw std::runtime_error("Maximum scan size must be between 1 and 16777216 bytes");
    const auto requested = std::min<uint64_t>(mapping.end - mapping.start, limit);
    const auto memory = read_memory(identity, mapping, mapping.start, static_cast<size_t>(requested));
    auto result = response("Memory strings");
    mapping_fields(result, mapping);
    field(result, "Bytes scanned", std::to_string(memory.bytes.size()) + " / " + std::to_string(mapping.end - mapping.start));
    field(result, "Minimum length", std::to_string(minimum));
    field(result, "Encodings", "Printable ASCII and UTF-16LE (ASCII character range), at either byte alignment");
    field(result, "Consistency", "Live memory; the process was not suspended.");
    std::string text = "Address             Encoding  String\n";
    size_t matches = 0;
    bool output_full = false;
    auto append_string = [&](size_t begin, size_t count, size_t stride) {
        if (count < minimum || output_full) return;
        // A single unterminated megabyte string must not monopolize the result.
        const auto displayed = std::min<size_t>(count, 4096);
        std::string line = hex(mapping.start + begin);
        line.append(20 - std::min<size_t>(20, line.size()), ' ');
        line += stride == 1 ? "ASCII     " : "UTF-16LE  ";
        for (size_t i = 0; i < displayed; ++i) line.push_back(static_cast<char>(memory.bytes[begin + i * stride]));
        if (displayed != count) line += " ... [" + std::to_string(count) + " characters]";
        line += '\n';
        if (text.size() + line.size() > TextLimit - 256) { output_full = true; return; }
        text += line;
        ++matches;
    };
    // Separate passes keep a long run's encoding unambiguous. Both UTF-16 byte
    // alignments are inspected: packed strings need not start at an even address.
    for (size_t stride : {size_t{1}, size_t{2}}) {
        for (size_t alignment = 0; alignment < stride && !output_full; ++alignment) {
            size_t begin = alignment, count = 0;
            for (size_t i = alignment; i + stride <= memory.bytes.size() && !output_full; i += stride) {
                const auto byte = memory.bytes[i];
                if (byte >= 32 && byte <= 126 && (stride == 1 || memory.bytes[i + 1] == 0)) {
                    if (count == 0) begin = i;
                    ++count;
                } else { append_string(begin, count, stride); count = 0; }
            }
            append_string(begin, count, stride);
        }
    }
    const bool partial = memory.bytes.size() < mapping.end - mapping.start;
    field(result, "Strings found", std::to_string(matches) + (output_full ? " (output limit reached)" : ""));
    if (partial) text += "\nOnly the first " + std::to_string(memory.bytes.size()) + " bytes of this mapping were scanned.\n";
    if (output_full) text += "\nString output stopped at the 2 MB limit.\n";
    if (!memory.note.empty()) text += '\n' + memory.note + '\n';
    result["text"] = std::move(text);
    result["truncated"] = partial || output_full;
    return result;
}

std::string file_type(mode_t mode) {
    if (S_ISREG(mode)) return "Regular file";
    if (S_ISDIR(mode)) return "Directory";
    if (S_ISSOCK(mode)) return "Socket";
    if (S_ISFIFO(mode)) return "Pipe / FIFO";
    if (S_ISCHR(mode)) return "Character device";
    if (S_ISBLK(mode)) return "Block device";
    if (S_ISLNK(mode)) return "Symbolic link";
    return "Other";
}
std::string flag_names(uint64_t flags) {
    std::string text;
    switch (flags & O_ACCMODE) {
    case O_RDONLY: text = "O_RDONLY"; break;
    case O_WRONLY: text = "O_WRONLY"; break;
    case O_RDWR: text = "O_RDWR"; break;
    default: text = "Unknown access mode"; break;
    }
    for (const auto& entry : std::vector<std::pair<uint64_t, const char*>>{
        {O_APPEND, "O_APPEND"}, {O_ASYNC, "O_ASYNC"}, {O_CLOEXEC, "O_CLOEXEC"},
        {O_DIRECT, "O_DIRECT"}, {O_DIRECTORY, "O_DIRECTORY"}, {O_DSYNC, "O_DSYNC"},
        {O_NOATIME, "O_NOATIME"}, {O_NOFOLLOW, "O_NOFOLLOW"}, {O_NONBLOCK, "O_NONBLOCK"},
        {O_PATH, "O_PATH"}, {O_SYNC, "O_SYNC"}})
        if ((flags & entry.first) == entry.first) text += std::string(" | ") + entry.second;
    return text + " (" + hex(flags) + ")";
}
bool same_inode(const struct stat& before, const struct stat& after) {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_mode == after.st_mode &&
        before.st_rdev == after.st_rdev;
}
uint64_t descriptor_number(const std::string& info, const char* name) {
    std::istringstream lines(info);
    std::string line;
    const std::string prefix = std::string(name) + ":";
    while (std::getline(lines, line)) {
        if (line.rfind(prefix, 0) == 0) return number(trim(line.substr(prefix.size())), 10, name);
    }
    return 0;
}
Json handle_properties(const ProcessIdentity& identity, const Json& row) {
    const auto fd = row.at("fd").get<int>();
    if (fd < 0) throw std::runtime_error("Invalid file descriptor");
    const auto path = proc_path(identity.pid, "fd/" + std::to_string(fd));
    const auto target = read_link(path);
    if (target.empty() || target != row.at("target").get<std::string>())
        throw std::runtime_error("This file descriptor changed or disappeared. Refresh the Handles tab and try again.");
    // O_PATH pins metadata without opening a device, socket or FIFO for I/O.
    // Some anonymous descriptors cannot be reopened even with O_PATH; stat on
    // the proc symlink is still metadata-only and is verified again afterward.
    const Descriptor pinned(open(path.c_str(), O_PATH | O_CLOEXEC));
    struct stat before{}, after{};
    if ((pinned.value >= 0 ? fstat(pinned.value, &before) : stat(path.c_str(), &before)) != 0)
        throw std::runtime_error("Cannot inspect this descriptor: " + std::string(std::strerror(errno)));
    const auto info = read_text(proc_path(identity.pid, "fdinfo/" + std::to_string(fd)), 256 * 1024);
    if (info.empty()) throw std::runtime_error("Cannot read file descriptor information");
    const auto requested_inode = row.value("inode", uint64_t{0});
    const auto requested_mount = row.value("mount_id", uint64_t{0});
    const auto current_inode = descriptor_number(info, "ino");
    const auto current_mount = descriptor_number(info, "mnt_id");
    // A name alone cannot distinguish a replacement opened at the same path.
    // New rows carry fdinfo identity; zero retains compatibility with old rows
    // whose kernel or snapshot did not expose those fields.
    if ((requested_inode && (requested_inode != current_inode || requested_inode != before.st_ino)) ||
        (requested_mount && requested_mount != current_mount) ||
        (current_inode && current_inode != before.st_ino))
        throw std::runtime_error("This file descriptor was replaced. Refresh the Handles tab and try again.");
    auto result = response("File descriptor properties");
    field(result, "File descriptor", std::to_string(fd));
    field(result, "Target", target);
    field(result, "Type", file_type(before.st_mode));
    std::ostringstream mode;
    mode << '0' << std::oct << (before.st_mode & 07777);
    field(result, "Permissions (octal)", mode.str());
    field(result, "Owner UID / GID", std::to_string(before.st_uid) + " / " + std::to_string(before.st_gid));
    field(result, "Size", std::to_string(before.st_size) + " bytes");
    field(result, "Device / inode", hex(major(before.st_dev)) + ":" + hex(minor(before.st_dev)) + " / " + std::to_string(before.st_ino));
    field(result, "Link count", std::to_string(before.st_nlink));
    field(result, "Deleted", deleted_path(target) ? "Yes" : "No");
    std::istringstream lines(info);
    std::string line;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto key = line.substr(0, colon), value = trim(line.substr(colon + 1));
        if (key == "pos") field(result, "Current offset", value + " bytes");
        else if (key == "flags") {
            const auto flags = number(value, 8, "descriptor flags");
            field(result, "Flags", flag_names(flags));
            field(result, "Close on exec", flags & O_CLOEXEC ? "Yes" : "No (inherited across exec)");
        } else if (key == "mnt_id") field(result, "Mount ID", value);
        else if (key == "lock") field(result, "Lock", value);
    }
    if (stat(path.c_str(), &after) != 0 || !same_inode(before, after) || read_link(path) != target)
        throw std::runtime_error("This file descriptor changed during inspection. Refresh and try again.");
    const auto final_info = read_text(proc_path(identity.pid, "fdinfo/" + std::to_string(fd)), 256 * 1024);
    if (final_info.empty() || descriptor_number(final_info, "ino") != current_inode ||
        descriptor_number(final_info, "mnt_id") != current_mount)
        throw std::runtime_error("This file descriptor changed during inspection. Refresh and try again.");
    require_identity(identity);
    result["text"] = info;
    result["truncated"] = info.size() == 256 * 1024;
    return result;
}

std::vector<Mapping> selected_module(const ProcessIdentity& identity, const Json& row) {
    const auto device = row.at("device").get<std::string>();
    const auto inode = row.at("inode").get<uint64_t>();
    const auto base = number(row.at("base").get<std::string>(), 16, "module base");
    const auto end = number(row.at("end").get<std::string>(), 16, "module end");
    std::vector<Mapping> result;
    uint64_t current_base = std::numeric_limits<uint64_t>::max(), current_end = 0;
    bool path_matches = false;
    for (const auto& mapping : mappings(identity)) {
        if (mapping.inode != inode || mapping.device != device) continue;
        result.push_back(mapping);
        current_base = std::min(current_base, mapping.start);
        current_end = std::max(current_end, mapping.end);
        path_matches |= mapping.path == row.at("path").get<std::string>();
    }
    if (!inode || result.empty() || !path_matches || current_base != base || current_end != end)
        throw std::runtime_error("This module changed or disappeared. Refresh the Modules tab and try again.");
    return result;
}
bool file_matches(const struct stat& info, const Mapping& mapping) {
    const auto colon = mapping.device.find(':');
    return S_ISREG(info.st_mode) && static_cast<uint64_t>(info.st_ino) == mapping.inode &&
        major(info.st_dev) == number(mapping.device.substr(0, colon), 16, "device major") &&
        minor(info.st_dev) == number(mapping.device.substr(colon + 1), 16, "device minor");
}
// readelf's wide dynamic-symbol rows have seven fixed columns followed by the
// name. Keep the original row (including version and demangling) after checking
// definition, binding and visibility; section headings are never symbols.
std::string filtered_symbols(const std::string& output, bool imports) {
    std::string result = "   Num:    Value          Size Type    Bind   Vis      Ndx Name\n";
    std::istringstream lines(output);
    std::string line;
    size_t count = 0;
    while (std::getline(lines, line)) {
        std::istringstream columns(line);
        std::string index, value, size, type, binding, visibility, section, name;
        if (!(columns >> index >> value >> size >> type >> binding >> visibility >> section)) continue;
        if (index.size() < 2 || index.back() != ':' ||
            index.substr(0, index.size() - 1).find_first_not_of("0123456789") != std::string::npos) continue;
        std::getline(columns >> std::ws, name);
        if (name.empty()) continue;
        const bool external = binding == "GLOBAL" || binding == "WEAK" || binding == "UNIQUE";
        if (!external || (imports ? section != "UND" :
            section == "UND" || (visibility != "DEFAULT" && visibility != "PROTECTED"))) continue;
        result += line + '\n';
        ++count;
    }
    if (!count) result += imports ? "(No imported dynamic symbols found.)\n" : "(No exported dynamic symbols found.)\n";
    return result;
}
Json module_inspect(const ProcessIdentity& identity, const Json& row, const std::string& action, const Json& inputs) {
    const auto before = selected_module(identity, row);
    const Mapping* representative = &before.front();
    for (const auto& mapping : before) if (mapping.offset == 0) representative = &mapping;
    int raw = open(proc_path(identity.pid, "map_files/" + representative->range).c_str(), O_PATH | O_CLOEXEC);
    if (raw < 0 && !deleted_path(representative->path) && !representative->path.empty() && representative->path.front() == '/')
        raw = open((proc_path(identity.pid, "root") + representative->path).c_str(), O_PATH | O_CLOEXEC);
    const Descriptor pinned(raw);
    struct stat file_before{}, file_after{};
    if (pinned.value < 0 || fstat(pinned.value, &file_before) != 0 || !file_matches(file_before, *representative))
        throw std::runtime_error("Cannot open the selected mapped file with matching device and inode. It may have been deleted or access may be restricted.");
    if (find_command("readelf").empty())
        throw std::runtime_error("ELF inspection requires readelf. Install binutils in this distribution (for Ubuntu: apt install binutils).");

    // The child opens our still-pinned descriptor through procfs. It never
    // resolves the target's original filename again or executes the image.
    const auto path = proc_path(getpid(), "fd/" + std::to_string(pinned.value));
    const auto symbols = inputs.value("symbols", std::string("all"));
    if (action == "symbols" && symbols != "all" && symbols != "exports" && symbols != "imports")
        throw std::runtime_error("Symbol selection must be exports, imports or all");
    std::vector<std::string> command{"readelf", "--wide"};
    if (action == "properties") {
        command.insert(command.end(), {"--file-header", "--program-headers", "--section-headers", "--notes", "--dynamic"});
    } else if (action == "symbols") command.insert(command.end(), {symbols == "all" ? "--symbols" : "--dyn-syms", "--demangle"});
    else if (action == "dependencies") command.push_back("--dynamic");
    else throw std::runtime_error("Unsupported module inspection action");
    command.push_back("--"); command.push_back(path);
    auto output = run_command(command, 5000, TextLimit);
    if (fstat(pinned.value, &file_after) != 0 || !same_inode(file_before, file_after) ||
        file_before.st_size != file_after.st_size || file_before.st_mtim.tv_sec != file_after.st_mtim.tv_sec ||
        file_before.st_mtim.tv_nsec != file_after.st_mtim.tv_nsec || file_before.st_ctim.tv_sec != file_after.st_ctim.tv_sec ||
        file_before.st_ctim.tv_nsec != file_after.st_ctim.tv_nsec)
        throw std::runtime_error("The mapped file changed during inspection. Results were discarded; try again.");
    const auto after = selected_module(identity, row);
    if (before.size() != after.size() || !std::equal(before.begin(), before.end(), after.begin(), same_mapping))
        throw std::runtime_error("The module mappings changed during inspection. Refresh and try again.");
    require_identity(identity);
    if (output.output.empty()) throw std::runtime_error(output.timed_out ? "readelf timed out after five seconds" : "readelf did not return any ELF information");
    auto result = response(action == "properties" ? "ELF module properties" : action == "symbols" ? "ELF symbols" : "ELF dependencies");
    field(result, "Module", row.at("path").get<std::string>());
    field(result, "Base address", row.at("base").get<std::string>());
    field(result, "Device / inode", representative->device + " / " + std::to_string(representative->inode));
    field(result, "File size", std::to_string(file_before.st_size) + " bytes");
    field(result, "Inspection", "readelf parses the mapped file; no code in the target image is executed.");
    if (action == "dependencies") field(result, "Dependency resolution", "DT_NEEDED names and embedded search paths only. Resolved library paths are available in Modules; ldd is not executed.");
    std::istringstream lines(output.output);
    std::string line;
    bool header = false;
    while (std::getline(lines, line)) {
        if (line == "ELF Header:") { header = true; continue; }
        if (header && trim(line).empty()) header = false;
        if (header) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) field(result, trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
        }
        for (const auto* tag : {"(NEEDED)", "(SONAME)", "(RPATH)", "(RUNPATH)"}) {
            const auto position = line.find(tag);
            if (position != std::string::npos) field(result, std::string(tag).substr(1, std::strlen(tag) - 2), trim(line.substr(position + std::strlen(tag))));
        }
        const auto build_id = line.find("Build ID:");
        if (build_id != std::string::npos) field(result, "Build ID", trim(line.substr(build_id + 9)));
    }
    const bool output_limit_reached = output.output.size() == TextLimit;
    const bool truncated = output.timed_out || output_limit_reached;
    if (action == "symbols" && symbols != "all") {
        field(result, "Symbol selection", symbols == "imports" ? "Imported dynamic symbols (undefined)" : "Exported dynamic symbols (defined, externally visible)");
        // Preserve diagnostics when readelf fails rather than converting errors
        // into a misleading successful empty symbol table.
        if (output.exit_code == 0 && !output.timed_out) output.output = filtered_symbols(output.output, symbols == "imports");
    }
    if (output.timed_out) output.output += "\nreadelf timed out after five seconds; the output above is incomplete.\n";
    else if (output_limit_reached) output.output += "\nreadelf output stopped at the 2 MB limit.\n";
    if (output.exit_code != 0) field(result, "readelf result", "Exit code " + std::to_string(output.exit_code) + "; see diagnostic output.");
    result["text"] = std::move(output.output);
    result["truncated"] = truncated;
    return result;
}
} // namespace

Json resource_tool(const Json& request) {
    const auto identity = request_identity(request);
    require_identity(identity);
    const auto kind = request.at("kind").get<std::string>();
    const auto action = request.at("action").get<std::string>();
    const auto& row = request.at("row");
    const auto inputs = request.value("inputs", Json::object());
    if (!inputs.is_object()) throw std::runtime_error("Resource inputs must be an object");
    Json result;
    if (kind == "memory") {
        const auto mapping = selected_mapping(identity, row);
        if (action == "properties") result = memory_properties(identity, mapping);
        else if (action == "read") result = memory_read(identity, mapping, inputs);
        else if (action == "strings") result = memory_strings(identity, mapping, inputs);
        else throw std::runtime_error("Unsupported memory inspection action");
    } else if (kind == "handle" && action == "properties") result = handle_properties(identity, row);
    else if (kind == "module") result = module_inspect(identity, row, action, inputs);
    else throw std::runtime_error("Unsupported resource inspection action");
    require_identity(identity);
    return result;
}
} // namespace observer
