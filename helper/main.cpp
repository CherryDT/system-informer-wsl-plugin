#include "observer.hpp"

#include <iostream>
#include <stdexcept>

namespace {
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
    if (operation == "hello") return observer::hello();
    if (operation == "snapshot") return observer::snapshot(request);
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
int main() {
    using observer::Json;
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
