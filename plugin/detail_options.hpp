#pragma once

#include "common.hpp"
#include <set>

namespace wsl
{
enum DetailOptionId
{
    HideStoppedThreads = 3000,
    HideSleepingThreads,
    RunningThreadsOnly,
    HideKnownLibraries,
    HideLoader,
    HideMainModule,
    DeletedModulesOnly,
    HideAnonymousMemory,
    HideFileMemory,
    HideUnreadableMemory,
    ExecutableMemoryOnly,
    HideProcessEnvironment,
    HideUserEnvironment,
    HideSystemEnvironment,
    HideEmptyEnvironment,
    HideInheritedFiles,
    HideCloseOnExecFiles,
    HideSockets,
    HidePipes,
    HideAnonymousFiles,
    DeletedFilesOnly,
    PadModuleAddresses,
    PadMemoryAddresses,
    HighlightThreadSuspended,
    HighlightThreadDelay,
    HighlightThreadUserRequest,
    HighlightThreadAlert,
    HighlightThreadQueue,
    HighlightThreadExecutive,
    HighlightKnownLibraries,
    HighlightLoader,
    HighlightMappedModules,
    HighlightMemoryPrivate,
    HighlightMemorySystem,
    HighlightMemoryExecute,
    HighlightEnvironmentProcess,
    HighlightEnvironmentUser,
    HighlightEnvironmentSystem,
    HighlightInheritedFiles
};
struct DetailOption
{
    int id;
    const wchar_t *key;
    const wchar_t *label;
    // Non-null color names are local highlight switches, still gated by the
    // host's global UseColor setting. They never silently enable global colors.
    const wchar_t *color = nullptr;
    bool defaultEnabled = false;
};
const std::vector<DetailOption> &detailOptions(Table::Kind kind);
bool passesDetailFilters(Table::Kind kind, const Row &row, const std::set<int> &enabled);
} // namespace wsl
