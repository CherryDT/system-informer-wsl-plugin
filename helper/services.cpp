#include "observer.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
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

CommandResult run_command(const std::vector<std::string>& arguments, int timeout_ms) {
    if (arguments.empty()) throw std::runtime_error("Missing command");
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) != 0) throw std::runtime_error("Cannot create command pipe");
    // Build argv before fork; the child performs only minimal descriptor setup
    // and exec. There is no shell, and unit names never become command syntax.
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); throw std::runtime_error("Cannot start command"); }
    if (child == 0) {
        setpgid(0, 0);
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) { dup2(null_fd, STDIN_FILENO); close(null_fd); }
        close(pipes[0]); close(pipes[1]);
        setenv("LC_ALL", "C", 1);
        setenv("SYSTEMD_COLORS", "0", 1);
        setenv("SYSTEMD_PAGER", "cat", 1);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    setpgid(child, child);
    close(pipes[1]);
    const int old_flags = fcntl(pipes[0], F_GETFL, 0);
    fcntl(pipes[0], F_SETFL, old_flags | O_NONBLOCK);
    CommandResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool exited = false, eof = false;
    int status = 0;
    constexpr size_t output_limit = 2 * 1024 * 1024;
    while (!exited || !eof) {
        char buffer[8192];
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
        if (!exited) exited = waitpid(child, &status, WNOHANG) == child;
        if (exited && eof) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            // Descendants may inherit stdout. Kill the whole command group so
            // an inherited pipe cannot make a read hang after the leader exits.
            kill(-child, SIGKILL);
            if (!exited) while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
            break;
        }
        pollfd descriptor{pipes[0], POLLIN | POLLHUP, 0};
        poll(&descriptor, 1, 50);
    }
    close(pipes[0]);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
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
        return result.output + (result.timed_out ? "\n[Command timed out]\n" : "");
    };
    return {{"text", "Status\n" + format(status) + "\nProperties\n" + properties.output + "\nUnit files\n" + format(unit) + "\nRecent journal\n" + format(journal)}};
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
