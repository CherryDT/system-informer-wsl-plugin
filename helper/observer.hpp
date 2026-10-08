#pragma once

#include "json.hpp"
#include <cstdint>
#include <string>
#include <optional>
#include <vector>

namespace observer {
using Json = nlohmann::json;

// Linux PIDs are only meaningful inside this helper's PID namespace. A process
// identity includes its start time so a recycled PID cannot receive an action.
struct ProcessIdentity {
    int pid;
    uint64_t start_ticks;
};
struct ProcessStat {
    int pid = 0, ppid = 0, threads = 0, pgrp = 0, session = 0;
    int nice = 0, priority = 0, processor = -1, policy = 0;
    int64_t tty_nr = 0;
    uint64_t minor_faults = 0, major_faults = 0, user_ticks = 0, kernel_ticks = 0;
    std::string name, state;
    uint64_t start_ticks = 0, cpu_ticks = 0, virtual_bytes = 0, rss_bytes = 0;
};
struct CommandCredentials {
    uint32_t uid;
    uint32_t gid;
};
struct CommandResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string output;
};

bool append_with_budget(Json& array, Json item, size_t& bytes_left);
std::string read_text(const std::string& path, size_t limit = 1024 * 1024);
std::string read_link(const std::string& path);
std::vector<int> process_ids();
ProcessStat process_stat(int pid);
ProcessIdentity request_identity(const Json& request);
void require_identity(const ProcessIdentity& identity);
std::string boot_id();
Json hello();
Json snapshot(const Json& request);
Json process_details(const Json& request);
Json send_signal(const Json& request);
Json process_stacks(const Json& request);
std::string runtime_for_executable(std::string executable);
Json script_stacks(const Json& request);
// Resolve only administrator-installed tools. Java may additionally use a
// target-owned JDK after dropping to that target's exact effective credentials.
std::string trusted_command_path(const std::string& path, uint32_t owner = 0);
std::string find_command(const std::string& name);
Json connections(const Json& request);
Json find_handles(const Json& request);
CommandResult run_command(const std::vector<std::string>& arguments, int timeout_ms = 5000,
    size_t output_limit = 2 * 1024 * 1024,
    std::optional<CommandCredentials> credentials = std::nullopt,
    const std::string& java_tool = {});
Json services(const Json& request);
Json service_details(const Json& request);
Json service_action(const Json& request);
} // namespace observer
