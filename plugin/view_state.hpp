#pragma once

#include "common.hpp"
#include "capture_model.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <optional>

// Private to the WSL main view. Inspectors and the controller do not depend on
// this state; all access happens on the window thread.
namespace wsl::ui
{
// Control IDs and action commands belong to this view, never the host main window.
enum Id
{
    DistroCombo = 100,
    RefreshButton,
    PauseButton,
    SettingsButton,
    SearchEdit,
    ViewTabs,
    ProcessTable,
    ConnectionTable,
    ServiceTable,
    ListenerCheck,
    TreeCheck,
    InspectButton,
    ActionsButton,
    ExportButton,
    InstallButton,
    FindHandlesButton,
    InactiveServicesCheck,
    Inspect = 200,
    CopyRow,
    CopyCommand,
    OpenExecutable,
    Terminate,
    Kill,
    Suspend,
    Resume,
    User1,
    User2,
    Hangup,
    WindowChanged,
    StartService,
    StopService,
    RestartService,
    ReloadService,
    EnableService,
    DisableService,
    GoToProcess,
    InterruptSignal
};
struct View
{
    HWND window{}, distro{}, settings{}, search{}, tabs{}, listeners{}, tree{}, exportButton{}, status{},
        graph{}, memoryGraph{}, installNotice{}, installButton{}, tooltips{}, findHandles{},
        inactiveServices{};
    Table processes, connections, services;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    std::shared_ptr<CaptureModel> capture = std::make_shared<CaptureModel>();
    Json sockets, units;
    std::wstring selectedDistro;
    std::vector<std::wstring> distroNames;
    uint64_t renderedRevision = UINT64_MAX;
    uint64_t renderedSnapshotTime = 0;
    std::string pendingSelection;
    std::string pendingExecutable;
    std::string pendingService;
    // Replies from earlier distro selections or disconnected sessions are ignored.
    unsigned epoch = 1;
    int page = 0;
    bool active = false, pending = false;
    bool refreshAfterPending = false;
    bool foreground = false;
    bool collectConnections = false, collectServices = false;
    bool refreshServiceMetadata = true;
    bool cpuPercentOfTotal = true;
    Table &table()
    {
        return page == 0 ? processes : page == 1 ? connections : services;
    }
};

constexpr uintptr_t ExecutableTag = 7;
constexpr uintptr_t ServiceProcessTag = 8;

// Stable logical IDs: persisted column layouts must survive new optional columns.
enum ProcessColumn
{
    ProcessName,
    ProcessPid,
    ProcessUser,
    ProcessCpu,
    ProcessRss,
    ProcessRead,
    ProcessWrite,
    ProcessState,
    ProcessThreads,
    ProcessParent,
    ProcessCommand,
    ProcessUid,
    ProcessEuid,
    ProcessGid,
    ProcessEgid,
    ProcessTty,
    ProcessNice,
    ProcessPriority,
    ProcessAge,
    ProcessVirtual,
    ProcessSession,
    ProcessGroup,
    ProcessProcessor,
    ProcessMinorFaults,
    ProcessMajorFaults,
    ProcessExecutable,
    ProcessDirectory,
    ProcessCgroup,
    ProcessTracer,
    ProcessSwap,
    ProcessReadTotal,
    ProcessWriteTotal,
    ProcessReadChars,
    ProcessWriteChars,
    ProcessReadCalls,
    ProcessWriteCalls,
    ProcessVoluntarySwitches,
    ProcessInvoluntarySwitches,
    ProcessSeccomp,
    ProcessNoNewPrivileges,
    ProcessArchitecture,
    ProcessUserTime,
    ProcessKernelTime,
    ProcessPolicy,
    ProcessCpuAverage
};

// Window/controller boundary. Queue tags carry the current epoch in their high
// bits so a late worker reply cannot replace a newly selected distro's data.
std::wstring windowText(HWND window);
void status(View &view, const std::wstring &message);
void queue(View &view, Json request, uintptr_t tag);
void refresh(View &view);
void selectPage(View &view, int page);

// Snapshot state, filtering and table presentation.
void clearDistro(View &view);
void updateButtons(View &view);
void render(View &view, uintptr_t changed = 0);
std::string connectionKey(const Json &connection);

// Commands shared by toolbar buttons, keyboard shortcuts and context menus.
void inspect(View &view);
void goToProcess(View &view, const Json &process);
void action(View &view, int id);
void menu(View &view, POINT point);
} // namespace wsl::ui
