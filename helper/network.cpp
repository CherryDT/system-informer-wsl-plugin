#include "observer.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace observer {
namespace {
struct Owner { int pid; uint64_t start_ticks; std::string name; };
std::string decode_address(const std::string& value, bool ipv6) {
    unsigned char bytes[16]{};
    const size_t expected = ipv6 ? 32 : 8;
    if (value.size() != expected) return value;
    try {
        // /proc prints each native-endian 32-bit word in hex, including IPv6.
        // Copying words preserves the kernel's byte ordering on either endian.
        for (size_t index = 0; index < expected / 8; ++index) {
            const auto word = static_cast<uint32_t>(std::stoul(value.substr(index * 8, 8), nullptr, 16));
            std::memcpy(bytes + index * 4, &word, sizeof(word));
        }
    } catch (...) { return value; }
    char output[INET6_ADDRSTRLEN]{};
    return inet_ntop(ipv6 ? AF_INET6 : AF_INET, bytes, output, sizeof(output)) ? output : value;
}
std::string socket_state(unsigned state, bool udp) {
    if (udp && state == 7) return "UNCONN";
    switch (state) {
    case 1: return "ESTABLISHED";
    case 2: return "SYN_SENT";
    case 3: return "SYN_RECV";
    case 4: return "FIN_WAIT1";
    case 5: return "FIN_WAIT2";
    case 6: return "TIME_WAIT";
    case 7: return "CLOSE";
    case 8: return "CLOSE_WAIT";
    case 9: return "LAST_ACK";
    case 10: return "LISTEN";
    case 11: return "CLOSING";
    case 12: return "NEW_SYN_RECV";
    default: return "UNKNOWN";
    }
}
}
Json connections(const Json& request) {
    const bool filtered = request.contains("pid");
    const bool identities_only = request.value("identities_only", false);
    ProcessIdentity identity{0, 0};
    if (filtered) { identity = request_identity(request); require_identity(identity); }
    std::map<uint64_t, std::vector<Owner>> owners;
    int inaccessible = 0;
    for (const int pid : process_ids()) {
        if (filtered && pid != identity.pid) continue;
        const std::string path = "/proc/" + std::to_string(pid) + "/fd";
        DIR* directory = opendir(path.c_str());
        if (!directory) { ++inaccessible; continue; }
        ProcessStat stat;
        try { stat = process_stat(pid); } catch (...) { closedir(directory); continue; }
        std::set<uint64_t> seen;
        while (const auto* entry = readdir(directory)) {
            const auto target = read_link(path + "/" + entry->d_name);
            if (target.rfind("socket:[", 0) != 0 || target.back() != ']') continue;
            try {
                const auto inode = std::stoull(target.substr(8, target.size() - 9));
                seen.insert(inode);
            } catch (...) { }
        }
        closedir(directory);
        try {
            // fd paths can change while we scan. Do not attach a previous PID
            // occupant's sockets to a newly started process with the same PID.
            if (process_stat(pid).start_ticks == stat.start_ticks)
                for (const auto inode : seen) owners[inode].push_back({pid, stat.start_ticks, stat.name});
        } catch (const std::exception&) { }
    }
    Json result = Json::array();
    size_t response_budget = 12 * 1024 * 1024;
    bool truncated = false;
    auto append = [&](Json item) {
        if (identities_only) {
            item.erase("process");
            item.erase("state");
        }
        if (!append_with_budget(result, std::move(item), response_budget)) truncated = true;
    };
    unsigned tables_read = 0;
    for (const std::string protocol : {"tcp", "tcp6", "udp", "udp6"}) {
        const bool ipv6 = protocol.back() == '6';
        const bool udp = protocol.rfind("udp", 0) == 0;
        // /proc/net always refers to this helper's network namespace. We do not
        // silently enter container namespaces or imply that this is VM-wide.
        const auto table = read_text("/proc/net/" + protocol, 32 * 1024 * 1024);
        if (!table.empty()) ++tables_read;
        std::istringstream lines(table);
        std::string line;
        std::getline(lines, line);
        while (!truncated && std::getline(lines, line)) {
            std::istringstream input(line);
            std::vector<std::string> fields;
            std::string field;
            while (input >> field) fields.push_back(field);
            if (fields.size() < 10) continue;
            try {
                const auto local_separator = fields[1].find(':');
                const auto remote_separator = fields[2].find(':');
                if (local_separator == std::string::npos || remote_separator == std::string::npos) continue;
                const auto inode = std::stoull(fields[9]);
                Json item = {{"protocol", protocol},
                    {"local_address", decode_address(fields[1].substr(0, local_separator), ipv6)},
                    {"local_port", std::stoul(fields[1].substr(local_separator + 1), nullptr, 16)},
                    {"remote_address", decode_address(fields[2].substr(0, remote_separator), ipv6)},
                    {"remote_port", std::stoul(fields[2].substr(remote_separator + 1), nullptr, 16)},
                    {"state", socket_state(std::stoul(fields[3], nullptr, 16), udp)}, {"inode", inode}};
                const auto found = owners.find(inode);
                if (found == owners.end()) {
                    if (!filtered) {
                        item["pid"] = 0; item["start_ticks"] = 0; item["process"] = "";
                        append(std::move(item));
                    }
                } else {
                    for (const auto& owner : found->second) {
                        item["pid"] = owner.pid; item["start_ticks"] = owner.start_ticks; item["process"] = owner.name;
                        append(item);
                    }
                }
            } catch (...) { /* A malformed or concurrently changing row is skipped. */ }
        }
    }
    const auto unix_table = read_text("/proc/net/unix", 32 * 1024 * 1024);
    if (!unix_table.empty()) ++tables_read;
    std::istringstream unix_lines(unix_table);
    std::string unix_line;
    std::getline(unix_lines, unix_line);
    while (!truncated && std::getline(unix_lines, unix_line)) {
        std::istringstream input(unix_line);
        std::string number, refs, protocol, flags, type, state, inode_text, path;
        if (!(input >> number >> refs >> protocol >> flags >> type >> state >> inode_text)) continue;
        std::getline(input >> std::ws, path);
        try {
            const auto inode = std::stoull(inode_text);
            const auto flag_value = std::stoul(flags, nullptr, 16);
            const auto state_value = std::stoul(state, nullptr, 16);
            Json item = {{"protocol", "unix"}, {"local_address", path}, {"local_port", 0},
                {"remote_address", ""}, {"remote_port", 0}, {"inode", inode},
                {"state", (flag_value & 0x10000) ? "LISTEN" : state_value == 3 ? "CONNECTED" : "UNCONNECTED"}};
            const auto found = owners.find(inode);
            if (found == owners.end()) {
                if (!filtered) {
                    item["pid"] = 0; item["start_ticks"] = 0; item["process"] = "";
                    append(std::move(item));
                }
            } else {
                for (const auto& owner : found->second) {
                    item["pid"] = owner.pid; item["start_ticks"] = owner.start_ticks; item["process"] = owner.name;
                    append(item);
                }
            }
        } catch (...) { }
    }
    if (filtered) require_identity(identity);
    return {{"identities_only", identities_only}, {"connections", result}, {"connections_truncated", truncated}, {"inaccessible_processes", inaccessible},
            {"tables_read", tables_read}, {"network_namespace", read_link("/proc/self/ns/net")},
            {"coverage", "Current network namespace only. PID 0 means no visible owner (including TIME_WAIT sockets)."}};
}
} // namespace observer
