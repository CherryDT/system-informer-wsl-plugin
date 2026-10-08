#pragma once

#include <chrono>
#include <functional>
#include <stdexcept>
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

// Reads registered WSL2 names without starting any distribution.
std::vector<std::wstring> registeredWsl2Distros();

// Lists already-running WSL2 distributions. Discovery never launches a distro.
std::vector<Distro> runningDistros(const std::function<bool()>& cancelled = {});

class ComponentMissing : public std::runtime_error {
public:
    ComponentMissing() : std::runtime_error(
        "The WSL component is not installed in this distribution. Choose Install and retry to install it.") {}
};

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

    // Explicit user-authorized first installation. Existing installations are
    // updated automatically on connection when the packaged SHA256 changes.
    Json installComponent();

    // Thread-safe cancellation. The worker releases pipes and its launcher when
    // it observes cancellation; this does not shut down WSL or the distribution.
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wsl
