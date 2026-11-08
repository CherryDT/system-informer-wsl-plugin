#include "observer.hpp"
#include "resource_tools.hpp"
#include "thread_tools.hpp"

#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// Only the connection bootstrap creates this root-private staging directory.
// Unlink after exec so normal EOF, cancellation and crashes leave no installed
// observer behind. Linux keeps the running executable available through /proc.
void remove_ephemeral_executable() {
    const auto executable = observer::read_link("/proc/self/exe");
    const std::string prefix = "/run/system-informer-wsl.";
    const auto slash = executable.find_last_of('/');
    if (executable.rfind(prefix, 0) != 0 || slash == std::string::npos ||
        executable.substr(slash + 1) != "wsl-observer" ||
        executable.find('/', prefix.size()) != slash)
        throw std::runtime_error("Unexpected temporary observer path");
    const auto directory = executable.substr(0, slash);
    struct stat file{}, parent{};
    if (geteuid() != 0 || lstat(directory.c_str(), &parent) != 0 ||
        !S_ISDIR(parent.st_mode) || parent.st_uid != 0 || (parent.st_mode & 0777) != 0700 ||
        lstat(executable.c_str(), &file) != 0 || !S_ISREG(file.st_mode) ||
        file.st_uid != 0 || file.st_nlink != 1 || (file.st_mode & 0777) != 0700)
        throw std::runtime_error("Unsafe temporary observer file");
    if (unlink(executable.c_str()) != 0 || rmdir(directory.c_str()) != 0)
        throw std::runtime_error("Could not remove the temporary observer upload");
}

void check_nesting(const std::string& line) {
    unsigned depth = 0;
    bool in_string = false, escaped = false;
    for (const char character : line) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') in_string = false;
        } else if (character == '"') {
            in_string = true;
        } else if (character == '{' || character == '[') {
            if (++depth > 32) throw std::runtime_error("Request nesting exceeds 32 levels");
        } else if ((character == '}' || character == ']') && depth != 0) {
            --depth;
        }
    }
}
observer::Json dispatch(const observer::Json& request) {
    const auto operation = request.at("op").get<std::string>();
    if (operation == "resource") return observer::resource_tool(request);
    if (operation == "thread") return observer::thread_tool(request);
    if (operation == "hello") return observer::hello();
    if (operation == "snapshot") return observer::snapshot(request);
    if (operation == "find_handles") return observer::find_handles(request);
    if (operation == "details") return observer::process_details(request);
    if (operation == "connections") return observer::connections(request);
    if (operation == "signal") return observer::send_signal(request);
    if (operation == "stacks") return observer::process_stacks(request);
    if (operation == "script_stacks") return observer::script_stacks(request);
    if (operation == "services") return observer::services(request);
    if (operation == "service_details") return observer::service_details(request);
    if (operation == "service_action") return observer::service_action(request);
    throw std::runtime_error("Unknown operation: " + operation);
}
}
int main(int argc, char** argv) {
    using observer::Json;
    try {
        if (argc == 2 && std::string(argv[1]) == "--ephemeral") {
            // The temporary observer is launched inside WSLg. Its /proc can
            // also see the paired user distro, which is a separate target.
            observer::restrict_to_current_pid_namespace();
            remove_ephemeral_executable();
        }
        else if (argc != 1)
            throw std::runtime_error("Unknown observer argument");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::ios::sync_with_stdio(false);
    constexpr size_t maximum_request = 1024 * 1024;
    std::string line;
    bool oversized = false;
    char character;
    // Keep draining an oversized request until its newline, without allocating
    // unbounded memory. stdout is reserved exclusively for protocol responses.
    while (std::cin.get(character)) {
        if (character != '\n') {
            if (line.size() < maximum_request) line.push_back(character);
            else oversized = true;
            continue;
        }
        Json response = {{"id", nullptr}, {"ok", false}};
        try {
            if (oversized) throw std::runtime_error("Request exceeds 1 MiB");
            check_nesting(line);
            const auto request = Json::parse(line);
            if (!request.is_object() || !request.contains("id") || !request["id"].is_number_integer())
                throw std::runtime_error("Request must include an integer id");
            response["id"] = request["id"];
            response["data"] = dispatch(request);
            response["ok"] = true;
        } catch (const std::exception& error) {
            response["error"] = error.what();
        }
        // Filenames and command lines can contain arbitrary bytes on Linux.
        // Replace invalid UTF-8 rather than losing the whole snapshot.
        std::cout << response.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n' << std::flush;
        if (!std::cout) break;
        line.clear();
        oversized = false;
    }
    return 0;
}
