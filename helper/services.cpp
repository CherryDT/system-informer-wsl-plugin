#include "observer.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <grp.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace observer {
namespace {
std::string unit_name(const Json& request) {
    auto name = request.at("name").get<std::string>();
    if (name.size() > 255 || name.size() <= 8 || name[0] == '-' ||
        name.compare(name.size() - 8, 8, ".service") != 0 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:_.@-\\") != std::string::npos)
        throw std::runtime_error("Expected a valid .service unit name");
    return name;
}
std::string checked_output(const CommandResult& result) {
    if (result.timed_out) throw std::runtime_error("Command timed out: " + result.output);
    if (result.exit_code != 0) throw std::runtime_error(result.output.empty() ? "Command failed" : result.output);
    return result.output;
}
bool systemd_available() { return access("/run/systemd/system", F_OK) == 0; }
struct ServiceFileCache {
    std::map<std::string, std::string> states;
    std::chrono::steady_clock::time_point expires{};
    bool complete = false;
};
ServiceFileCache service_file_cache;
}

std::string trusted_command_path(const std::string& path, uint32_t owner) {
    // Resolve symlinks once, then execute that canonical path. Check every
    // component so a writable directory cannot substitute privileged code.
    char* resolved = realpath(path.c_str(), nullptr);
    if (!resolved) return {};
    const std::string canonical(resolved);
    free(resolved);
    std::string current = canonical;
    bool file = true;
    for (;;) {
        struct stat metadata{};
        if (lstat(current.c_str(), &metadata) != 0 ||
            (metadata.st_uid != 0 && metadata.st_uid != owner) ||
            (metadata.st_mode & (S_IWGRP | S_IWOTH)) ||
            (file ? !S_ISREG(metadata.st_mode) : !S_ISDIR(metadata.st_mode))) return {};
        if (current == "/") break;
        file = false;
        const auto slash = current.find_last_of('/');
        current = slash == 0 ? "/" : current.substr(0, slash);
    }
    return canonical;
}

std::string find_command(const std::string& name) {
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) return {};
    for (const auto* directory : {"/usr/local/bin/", "/usr/bin/", "/bin/"}) {
        const auto path = trusted_command_path(std::string(directory) + name);
        if (!path.empty() && access(path.c_str(), X_OK) == 0) return path;
    }
    return {};
}

CommandResult run_command(const std::vector<std::string>& arguments, int timeout_ms, size_t output_limit,
    std::optional<CommandCredentials> credentials, const std::string& java_tool) {
    if (arguments.empty() || (arguments[0] != "systemctl" && arguments[0] != "journalctl" &&
        arguments[0] != "gdb" && arguments[0] != "lldb" && arguments[0] != "py-spy" && arguments[0] != "jcmd" &&
        arguments[0] != "python3" && arguments[0] != "getent"))
        throw std::runtime_error("Unsupported system command");
    std::string executable;
    if (!java_tool.empty()) {
        if (arguments[0] != "jcmd" || !credentials) throw std::runtime_error("Invalid Java tool request");
        executable = trusted_command_path(java_tool, credentials->uid);
    } else {
        executable = find_command(arguments[0]);
    }
    if (executable.empty()) throw std::runtime_error(arguments[0] + " is not installed in a trusted location");

    // A process stuck in an uninterruptible kernel wait may outlive SIGKILL.
    // Reap it on the next command instead of blocking the entire transport.
    static std::vector<pid_t> pending_children;
    pending_children.erase(std::remove_if(pending_children.begin(), pending_children.end(), [](pid_t pid) {
        int status;
        const auto result = waitpid(pid, &status, WNOHANG);
        return result == pid || (result < 0 && errno == ECHILD);
    }), pending_children.end());
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) != 0) throw std::runtime_error("Cannot create command pipe");
    if (fcntl(pipes[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipes[0]); close(pipes[1]);
        throw std::runtime_error("Cannot configure command pipe");
    }
    // Never search a root process's inherited PATH or inherit dynamic-loader,
    // bus-address, pager, or other environment overrides from the caller.
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> environment{"PATH=/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C",
        "SYSTEMD_COLORS=0", "SYSTEMD_URLIFY=0", "SYSTEMD_PAGER=cat", "DEBUGINFOD_URLS=", "HOME=/"};
    // Without this, llnode can silently replace undecodable JS frames with
    // native addresses and still return success. The caller filters diagnostics.
    if (arguments[0] == "lldb") environment.push_back("LLNODE_DEBUG=true");
    std::vector<char*> envp;
    for (auto& entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); throw std::runtime_error("Cannot start command"); }
    if (child == 0) {
        if (setpgid(0, 0) != 0 || chdir("/") != 0) _exit(126);
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
            dup2(pipes[1], STDOUT_FILENO) < 0 || dup2(pipes[1], STDERR_FILENO) < 0) _exit(126);
        close(null_fd); close(pipes[0]); close(pipes[1]);
        const rlimit no_core{0, 0};
        if (setrlimit(RLIMIT_CORE, &no_core) != 0 || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(126);
        if (credentials) {
            // HotSpot's attach protocol checks both effective UID and GID.
            // Drop supplementary groups and all saved IDs before executing any
            // target-owned JDK tool; never run that code with root privileges.
            if (geteuid() == 0) {
                if (setgroups(0, nullptr) != 0 ||
                    setresgid(credentials->gid, credentials->gid, credentials->gid) != 0 ||
                    setresuid(credentials->uid, credentials->uid, credentials->uid) != 0) _exit(126);
            } else if (geteuid() != credentials->uid || getegid() != credentials->gid) _exit(126);
        }
        execve(executable.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    setpgid(child, child);
    close(pipes[1]);
    CommandResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool exited = false, eof = false;
    while (!exited || !eof) {
        char buffer[8192];
        // A continuously writing child must not prevent deadline checks.
        for (unsigned reads = 0; reads < 64; ++reads) {
            const auto count = read(pipes[0], buffer, sizeof(buffer));
            if (count > 0) {
                if (result.output.size() < output_limit)
                    result.output.append(buffer, std::min(static_cast<size_t>(count), output_limit - result.output.size()));
            } else {
                if (count == 0) eof = true;
                break;
            }
        }
        if (!exited) {
            siginfo_t info{};
            // Observe exit without releasing the PID: descendants may still
            // own the pipe, and timeout cleanup must never kill a reused group.
            if (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT) == 0)
                exited = info.si_pid == child;
        }
        if (exited && eof) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            kill(-child, SIGKILL);
            kill(child, SIGKILL);
            break;
        }
        pollfd descriptor{pipes[0], POLLIN | POLLHUP, 0};
        if (eof) poll(nullptr, 0, 20);
        else poll(&descriptor, 1, 50);
    }
    close(pipes[0]);
    int status = 0;
    const auto reap_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    for (;;) {
        const auto reaped = waitpid(child, &status, WNOHANG);
        if (reaped == child) {
            result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            break;
        }
        if (reaped < 0 && errno != EINTR) break;
        if (std::chrono::steady_clock::now() >= reap_deadline) {
            pending_children.push_back(child);
            break;
        }
        poll(nullptr, 0, 10);
    }
    if (result.output.size() == output_limit) result.output += "\n[Output truncated]\n";
    return result;
}
Json services(const Json& request) {
    const bool identities_only = request.value("identities_only", false);
    const bool include_pids = !identities_only && request.value("include_pids", false);
    if (!systemd_available()) return {{"available", false}, {"services", Json::array()},
        {"message", "systemd is not running in this distribution"}};
    auto complete_output = [](const CommandResult& result) {
        return !result.timed_out && result.exit_code == 0 &&
            result.output.find("[Output truncated]") == std::string::npos;
    };
    auto valid_name = [](const std::string& name) {
        return name.size() > 8 && name.compare(name.size() - 8, 8, ".service") == 0;
    };
    std::map<std::string, Json> units;
    bool loaded = false, loaded_complete = true, pids_complete = false;
    if (include_pids) {
        // Retrieve state and MainPID together, rather than asking systemd for
        // a list and then making a second request for the same loaded units.
        // The literal glob is expanded by systemctl, never by a shell.
        const auto properties = run_command({"systemctl", "show", "--all", "--no-pager",
            "--property=Id,MainPID,Description,LoadState,ActiveState,SubState", "--", "*.service"});
        if (complete_output(properties)) {
            loaded = true;
            pids_complete = true;
            std::map<std::string, std::string> values;
            auto merge_unit = [&] {
                if (values.empty()) return;
                const auto name = values["Id"];
                if (!valid_name(name)) {
                    loaded_complete = false;
                    pids_complete = false;
                    values.clear();
                    return;
                }
                Json unit = {{"name", name}, {"description", values["Description"]},
                    {"load", values["LoadState"]}, {"active", values["ActiveState"]},
                    {"sub", values["SubState"]}, {"enabled", "unknown"},
                    {"pid", 0}, {"start_ticks", 0}};
                const auto& main_pid = values["MainPID"];
                if (!main_pid.empty() && main_pid.find_first_not_of("0123456789") == std::string::npos) {
                    try {
                        const auto pid = std::stoi(main_pid);
                        if (pid > 0) {
                            const auto process = process_stat(pid);
                            unit["pid"] = pid;
                            unit["start_ticks"] = process.start_ticks;
                        }
                    } catch (const std::exception&) {
                        // A service can exit between the D-Bus reply and /proc
                        // read. Do not offer navigation without its identity.
                    }
                }
                units[name] = std::move(unit);
                values.clear();
            };
            std::istringstream lines(properties.output);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.empty()) merge_unit();
                else {
                    const auto separator = line.find('=');
                    if (separator != std::string::npos)
                        values[line.substr(0, separator)] = line.substr(separator + 1);
                }
            }
            merge_unit();
        }
    }
    if (!loaded) {
        const auto result = run_command({"systemctl", "list-units", "--all", "--type=service",
            "--no-pager", "--plain", "--output=json"});
        if (complete_output(result)) {
            const auto parsed = Json::parse(result.output, nullptr, false);
            if (parsed.is_array()) {
                loaded = true;
                for (const auto& item : parsed) {
                    const auto name = item.value("unit", "");
                    if (!valid_name(name)) { loaded_complete = false; continue; }
                    units[name] = {{"name", name}, {"description", item.value("description", "")},
                        {"load", item.value("load", "")}, {"active", item.value("active", "")},
                        {"sub", item.value("sub", "")}, {"enabled", "unknown"}};
                }
            }
        }
    }
    if (!loaded) {
        // Older systemd releases do not support JSON for list-units. With C
        // locale and no legend, only the description can contain whitespace.
        const auto fallback = run_command({"systemctl", "list-units", "--all", "--type=service",
            "--no-pager", "--plain", "--no-legend", "--full"});
        std::istringstream lines(checked_output(fallback));
        loaded_complete = complete_output(fallback);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string name, load, active, sub, description;
            if (!(fields >> name >> load >> active >> sub) || !valid_name(name)) continue;
            std::getline(fields >> std::ws, description);
            units[name] = {{"name", name}, {"description", description}, {"load", load},
                {"active", active}, {"sub", sub}, {"enabled", "unknown"}};
        }
    }

    // Installed unit files and startup policy rarely change on each tick.
    // Explicit refresh and successful enable/disable actions bypass this cache.
    const auto now = std::chrono::steady_clock::now();
    if (request.value("refresh_metadata", false) || now >= service_file_cache.expires) {
        std::map<std::string, std::string> states;
        bool complete = false;
        const auto installed = run_command({"systemctl", "list-unit-files", "--type=service",
            "--no-pager", "--output=json"});
        if (complete_output(installed)) {
            const auto parsed = Json::parse(installed.output, nullptr, false);
            if (parsed.is_array()) {
                complete = true;
                for (const auto& item : parsed) {
                    const auto name = item.value("unit_file", "");
                    if (valid_name(name)) states[name] = item.value("state", "unknown");
                    else complete = false;
                }
            }
        }
        if (!complete) {
            states.clear();
            const auto fallback = run_command({"systemctl", "list-unit-files", "--type=service",
                "--no-pager", "--no-legend", "--full"});
            if (complete_output(fallback)) {
                complete = true;
                std::istringstream lines(fallback.output);
                std::string line;
                while (std::getline(lines, line)) {
                    std::istringstream fields(line);
                    std::string name, enabled;
                    if (fields >> name >> enabled && valid_name(name)) states[name] = enabled;
                }
            }
        }
        if (complete) service_file_cache.states = std::move(states);
        // Retain old metadata on failure, but mark the response incomplete so
        // missing rows are not treated as deleted. Retry failures after 5s.
        service_file_cache.complete = complete;
        service_file_cache.expires = std::chrono::steady_clock::now() +
            std::chrono::seconds(complete ? 30 : 5);
    }
    for (const auto& entry : service_file_cache.states) {
        const auto found = units.find(entry.first);
        if (found == units.end()) {
            units[entry.first] = {{"name", entry.first}, {"description", ""}, {"load", "not loaded"},
                {"active", "inactive"}, {"sub", "dead"}, {"enabled", entry.second}};
        } else {
            found->second["enabled"] = entry.second;
        }
    }
    std::string warning;
    if (!service_file_cache.complete)
        warning = "Installed unit-file states could not be refreshed; any previous metadata is retained.";
    if (include_pids && !pids_complete) {
        if (!warning.empty()) warning += "\n";
        warning += "Service main process IDs are unavailable; refresh to retry.";
    }
    Json rows = Json::array();
    bool truncated = !loaded_complete || !service_file_cache.complete;
    size_t response_budget = 12 * 1024 * 1024;
    for (auto& entry : units) {
        if (include_pids && !entry.second.contains("pid")) {
            entry.second["pid"] = 0;
            entry.second["start_ticks"] = 0;
        }
        auto row = identities_only ? Json{{"name", entry.first}} : std::move(entry.second);
        if (!append_with_budget(rows, std::move(row), response_budget)) { truncated = true; break; }
    }
    return {{"available", true}, {"services", rows}, {"message", warning},
            {"identities_only", identities_only}, {"include_pids", include_pids},
            {"pids_complete", pids_complete}, {"services_truncated", truncated}};
}
Json service_details(const Json& request) {
    const auto name = unit_name(request);
    if (!systemd_available()) throw std::runtime_error("systemd is not running in this distribution");
    auto format = [](const CommandResult& result) {
        auto text = result.output.substr(0, 128 * 1024);
        if (text.size() < result.output.size()) text += "\n[Display truncated]\n";
        return text + (result.timed_out ? "\n[Command timed out]\n" : "");
    };
    const bool is_template = name.size() >= 9 && name.compare(name.size() - 9, 9, "@.service") == 0;
    if (is_template) {
        // A template is a unit definition, not a runtime unit. systemctl show
        // rejects the bare @.service name; never invent an instance to query.
        const auto unit = run_command({"systemctl", "cat", "--no-pager", "--", name});
        checked_output(unit);
        const auto files = run_command({"systemctl", "list-unit-files", "--no-pager", "--no-legend", "--full", "--", name});
        std::string enabled = "unknown", listed_name;
        if (!files.timed_out && files.exit_code == 0) {
            std::istringstream row(files.output);
            row >> listed_name >> enabled;
            if (listed_name != name) enabled = "unknown";
        }
        std::string path;
        std::istringstream lines(unit.output);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("# /", 0) == 0) { path = line.substr(2); break; }
        }
        const std::string explanation = "This is a service template. Choose a named instance to inspect runtime properties or control its process.";
        auto pattern = name;
        pattern.insert(pattern.size() - 8, "*");
        const auto journal = run_command({"journalctl", "--unit=" + pattern, "--lines=100", "--no-pager", "--output=short-iso"});
        Json overview{{"name", name}, {"is_template", true}, {"description", explanation},
            {"load", "Unit definition"}, {"active", "Template"}, {"sub", "Not an instance"},
            {"enabled", enabled}, {"fragment_path", path}, {"main_pid", "Not instantiated"},
            {"user", "Instance-specific"}, {"group", "Instance-specific"},
            {"exec_start", "See the unit definition; instance specifiers are not expanded."}};
        return {{"overview", overview},
            {"journal", "Recent entries for instances matching " + pattern + "\n\n" + format(journal)},
            {"text", explanation + "\n\nUnit files\n" + format(unit)}};
    }
    const auto properties = run_command({"systemctl", "show", "--no-pager", "--", name});
    checked_output(properties);
    // Keep journal history in its own response field and inspector page.
    const auto status = run_command({"systemctl", "status", "--no-pager", "--full", "--lines=0", "--", name});
    const auto unit = run_command({"systemctl", "cat", "--no-pager", "--", name});
    const auto journal = run_command({"journalctl", "--unit=" + name, "--lines=100", "--no-pager", "--output=short-iso"});
    // systemctl show properties are one key=value per line. Split once so
    // commands and descriptions containing '=' retain their complete value.
    std::map<std::string, std::string> property_values;
    std::istringstream property_lines(properties.output);
    std::string property_line;
    while (std::getline(property_lines, property_line)) {
        const auto separator = property_line.find('=');
        if (separator == std::string::npos) continue;
        auto value = property_line.substr(separator + 1);
        if (value.size() > 16 * 1024) value = value.substr(0, 16 * 1024) + " [truncated]";
        property_values[property_line.substr(0, separator)] = std::move(value);
    }
    const std::pair<const char*, const char*> fields[] = {
        {"name", "Id"}, {"description", "Description"}, {"load", "LoadState"},
        {"active", "ActiveState"}, {"sub", "SubState"}, {"enabled", "UnitFileState"},
        {"main_pid", "MainPID"}, {"fragment_path", "FragmentPath"}, {"exec_start", "ExecStart"},
        {"user", "User"}, {"group", "Group"}, {"restarts", "NRestarts"},
        {"result", "Result"}, {"active_since", "ActiveEnterTimestamp"},
        {"memory_current", "MemoryCurrent"}, {"tasks_current", "TasksCurrent"}
    };
    Json overview = Json::object();
    for (const auto& field : fields) overview[field.first] = property_values[field.second];
    if (overview["name"].get<std::string>().empty()) overview["name"] = name;
    return {{"overview", overview}, {"journal", format(journal)},
            {"text", "Status\n" + format(status) + "\nProperties\n" + format(properties) + "\nUnit files\n" + format(unit)}};
}
Json service_action(const Json& request) {
    const auto name = unit_name(request);
    const auto action = request.at("action").get<std::string>();
    const std::vector<std::string> allowed{"start", "stop", "restart", "reload", "enable", "disable"};
    if (std::find(allowed.begin(), allowed.end(), action) == allowed.end()) throw std::runtime_error("Unsupported service action");
    if (name.size() >= 9 && name.compare(name.size() - 9, 9, "@.service") == 0 &&
        action != "enable" && action != "disable")
        throw std::runtime_error("Choose a named service instance; a template has no running process to control.");
    if (!systemd_available()) throw std::runtime_error("systemd is not running in this distribution");
    // --no-block returns once a job is queued. The UI refresh reports its actual
    // state; a service with a long startup must not freeze the transport.
    const auto result = run_command({"systemctl", "--no-ask-password", "--no-pager", "--no-block", action, "--", name}, 10000);
    auto message = checked_output(result);
    if (action == "enable" || action == "disable") service_file_cache.expires = {};
    return {{"accepted", true}, {"message", std::move(message)}};
}
} // namespace observer
