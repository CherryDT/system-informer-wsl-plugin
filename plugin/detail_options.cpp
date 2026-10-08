#include "detail_options.hpp"

namespace wsl
{
const std::vector<DetailOption> &detailOptions(Table::Kind kind)
{
    static const std::vector<DetailOption> threads = {
        {HideStoppedThreads, L"Threads.HideStopped", L"Hide suspended threads"},
        {HideSleepingThreads, L"Threads.HideSleeping", L"Hide sleeping threads"},
        {RunningThreadsOnly, L"Threads.RunningOnly", L"Show only runnable threads"},
        {HighlightThreadSuspended, L"Threads.HighlightStopped", L"Highlight suspended",
         L"ColorThreadSuspended", true},
        {HighlightThreadDelay, L"Threads.HighlightDelay", L"Highlight delay execution",
         L"ColorThreadDelayExecution", true},
        {HighlightThreadUserRequest, L"Threads.HighlightUserRequest", L"Highlight user request",
         L"ColorThreadUserRequest", true},
        {HighlightThreadAlert, L"Threads.HighlightAlert", L"Highlight alert by thread ID",
         L"ColorThreadAlertByThreadId", true},
        {HighlightThreadQueue, L"Threads.HighlightQueue", L"Highlight queue", L"ColorThreadQueue", true},
        {HighlightThreadExecutive, L"Threads.HighlightExecutive", L"Highlight executive",
         L"ColorThreadExecutive", true}};
    static const std::vector<DetailOption> modules = {
        {HideKnownLibraries, L"Modules.HideKnown", L"Hide standard libraries"},
        {HideLoader, L"Modules.HideLoader", L"Hide native loader"},
        {HideMainModule, L"Modules.HideMain", L"Hide main executable"},
        {DeletedModulesOnly, L"Modules.DeletedOnly", L"Show only deleted modules"},
        {PadModuleAddresses, L"Modules.PadAddresses", L"Zero pad addresses", nullptr, true},
        {HighlightKnownLibraries, L"Modules.HighlightKnown", L"Highlight standard libraries",
         L"ColorModuleImageKnownDll", true},
        {HighlightLoader, L"Modules.HighlightLoader", L"Highlight native loader", L"ColorModuleSystem", true},
        {HighlightMappedModules, L"Modules.HighlightMapped", L"Highlight mapped modules",
         L"ColorModuleMapped", true}};
    static const std::vector<DetailOption> memory = {
        {HideAnonymousMemory, L"Memory.HideAnonymous", L"Hide anonymous mappings"},
        {HideFileMemory, L"Memory.HideFile", L"Hide file-backed mappings"},
        {HideUnreadableMemory, L"Memory.HideUnreadable", L"Hide unreadable mappings"},
        {ExecutableMemoryOnly, L"Memory.ExecutableOnly", L"Show only executable mappings"},
        {PadMemoryAddresses, L"Memory.PadAddresses", L"Zero pad addresses", nullptr, true},
        {HighlightMemoryPrivate, L"Memory.HighlightPrivate", L"Highlight private pages",
         L"ColorMemoryPrivatePages", true},
        {HighlightMemorySystem, L"Memory.HighlightSystem", L"Highlight system pages",
         L"ColorMemorySystemPages", true},
        {HighlightMemoryExecute, L"Memory.HighlightExecute", L"Highlight executable pages",
         L"ColorMemoryExecutePages", true}};
    static const std::vector<DetailOption> environment = {
        {HideProcessEnvironment, L"Environment.HideProcess", L"Hide process environment"},
        {HideUserEnvironment, L"Environment.HideUser", L"Hide user environment"},
        {HideSystemEnvironment, L"Environment.HideSystem", L"Hide system environment"},
        {HideEmptyEnvironment, L"Environment.HideEmpty", L"Hide empty values"},
        {HighlightEnvironmentProcess, L"Environment.HighlightProcess", L"Highlight process environment",
         L"ColorEnvironmentProcess", true},
        {HighlightEnvironmentUser, L"Environment.HighlightUser", L"Highlight user environment",
         L"ColorEnvironmentUser", true},
        {HighlightEnvironmentSystem, L"Environment.HighlightSystem", L"Highlight system environment",
         L"ColorEnvironmentSystem", true}};
    static const std::vector<DetailOption> handles = {
        {HideInheritedFiles, L"Handles.HideInherited", L"Hide inherited descriptors"},
        {HideCloseOnExecFiles, L"Handles.HideCloseOnExec", L"Hide close-on-exec descriptors"},
        {HideSockets, L"Handles.HideSockets", L"Hide sockets"},
        {HidePipes, L"Handles.HidePipes", L"Hide pipes"},
        {HideAnonymousFiles, L"Handles.HideAnonymous", L"Hide anonymous-inode descriptors"},
        {DeletedFilesOnly, L"Handles.DeletedOnly", L"Show only deleted files"},
        {HighlightInheritedFiles, L"Handles.HighlightInherited", L"Highlight inherited descriptors",
         L"ColorInheritHandles", true}};
    static const std::vector<DetailOption> none;
    switch (kind)
    {
    case Table::Kind::Threads:
        return threads;
    case Table::Kind::Modules:
        return modules;
    case Table::Kind::Memory:
        return memory;
    case Table::Kind::Environment:
        return environment;
    case Table::Kind::Handles:
        return handles;
    default:
        return none;
    }
}

bool passesDetailFilters(Table::Kind kind, const Row &row, const std::set<int> &enabled)
{
    const auto &data = row.data;
    auto on = [&](int id) { return enabled.count(id) != 0; };
    switch (kind)
    {
    case Table::Kind::Threads: {
        const auto state = data.value("state", std::string{});
        return !(on(HideStoppedThreads) && (state == "T" || state == "t")) &&
               !(on(HideSleepingThreads) && (state == "S" || state == "D" || state == "I")) &&
               (!on(RunningThreadsOnly) || state == "R");
    }
    case Table::Kind::Modules:
        return !(on(HideKnownLibraries) && data.value("known_library", false)) &&
               !(on(HideLoader) && data.value("native_module", false)) &&
               !(on(HideMainModule) && data.value("main_module", false)) &&
               (!on(DeletedModulesOnly) || data.value("deleted", false));
    case Table::Kind::Memory: {
        const auto path = data.value("path", std::string{});
        const auto protection = data.value("permissions", std::string{});
        const bool file = !path.empty() && path.front() == '/';
        return !(on(HideAnonymousMemory) && !file) && !(on(HideFileMemory) && file) &&
               (!on(HideUnreadableMemory) || protection.find('r') != std::string::npos) &&
               (!on(ExecutableMemoryOnly) || protection.find('x') != std::string::npos);
    }
    case Table::Kind::Environment: {
        const auto scope = data.value("scope", std::string("process"));
        return !(on(HideProcessEnvironment) && scope == "process") &&
               !(on(HideUserEnvironment) && scope == "user") &&
               !(on(HideSystemEnvironment) && scope == "system") &&
               !(on(HideEmptyEnvironment) && data.value("value", std::string{}).empty());
    }
    case Table::Kind::Handles: {
        const auto target = data.value("target", std::string{});
        const bool knownFlags = data.value("flags_text", std::string{}) != "Unknown";
        const bool deleted = target.size() >= 10 && target.compare(target.size() - 10, 10, " (deleted)") == 0;
        return !(on(HideInheritedFiles) && data.value("inherited", false)) &&
               !(on(HideCloseOnExecFiles) && knownFlags && !data.value("inherited", false)) &&
               !(on(HideSockets) && target.rfind("socket:[", 0) == 0) &&
               !(on(HidePipes) && target.rfind("pipe:[", 0) == 0) &&
               !(on(HideAnonymousFiles) && target.rfind("anon_inode:", 0) == 0) &&
               (!on(DeletedFilesOnly) || deleted);
    }
    default:
        return true;
    }
}
} // namespace wsl
