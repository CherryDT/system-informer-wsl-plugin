#include "observer.hpp"

#include <iostream>
#include <stdexcept>

namespace {
observer::Json dispatch(const observer::Json& request) {
    const auto operation = request.at("op").get<std::string>();
    if (operation == "hello") return observer::hello();
    if (operation == "snapshot") return observer::snapshot();
    if (operation == "details") return observer::process_details(request);
    if (operation == "connections") return observer::connections(request);
    if (operation == "signal") return observer::send_signal(request);
    if (operation == "services") return observer::services();
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
