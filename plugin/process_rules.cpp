#include "process_rules.hpp"
#include "settings.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace wsl
{
namespace
{
constexpr wchar_t ValueName[] = L"SavedProcessScheduling";
constexpr DWORD MaximumBytes = 1024 * 1024;
constexpr size_t MaximumRules = 128;
// Normally only live matching processes occupy this map. The ceiling also
// bounds retention if every subsequent snapshot is truncated.
constexpr size_t MaximumIdentities = 65536;
using RuleKey = std::pair<std::wstring, std::string>;
using Rules = std::map<RuleKey, Json>;
using Identity = std::pair<uint64_t, uint64_t>;

struct Attempt
{
    bool pending = true;
    uint64_t token = 0;
};
struct Process
{
    std::string exe;
    uint64_t threads = 0;
    std::map<std::string, Attempt> attempts;
};
struct Distro
{
    std::string boot;
    std::map<Identity, Process> processes;
    std::wstring error;
};
std::mutex mutex;
bool loaded = false;
Rules rules;
uint64_t revision = 1;
uint64_t nextToken = 1;
std::wstring loadError;
std::map<std::wstring, Distro> distros;

bool validExecutable(const std::string &exe)
{
    constexpr char deleted[] = " (deleted)";
    constexpr size_t suffixLength = sizeof(deleted) - 1;
    return exe.size() > 1 && exe.size() <= 32768 && exe.front() == '/' &&
           exe.find('\0') == std::string::npos &&
           !(exe.size() >= suffixLength &&
             exe.compare(exe.size() - suffixLength, suffixLength, deleted) == 0);
}

void validateKey(const std::wstring &distro, const std::string &exe)
{
    if (distro.empty() || distro.size() > 1024 || distro.find(L'\0') != std::wstring::npos ||
        !validExecutable(exe))
        throw std::runtime_error("Saved scheduling needs a distribution and an absolute executable path that "
                                 "has not been deleted.");
}

Json schedulerInputs(const std::string &action, const Json &inputs)
{
    std::vector<const char *> fields;
    if (action == "set_nice")
        fields = {"nice"};
    else if (action == "set_affinity")
        fields = {"cpus"};
    else if (action == "set_policy")
        fields = {"policy"};
    else if (action == "set_io_priority")
        fields = {"class", "level"};
    else
        throw std::runtime_error("This action cannot be saved as a scheduling rule.");
    if (!inputs.is_object())
        throw std::runtime_error("Invalid saved scheduling inputs.");
    Json result = Json::object();
    for (const auto *field : fields)
    {
        // Non-best-effort I/O classes have no level, but accepting one keeps
        // the persistence format independent of the dialog's field layout.
        if (!inputs.contains(field) && action == "set_io_priority" && std::string(field) == "level" &&
            inputs.value("class", "") != "best-effort")
            continue;
        if (!inputs.contains(field) || !inputs.at(field).is_string())
            throw std::runtime_error("A saved scheduling input is missing or is not text.");
        const auto value = inputs.at(field).get<std::string>();
        const size_t limit = std::string(field) == "cpus" ? 65536 : 4096;
        if (value.empty() || value.size() > limit || value.find('\0') != std::string::npos)
            throw std::runtime_error("A saved scheduling input is invalid.");
        result[field] = value;
    }
    return result;
}

void load()
{
    if (loaded)
        return;
    loaded = true;
    try
    {
        DWORD bytes = 0;
        auto status =
            RegGetValueW(HKEY_CURRENT_USER, RegistryKey, ValueName, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
            return;
        if (status != ERROR_SUCCESS)
            throw std::runtime_error("Unable to read saved process scheduling.");
        if (bytes < sizeof(wchar_t) || bytes > MaximumBytes || bytes % sizeof(wchar_t))
            throw std::runtime_error("Saved process scheduling has an invalid size.");
        std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
        status = RegGetValueW(HKEY_CURRENT_USER, RegistryKey, ValueName, RRF_RT_REG_SZ, nullptr,
                              buffer.data(), &bytes);
        if (status != ERROR_SUCCESS)
            throw std::runtime_error("Unable to read saved process scheduling.");
        buffer.resize(bytes / sizeof(wchar_t));
        while (!buffer.empty() && buffer.back() == L'\0')
            buffer.pop_back();
        const auto document = Json::parse(utf8(buffer));
        if (!document.is_object() || document.value("version", 0) != 1 || !document.contains("rules") ||
            !document.at("rules").is_array() || document.at("rules").size() > MaximumRules)
            throw std::runtime_error("Saved process scheduling has an unsupported format.");
        Rules parsed;
        for (const auto &rule : document.at("rules"))
        {
            const auto distro = wide(rule.at("distro").get<std::string>());
            const auto exe = rule.at("exe").get<std::string>();
            validateKey(distro, exe);
            const auto &actions = rule.at("actions");
            if (!actions.is_object() || actions.empty() || actions.size() > 4)
                throw std::runtime_error("A saved process scheduling rule has invalid actions.");
            Json validated = Json::object();
            for (auto action = actions.begin(); action != actions.end(); ++action)
                validated[action.key()] = schedulerInputs(action.key(), action.value());
            if (!parsed.emplace(RuleKey{distro, exe}, std::move(validated)).second)
                throw std::runtime_error("Saved process scheduling contains a duplicate executable.");
        }
        rules = std::move(parsed);
    }
    catch (const std::exception &)
    {
        rules.clear();
        loadError =
            L"Saved process scheduling could not be loaded. Check the SavedProcessScheduling registry value.";
    }
}

void persist(Rules replacement)
{
    if (!loadError.empty())
        throw std::runtime_error(utf8(loadError));
    if (replacement.size() > MaximumRules)
        throw std::runtime_error("At most 128 executables can have saved scheduling settings.");
    Json entries = Json::array();
    for (const auto &rule : replacement)
        entries.push_back(
            {{"distro", utf8(rule.first.first)}, {"exe", rule.first.second}, {"actions", rule.second}});
    const auto value = wide(Json{{"version", 1}, {"rules", entries}}.dump());
    if ((value.size() + 1) * sizeof(wchar_t) > MaximumBytes)
        throw std::runtime_error("Saved process scheduling exceeds the 1 MiB limit.");
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Unable to open the settings registry key.");
    const auto status =
        RegSetValueExW(key, ValueName, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS)
        throw std::runtime_error("Unable to save process scheduling.");
    // An unsuccessful registry write must never enable a rule just in memory.
    rules = std::move(replacement);
    ++revision;
    distros.clear();
}

uint64_t unsignedField(const Json &item, const char *field)
{
    const auto &value = item.at(field);
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        throw std::runtime_error("Invalid process identity in scheduling snapshot.");
    return value.get<uint64_t>();
}
} // namespace

bool hasSavedScheduling(const std::wstring &distro)
{
    std::lock_guard<std::mutex> lock(mutex);
    load();
    const auto first = rules.lower_bound({distro, ""});
    return first != rules.end() && first->first.first == distro;
}

bool hasSavedScheduling(const std::wstring &distro, const std::string &exe)
{
    std::lock_guard<std::mutex> lock(mutex);
    load();
    return rules.count({distro, exe}) != 0;
}

Json savedSchedulingInputs(const std::wstring &distro, const std::string &exe, const std::string &action)
{
    std::lock_guard<std::mutex> lock(mutex);
    load();
    const auto rule = rules.find({distro, exe});
    if (rule == rules.end() || !rule->second.contains(action))
        return nullptr;
    return rule->second.at(action);
}

void saveScheduling(const std::wstring &distro, const std::string &exe, const std::string &action,
                    const Json &inputs)
{
    validateKey(distro, exe);
    auto validated = schedulerInputs(action, inputs);
    std::lock_guard<std::mutex> lock(mutex);
    load();
    auto replacement = rules;
    replacement[{distro, exe}][action] = std::move(validated);
    persist(std::move(replacement));
}

void removeSavedScheduling(const std::wstring &distro, const std::string &exe, const std::string &action)
{
    std::lock_guard<std::mutex> lock(mutex);
    load();
    if (!loadError.empty())
        throw std::runtime_error(utf8(loadError));
    auto replacement = rules;
    const auto rule = replacement.find({distro, exe});
    if (rule == replacement.end())
        return;
    if (action.empty())
        replacement.erase(rule);
    else
    {
        if (!rule->second.erase(action))
            return;
        if (rule->second.empty())
            replacement.erase(rule);
    }
    persist(std::move(replacement));
}

std::optional<Json> nextSavedScheduling(const std::wstring &distro, const Json &snapshot)
{
    std::lock_guard<std::mutex> lock(mutex);
    load();
    const auto first = rules.lower_bound({distro, ""});
    if (first == rules.end() || first->first.first != distro)
        return std::nullopt;
    try
    {
        const auto boot = snapshot.at("boot_id").get<std::string>();
        if (boot.empty() || !snapshot.at("processes").is_array())
            return std::nullopt;
        auto &state = distros[distro];
        if (state.boot != boot)
        {
            state = Distro{};
            state.boot = boot;
        }
        std::set<Identity> live;
        std::optional<Json> next;
        for (const auto &process : snapshot.at("processes"))
        {
            const Identity identity{unsignedField(process, "pid"), unsignedField(process, "start_ticks")};
            if (!identity.first || identity.first > static_cast<uint64_t>(std::numeric_limits<int>::max()))
                continue;
            live.insert(identity);
            const auto exe = process.value("exe", "");
            const auto rule = rules.find({distro, exe});
            if (rule == rules.end())
            {
                state.processes.erase(identity);
                continue;
            }
            if (next)
                continue;
            auto found = state.processes.find(identity);
            if (found == state.processes.end())
            {
                if (state.processes.size() >= MaximumIdentities)
                    continue;
                found = state.processes.emplace(identity, Process{}).first;
            }
            auto &tracked = found->second;
            const auto threads = unsignedField(process, "threads");
            if (tracked.exe != exe || tracked.threads != threads)
            {
                tracked = Process{};
                tracked.exe = exe;
                tracked.threads = threads;
            }
            for (auto action = rule->second.begin(); action != rule->second.end(); ++action)
            {
                if (tracked.attempts.count(action.key()))
                    continue;
                const auto token = nextToken++;
                next = Json{{"op", "thread"},
                            {"all_threads", true},
                            {"action", action.key()},
                            {"inputs", action.value()},
                            {"pid", identity.first},
                            {"start_ticks", identity.second},
                            {"expected_exe", exe},
                            {"_saved_scheduling", true},
                            {"_saved_scheduling_revision", revision},
                            {"_saved_scheduling_threads", threads},
                            {"_saved_scheduling_boot", boot},
                            {"_saved_scheduling_attempt", token}};
                break;
            }
        }
        if (!snapshot.value("processes_truncated", false))
            for (auto item = state.processes.begin(); item != state.processes.end();)
                if (!live.count(item->first))
                    item = state.processes.erase(item);
                else
                    ++item;
        // Reserve only after the whole snapshot was parsed. A malformed later
        // row must not leave an action marked pending without returning it.
        if (next)
        {
            const Identity identity{unsignedField(*next, "pid"), unsignedField(*next, "start_ticks")};
            state.processes.at(identity).attempts.emplace(
                next->at("action").get<std::string>(),
                Attempt{true, next->at("_saved_scheduling_attempt").get<uint64_t>()});
        }
        return next;
    }
    catch (const std::exception &)
    {
        distros[distro].error = L"Saved process scheduling could not read the process snapshot.";
        return std::nullopt;
    }
}

bool savedSchedulingCurrent(const std::wstring &distro, const Json &request)
{
    std::lock_guard<std::mutex> lock(mutex);
    try
    {
        if (!request.value("_saved_scheduling", false) ||
            request.at("_saved_scheduling_revision").get<uint64_t>() != revision)
            return false;
        const auto exe = request.at("expected_exe").get<std::string>();
        const auto action = request.at("action").get<std::string>();
        const auto rule = rules.find({distro, exe});
        if (rule == rules.end() || !rule->second.contains(action) ||
            rule->second.at(action) != request.at("inputs"))
            return false;
        const auto state = distros.find(distro);
        if (state == distros.end() ||
            state->second.boot != request.at("_saved_scheduling_boot").get<std::string>())
            return false;
        const auto process = state->second.processes.find(
            {unsignedField(request, "pid"), unsignedField(request, "start_ticks")});
        if (process == state->second.processes.end() || process->second.exe != exe ||
            process->second.threads != request.at("_saved_scheduling_threads").get<uint64_t>())
            return false;
        const auto attempt = process->second.attempts.find(action);
        return attempt != process->second.attempts.end() && attempt->second.pending &&
               attempt->second.token == request.at("_saved_scheduling_attempt").get<uint64_t>();
    }
    catch (const std::exception &)
    {
        return false;
    }
}

void finishSavedScheduling(const std::wstring &distro, const Json &request, const std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex);
    try
    {
        if (request.at("_saved_scheduling_revision").get<uint64_t>() != revision)
            return;
        const auto found = distros.find(distro);
        if (found == distros.end() ||
            found->second.boot != request.at("_saved_scheduling_boot").get<std::string>())
            return;
        auto &state = found->second;
        const auto process =
            state.processes.find({unsignedField(request, "pid"), unsignedField(request, "start_ticks")});
        if (process == state.processes.end() ||
            process->second.exe != request.at("expected_exe").get<std::string>() ||
            process->second.threads != request.at("_saved_scheduling_threads").get<uint64_t>())
            return;
        const auto attempt = process->second.attempts.find(request.at("action").get<std::string>());
        if (attempt == process->second.attempts.end() || !attempt->second.pending ||
            attempt->second.token != request.at("_saved_scheduling_attempt").get<uint64_t>())
            return;
        // A failed application remains attempted until the identity, thread
        // count or rule changes; a persistent permissions error must not poll.
        attempt->second.pending = false;
        if (!error.empty())
        {
            auto message = wide(error);
            std::replace(message.begin(), message.end(), L'\r', L' ');
            std::replace(message.begin(), message.end(), L'\n', L' ');
            if (message.size() > 240)
                message = message.substr(0, 237) + L"...";
            state.error =
                L"Saved scheduling (PID " + std::to_wstring(unsignedField(request, "pid")) + L"): " + message;
        }
    }
    catch (const std::exception &)
    {
        // Completion may arrive after a rule was removed or the distro closed.
        // It must never escape into the controller's response-delivery path.
    }
}

void cancelSavedScheduling(const std::wstring &distro, const Json &request)
{
    std::lock_guard<std::mutex> lock(mutex);
    try
    {
        const auto state = distros.find(distro);
        if (state == distros.end() || request.at("_saved_scheduling_revision").get<uint64_t>() != revision ||
            state->second.boot != request.at("_saved_scheduling_boot").get<std::string>())
            return;
        const auto process = state->second.processes.find(
            {unsignedField(request, "pid"), unsignedField(request, "start_ticks")});
        if (process == state->second.processes.end())
            return;
        const auto attempt = process->second.attempts.find(request.at("action").get<std::string>());
        if (attempt != process->second.attempts.end() && attempt->second.pending &&
            attempt->second.token == request.at("_saved_scheduling_attempt").get<uint64_t>())
            process->second.attempts.erase(attempt);
    }
    catch (const std::exception &)
    {
    }
}

std::wstring savedSchedulingError(const std::wstring &distro)
{
    std::lock_guard<std::mutex> lock(mutex);
    load();
    if (!loadError.empty())
        return loadError;
    const auto found = distros.find(distro);
    return found == distros.end() ? L"" : found->second.error;
}
} // namespace wsl
