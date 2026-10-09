#pragma once

#include "common.hpp"
#include <optional>

namespace wsl
{
enum ProcessSchedulingCommand
{
    ProcessNice = 4500,
    ProcessAffinity,
    ProcessPolicy,
    ProcessIoPriority,
    RemoveSavedScheduling
};

// Shared by the process row menu and the inspector's bottom Options menu.
// Linux scheduling is per thread, so these commands apply to all current threads.
void appendProcessSchedulingMenu(HMENU menu, bool enabled, const std::wstring &distro, const Json &process);
bool openProcessScheduling(HWND owner, const std::wstring &distro, const Json &process, int command);

// Local copy/open commands run here. Confirmed changes are returned to the
// inspector so its normal worker and refresh path remain responsible for them.
struct TargetOptionsResult
{
    std::optional<Json> request;
    bool forceScriptStacks = false;
};

TargetOptionsResult targetOptions(HWND owner, HWND anchor, const std::wstring &distro, const Json &process,
                                  const std::string &service, bool busy, bool scriptStacksForced = false);
} // namespace wsl
