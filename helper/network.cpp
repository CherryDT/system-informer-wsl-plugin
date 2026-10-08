#include "observer.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <dirent.h>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace observer {
namespace {
struct Owner { int pid; uint64_t start_ticks; std::string name; };

std::string reverse_lookup_address(const std::string& address) {
    in_addr ipv4{};
    in6_addr ipv6{};
    int family = AF_INET;
    const void* bytes = &ipv4;
    if (inet_pton(AF_INET, address.c_str(), &ipv4) != 1) {
        if (inet_pton(AF_INET6, address.c_str(), &ipv6) != 1 ||
            IN6_IS_ADDR_UNSPECIFIED(&ipv6) || IN6_IS_ADDR_MULTICAST(&ipv6)) return {};
        if (IN6_IS_ADDR_V4MAPPED(&ipv6)) {
            // Some NSS providers return IPv4 for a mapped IPv6 query. Use one
            // canonical key so those results and duplicate sockets share it.
            std::memcpy(&ipv4, ipv6.s6_addr + 12, sizeof(ipv4));
        } else {
            family = AF_INET6;
            bytes = &ipv6;
        }
    }
    if (family == AF_INET) {
        const auto host_address = ntohl(ipv4.s_addr);
        if (host_address == 0 || (host_address >> 24) >= 224) return {};
    }
    char normalized[INET6_ADDRSTRLEN]{};
    return inet_ntop(family, bytes, normalized, sizeof(normalized)) ? normalized : std::string{};
}

std::string remote_hostname(const std::string& address, bool& lookup_used) {
    const auto key = reverse_lookup_address(address);
    if (key.empty()) return {};
    using Clock = std::chrono::steady_clock;
    struct Entry { std::string hostname; Clock::time_point expires; };
    // The observer dispatches requests serially. Positive and negative entries
    // both expire, and the limit also covers hosts seen only briefly.
    static std::map<std::string, Entry> cache;
    const auto now = Clock::now();
    const auto cached = cache.find(key);
    if (cached != cache.end() && cached->second.expires > now) return cached->second.hostname;
    if (lookup_used) return {};
    lookup_used = true;

    std::string hostname;
    try {
        // Use the distro's own NSS/DNS configuration without loading its NSS
        // modules into our static helper. One short-lived command per request
        // keeps DNS failures from holding up monitoring or shutdown.
        const auto result = run_command({"getent", "hosts", key}, 500, 4096);
        if (!result.timed_out && result.exit_code == 0) {
            std::istringstream lines(result.output);
            std::string line;
            while (std::getline(lines, line)) {
                std::istringstream fields(line);
                std::string returned_address, name;
                if (!(fields >> returned_address >> name) || reverse_lookup_address(returned_address) != key)
                    continue;
                // A hostname is display data, never an executable argument.
                // Reject diagnostics, numeric fallbacks, and control characters.
                in_addr name_ipv4{};
                in6_addr name_ipv6{};
                if (name.size() <= 253 && name.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == std::string::npos &&
                    inet_pton(AF_INET, name.c_str(), &name_ipv4) != 1 &&
                    inet_pton(AF_INET6, name.c_str(), &name_ipv6) != 1) {
                    hostname = std::move(name);
                    break;
                }
            }
        }
    } catch (const std::exception&) {
        // Missing getent and resolver failures leave the numeric endpoint
        // usable. Cache the failure instead of retrying it on every tick.
    }
    if (cache.size() >= 1024 && cached == cache.end()) {
        for (auto entry = cache.begin(); entry != cache.end();) {
            if (entry->second.expires <= now) entry = cache.erase(entry);
            else ++entry;
        }
        if (cache.size() >= 1024) {
            const auto oldest = std::min_element(cache.begin(), cache.end(),
                [](const auto& left, const auto& right) { return left.second.expires < right.second.expires; });
            cache.erase(oldest);
        }
    }
    cache[key] = {hostname, Clock::now() + std::chrono::seconds(hostname.empty() ? 60 : 300)};
    return hostname;
}

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
    const bool resolve_names = !identities_only && request.value("resolve_names", false);
    bool lookup_used = false;
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
        } else if (resolve_names) {
            item["remote_hostname"] = remote_hostname(item.value("remote_address", ""), lookup_used);
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
