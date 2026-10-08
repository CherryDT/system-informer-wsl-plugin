#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "../vendor/json.hpp"

namespace wsl {

using Json = nlohmann::json;

std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
// Quote one argument using the Windows CommandLineToArgvW conventions.
std::wstring quoteArg(const std::wstring& value);

struct Distro {
    std::wstring name;
};

// Lists already-running WSL2 distributions. Discovery never launches a distro.
std::vector<Distro> runningDistros();

class Client {
public:
    Client(std::wstring distro, std::wstring helperPath);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Call from one worker thread. Failed clients are terminal: create a new
    // client only after the user explicitly reconnects or discovery sees it again.
    Json request(const Json& payload,
        std::chrono::milliseconds timeout = std::chrono::seconds(20));

    // Thread-safe cancellation. The worker releases pipes and its launcher when
    // it observes cancellation; this does not shut down WSL or the distribution.
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wsl
