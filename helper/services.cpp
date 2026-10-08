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
        arguments[0] != "gdb" && arguments[0] != "lldb" && arguments[0] != "py-spy" && arguments[0] != "jcmd"))
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
Json services() {
    if (!systemd_available()) return {{"available", false}, {"services", Json::array()},
        {"message", "systemd is not running in this distribution"}};
    std::map<std::string, Json> units;
    const auto result = run_command({"systemctl", "list-units", "--all", "--type=service", "--no-pager", "--plain", "--output=json"});
    bool loaded_json = false;
    if (!result.timed_out && result.exit_code == 0) {
        const auto parsed = Json::parse(result.output, nullptr, false);
        if (parsed.is_array()) {
            loaded_json = true;
            for (const auto& item : parsed) {
                const auto name = item.value("unit", "");
                units[name] = {{"name", name}, {"description", item.value("description", "")},
                    {"load", item.value("load", "")}, {"active", item.value("active", "")},
                    {"sub", item.value("sub", "")}, {"enabled", "unknown"}};
            }
        }
    }
    if (!loaded_json) {
        // Older systemd releases do not support JSON for list-units. With C
        // locale and no legend, only the description can contain whitespace.
        const auto fallback = run_command({"systemctl", "list-units", "--all", "--type=service", "--no-pager", "--plain", "--no-legend", "--full"});
        std::istringstream lines(checked_output(fallback));
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string name, load, active, sub, description;
            if (!(fields >> name >> load >> active >> sub)) continue;
            std::getline(fields >> std::ws, description);
            units[name] = {{"name", name}, {"description", description}, {"load", load},
                {"active", active}, {"sub", sub}, {"enabled", "unknown"}};
        }
    }
    auto merge_installed = [&units](const std::string& name, const std::string& enabled) {
        auto found = units.find(name);
        if (found == units.end()) {
            units[name] = {{"name", name}, {"description", ""}, {"load", "not loaded"},
                {"active", "inactive"}, {"sub", "dead"}, {"enabled", enabled}};
        } else {
            found->second["enabled"] = enabled;
        }
    };
    const auto installed = run_command({"systemctl", "list-unit-files", "--type=service", "--no-pager", "--output=json"});
    bool installed_json = false;
    if (!installed.timed_out && installed.exit_code == 0) {
        const auto parsed = Json::parse(installed.output, nullptr, false);
        if (parsed.is_array()) {
            installed_json = true;
            for (const auto& item : parsed)
                merge_installed(item.value("unit_file", ""), item.value("state", "unknown"));
        }
    }
    std::string warning;
    if (!installed_json) {
        const auto fallback = run_command({"systemctl", "list-unit-files", "--type=service", "--no-pager", "--no-legend", "--full"});
        if (!fallback.timed_out && fallback.exit_code == 0) {
            std::istringstream lines(fallback.output);
            std::string line;
            while (std::getline(lines, line)) {
                std::istringstream fields(line);
                std::string name, enabled;
                if (fields >> name >> enabled) merge_installed(name, enabled);
            }
        } else {
            warning = "Loaded units are shown; installed unit-file states are unavailable: " + fallback.output;
        }
    }
    Json rows = Json::array();
    for (auto& entry : units) rows.push_back(std::move(entry.second));
    return {{"available", true}, {"services", rows}, {"message", warning}};
}
Json service_details(const Json& request) {
    const auto name = unit_name(request);
    if (!systemd_available()) throw std::runtime_error("systemd is not running in this distribution");
    const auto properties = run_command({"systemctl", "show", "--no-pager", "--", name});
    checked_output(properties);
    const auto status = run_command({"systemctl", "status", "--no-pager", "--full", "--", name});
    const auto unit = run_command({"systemctl", "cat", "--no-pager", "--", name});
    const auto journal = run_command({"journalctl", "--unit=" + name, "--lines=100", "--no-pager", "--output=short-iso"});
    auto format = [](const CommandResult& result) {
        auto text = result.output.substr(0, 128 * 1024);
        if (text.size() < result.output.size()) text += "\n[Display truncated]\n";
        return text + (result.timed_out ? "\n[Command timed out]\n" : "");
    };
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
    return {{"overview", overview}, {"text", "Status\n" + format(status) + "\nProperties\n" + format(properties) + "\nUnit files\n" + format(unit) + "\nRecent journal\n" + format(journal)}};
}
Json service_action(const Json& request) {
    const auto name = unit_name(request);
    const auto action = request.at("action").get<std::string>();
    const std::vector<std::string> allowed{"start", "stop", "restart", "reload", "enable", "disable"};
    if (std::find(allowed.begin(), allowed.end(), action) == allowed.end()) throw std::runtime_error("Unsupported service action");
    if (!systemd_available()) throw std::runtime_error("systemd is not running in this distribution");
    // --no-block returns once a job is queued. The UI refresh reports its actual
    // state; a service with a long startup must not freeze the transport.
    const auto result = run_command({"systemctl", "--no-ask-password", "--no-pager", "--no-block", action, "--", name}, 10000);
    return {{"accepted", true}, {"message", checked_output(result)}};
}
} // namespace observer
