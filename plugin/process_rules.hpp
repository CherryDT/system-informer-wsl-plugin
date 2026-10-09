#pragma once
#include "common.hpp"
#include <optional>

namespace wsl
{
// Saved rules use the distro and complete /proc/PID/exe path. A process name
// alone can refer to unrelated programs, especially interpreters and services.
bool hasSavedScheduling(const std::wstring &distro);
bool hasSavedScheduling(const std::wstring &distro, const std::string &exe);
Json savedSchedulingInputs(const std::wstring &distro, const std::string &exe, const std::string &action);
void saveScheduling(const std::wstring &distro, const std::string &exe, const std::string &action,
                    const Json &inputs);
void removeSavedScheduling(const std::wstring &distro, const std::string &exe,
                           const std::string &action = {});

// Called by the controller, using its existing raw process snapshot. The
// returned request is already reserved and must eventually be finished, even
// when submission is cancelled or fails. Host text is parsed only when changed.
std::optional<Json> nextSavedScheduling(const std::wstring &distro, const Json &snapshot);
// Check immediately before sending a queued request: the user may have changed
// or removed its rule while earlier controller jobs were still running.
bool savedSchedulingCurrent(const std::wstring &distro, const Json &request);
void finishSavedScheduling(const std::wstring &distro, const Json &request, const std::string &error);
// A request discarded by capture policy has not failed to apply. Allow it to
// be selected again after capture resumes, without erasing completed actions.
void cancelSavedScheduling(const std::wstring &distro, const Json &request);
std::wstring savedSchedulingError(const std::wstring &distro);
} // namespace wsl
