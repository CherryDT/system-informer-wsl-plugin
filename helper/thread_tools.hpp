#pragma once

#include "observer.hpp"

namespace observer {
// Cheap fields from a stat record that the process-details request already read.
Json thread_snapshot(int pid, int tid, const std::string& stat_text);
// Explicit diagnostics and scheduler changes for one thread identity. Setting
// all_threads applies scheduling to a single snapshot of the process's threads;
// results report partial failures and whether any mutation was applied. An
// optional expected_exe also guards against exec into a different executable.
Json thread_tool(const Json& request);
} // namespace observer
