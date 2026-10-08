#pragma once

#include "common.hpp"
#include <deque>
#include <map>

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
    GoToProcess
};
struct ProcessSample
{
    uint64_t ticks = 0, read = 0, written = 0;
};
struct View
{
    HWND window{}, distro{}, refresh{}, pause{}, settings{}, search{}, tabs{}, listeners{}, tree{}, inspect{},
        actions{}, exportButton{}, status{}, graph{}, memoryGraph{};
    Table processes, connections, services;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    Json snapshot, sockets, units;
    // Only the current process identities are retained between samples.
    std::map<std::string, ProcessSample> previous;
    std::map<std::string, double> cpu, readRate, writeRate;
    std::deque<double> history;
    std::deque<double> memoryHistory;
    uint64_t previousTime = 0;
    std::string bootId;
    std::wstring selectedDistro;
    std::wstring statistics;
    std::string pendingSelection;
    // Replies from earlier distro selections or disconnected sessions are ignored.
    unsigned epoch = 1;
    int page = 0;
    bool active = false, paused = false, pending = false, failed = false;
    Table &table()
    {
        return page == 0 ? processes : page == 1 ? connections : services;
    }
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
void render(View &view);
void updateSnapshot(View &view, const Json &data);

// Commands shared by toolbar buttons, keyboard shortcuts and context menus.
void inspect(View &view);
void action(View &view, int id);
void menu(View &view, POINT point);
} // namespace wsl::ui
