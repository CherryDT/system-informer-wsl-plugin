#include "observer.hpp"

#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace observer
{
namespace
{
std::string folded(std::string value)
{
    // Linux names are byte strings. Fold ASCII without invoking the process
    // locale or damaging UTF-8 sequences in a filename.
    for (char &character : value)
        if (character >= 'A' && character <= 'Z')
            character += 'a' - 'A';
    return value;
}
std::string descriptor_type(const std::string &target)
{
    if (target.rfind("socket:[", 0) == 0)
        return "Socket";
    if (target.rfind("pipe:[", 0) == 0)
        return "Pipe";
    if (target.rfind("anon_inode:", 0) == 0)
        return "Anonymous inode";
    return "File";
}
} // namespace

Json find_handles(const Json &request)
{
    const bool enumerate = request.value("enumerate", false);
    const bool case_sensitive = request.value("case_sensitive", false);
    const std::string query = case_sensitive ? request.value("query", std::string{})
                                            : folded(request.value("query", std::string{}));
    if (query.empty() || query.size() > 1024)
        throw std::runtime_error("Enter a search string between 1 and 1024 UTF-8 bytes.");

    // A search never opens the target file or enters a mount. Only procfs
    // metadata is read, so a FIFO, socket or unavailable filesystem cannot
    // turn a search into an open() on an arbitrary application resource.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Json rows = Json::array();
    size_t bytes_left = 8 * 1024 * 1024;
    unsigned scanned = 0, inaccessible = 0;
    bool truncated = false;
    auto exhausted = [&] {
        return truncated || rows.size() >= 10000 || std::chrono::steady_clock::now() >= deadline;
    };
    for (int pid : process_ids())
    {
        if (exhausted())
        {
            truncated = true;
            break;
        }
        ProcessStat before;
        try
        {
            before = process_stat(pid);
        }
        catch (...)
        {
            continue;
        }
        ++scanned;
        const std::string base = "/proc/" + std::to_string(pid);
        Json process_rows = Json::array();
        auto consider = [&](const std::string &handle, const std::string &type, const std::string &path) {
            if (exhausted() || process_rows.size() + rows.size() >= 10000)
            {
                truncated = true;
                return;
            }
            if (path.empty())
                return;
            // The Windows search control supplies the final match semantics,
            // including PCRE and Unicode case folding. Only discard candidates
            // here when a literal byte comparison is known to be safe. A
            // process name or PID must never cause all its handles to match.
            const bool non_ascii = std::any_of(path.begin(), path.end(), [](unsigned char c) {
                return c >= 0x80;
            });
            if (!enumerate && (case_sensitive || !non_ascii) &&
                (case_sensitive ? path : folded(path)).find(query) == std::string::npos)
                return;
            Json item = {{"pid", pid},
                         {"start_ticks", before.start_ticks},
                         {"process", before.name},
                         {"handle", handle},
                         {"type", type},
                         {"path", path}};
            if (!append_with_budget(process_rows, std::move(item), bytes_left))
                truncated = true;
        };
        const auto close_directory = [](DIR *directory) { closedir(directory); };
        using Directory = std::unique_ptr<DIR, decltype(close_directory)>;
        Directory descriptors(opendir((base + "/fd").c_str()), close_directory);
        if (!descriptors)
            ++inaccessible;
        else
            while (const auto *entry = readdir(descriptors.get()))
            {
                if (exhausted() || process_rows.size() + rows.size() >= 10000)
                {
                    truncated = true;
                    break;
                }
                if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
                    continue;
                const auto target = read_link(base + "/fd/" + entry->d_name);
                consider(entry->d_name, descriptor_type(target), target);
            }
        if (!exhausted())
        {
            consider("cwd", "Working directory", read_link(base + "/cwd"));
            consider("exe", "Executable", read_link(base + "/exe"));
            consider("root", "Root directory", read_link(base + "/root"));
            const auto maps = read_text(base + "/maps", 4 * 1024 * 1024);
            if (maps.size() == 4 * 1024 * 1024)
                truncated = true;
            std::istringstream lines(maps);
            std::set<std::string> seen;
            std::string line;
            while (std::getline(lines, line))
            {
                if (exhausted() || process_rows.size() + rows.size() >= 10000)
                {
                    truncated = true;
                    break;
                }
                std::istringstream fields(line);
                std::string range, permissions, offset, device, inode, path;
                if (!(fields >> range >> permissions >> offset >> device >> inode))
                    continue;
                std::getline(fields >> std::ws, path);
                if (path.empty() || path[0] != '/' || inode == "0")
                    continue;
                const bool executable = permissions.find('x') != std::string::npos;
                const std::string type = executable ? "Executable mapping" : "Mapped file";
                if (!seen.insert(type + ':' + device + ':' + inode + ':' + path).second)
                    continue;
                consider(range.substr(0, range.find('-')), type, path);
            }
        }
        // Keep names, descriptors and process identity from the same PID
        // occupant. Details and subsequent actions recheck this identity too.
        try
        {
            if (process_stat(pid).start_ticks != before.start_ticks)
                continue;
        }
        catch (...)
        {
            continue;
        }
        for (auto &row : process_rows)
            rows.push_back(std::move(row));
    }
    return {{"results", std::move(rows)},
            {"processes_scanned", scanned},
            {"inaccessible_processes", inaccessible},
            {"truncated", truncated}};
}
} // namespace observer
