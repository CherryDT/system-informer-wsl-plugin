#include "capture.hpp"
#include "controller.hpp"
#include "host_bridge.h"
#include "process_rules.hpp"
#include "settings.hpp"
#include "transport.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <set>

namespace wsl
{
namespace
{
constexpr wchar_t CaptureClass[] = L"WslTools.Capture";
constexpr UINT SettingsMessage = WM_APP + 94;
constexpr DWORD DiscoveryInterval = 15000;

struct Distribution
{
    std::shared_ptr<CaptureModel> model = std::make_shared<CaptureModel>();
    uintptr_t pendingTag = 0;
    ULONGLONG lastRequest = 0;
    bool force = false;
};
struct PendingSnapshot
{
    std::wstring distro;
    bool accumulate = false;
};
struct Coordinator
{
    HWND window = nullptr, host = nullptr, view = nullptr;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    std::map<std::wstring, Distribution> distributions;
    std::vector<std::wstring> running;
    std::map<uintptr_t, PendingSnapshot> requests;
    std::wstring selected;
    Json visibleRequest;
    bool visible = false, background = true, automatic = true;
    bool forceDiscovery = true;
    bool forceNewOnDiscovery = false, discoveryManual = false;
    uintptr_t nextTag = 1, discoveryTag = 0;
    ULONGLONG lastDiscovery = 0;
};
std::unique_ptr<Coordinator> coordinator;
std::atomic<HWND> captureWindow{};

bool visible(const Coordinator &c)
{
    return c.visible && c.view && IsWindowVisible(c.view) && c.host && IsWindowVisible(c.host) &&
           !IsIconic(c.host);
}
bool eligible(const Coordinator &c, const std::wstring &distro)
{
    return c.background || (visible(c) && c.selected == distro);
}
void notify(Coordinator &c)
{
    if (c.view)
        PostMessageW(c.view, CaptureChangedMessage, 0, 0);
}
void cancelSnapshot(Coordinator &c, Distribution &entry)
{
    if (entry.pendingTag)
        c.requests.erase(entry.pendingTag);
    entry.pendingTag = 0;
    entry.model->pending = false;
}
void change(Coordinator &c, CaptureModel &model)
{
    ++model.revision;
    notify(c);
}
void reconcilePolicy(Coordinator &c)
{
    const bool background = readSetting(L"EnableBackgroundCapture", 1) != 0;
    const bool automatic = WslHostRefreshAutomatically() != FALSE;
    const bool changed = c.background != background || c.automatic != automatic;
    c.background = background;
    c.automatic = automatic;
    setCapturePolicy(background, visible(c), c.selected, automatic);
    const auto now = GetTickCount64();
    for (auto &[name, entry] : c.distributions)
    {
        auto &model = *entry.model;
        const bool suspended =
            !model.running || model.failed || model.componentMissing || !eligible(c, name) || !automatic;
        if (suspended != model.suspended)
        {
            cancelSnapshot(c, entry);
            model.suspended = suspended;
            if (suspended)
            {
                suspendCaptureModel(model, now);
                disconnect(name);
                entry.force = false;
            }
            else
            {
                resetCaptureBaseline(model);
                entry.lastRequest = 0;
                // A transport failure still needs explicit Refresh. Pausing
                // capture must not create an automatic restart loop.
            }
            change(c, model);
        }
        if (model.suspended)
        {
            const auto sequence = model.graphSequence;
            advanceCaptureGap(model, now, WslHostRefreshInterval());
            if (sequence != model.graphSequence)
                change(c, model);
        }
    }
    if (!background && !visible(c))
    {
        // A reply from an earlier discovery cannot restart capture after hide.
        c.discoveryTag = 0;
        c.forceDiscovery = false;
        c.forceNewOnDiscovery = c.discoveryManual = false;
    }
    else if (changed)
    {
        c.lastDiscovery = 0;
        c.forceDiscovery = automatic;
    }
}

Json requestFor(Coordinator &c, const std::wstring &name)
{
    if (visible(c) && name == c.selected && c.visibleRequest.is_object())
        return c.visibleRequest;
    Json request{{"op", "snapshot"}, {"fields", Json::array()}};
    if (hasSavedScheduling(name))
        request["fields"].push_back("exe");
    return request;
}
void tick(Coordinator &c)
{
    reconcilePolicy(c);
    const auto now = GetTickCount64();
    if (!c.background && !visible(c))
        return;
    if (!c.discoveryTag && (c.forceDiscovery || (c.automatic && (!c.lastDiscovery || now - c.lastDiscovery >=
                                                                                         DiscoveryInterval))))
    {
        c.discoveryTag = c.nextTag++;
        c.discoveryManual = c.forceNewOnDiscovery;
        c.forceNewOnDiscovery = false;
        c.forceDiscovery = false;
        c.lastDiscovery = now;
        submit({}, {{"op", "discover"}, {"_automatic_capture", c.automatic}}, c.mailbox, c.discoveryTag);
    }
    const auto interval = WslHostRefreshInterval();
    std::vector<std::pair<std::wstring, Distribution *>> due;
    for (auto &[name, entry] : c.distributions)
    {
        const auto &model = *entry.model;
        if (!model.running || !eligible(c, name) || model.pending || model.failed || model.componentMissing)
            continue;
        if (entry.force || (c.automatic && (!entry.lastRequest || now - entry.lastRequest >= interval)))
            due.emplace_back(name, &entry);
    }
    // Leave queue space for interactive commands. Oldest/never sampled distros
    // go first, so a long list cannot starve the entries at its end.
    std::stable_sort(due.begin(), due.end(), [](const auto &a, const auto &b) {
        if (a.second->force != b.second->force)
            return a.second->force;
        return a.second->lastRequest < b.second->lastRequest;
    });
    constexpr size_t MaximumPendingSnapshots = 8;
    for (auto &[name, item] : due)
    {
        if (c.requests.size() >= MaximumPendingSnapshots)
            break;
        auto &entry = *item;
        entry.force = false;
        entry.lastRequest = now;
        entry.pendingTag = c.nextTag++;
        entry.model->pending = true;
        auto request = requestFor(c, name);
        request["_automatic_capture"] = c.automatic;
        c.requests.emplace(entry.pendingTag, PendingSnapshot{name, c.automatic});
        submit(name, std::move(request), c.mailbox, entry.pendingTag);
    }
}

void discovered(Coordinator &c, const Json &data, bool manual)
{
    std::vector<std::wstring> names;
    std::set<std::wstring> seen;
    std::vector<Distribution *> newlyRunning;
    for (const auto &item : data)
    {
        auto name = wide(item.get<std::string>());
        if (!name.empty() && seen.insert(name).second)
            names.push_back(std::move(name));
    }
    for (auto &[name, entry] : c.distributions)
        if (!seen.count(name) && entry.model->running)
        {
            entry.model->running = false;
            cancelSnapshot(c, entry);
            entry.model->error = L"This distribution is no longer running.";
            change(c, *entry.model);
        }
    for (const auto &name : names)
    {
        auto [it, added] = c.distributions.try_emplace(name);
        auto &entry = it->second;
        if (added || !entry.model->running)
        {
            newlyRunning.push_back(&entry);
            entry.model->running = true;
            entry.model->failed = false;
            entry.model->componentMissing = false;
            entry.model->error.clear();
            entry.model->defaultUid = distroDefaultUid(name);
            resetCaptureBaseline(*entry.model);
            entry.lastRequest = 0;
            change(c, *entry.model);
        }
    }
    // Apply pause policy before setting the one-shot intent: a paused first
    // discovery must still sample new distributions when requested with F5.
    reconcilePolicy(c);
    if (manual)
        for (auto entry : newlyRunning)
            entry->force = true;
    if (c.running != names)
    {
        c.running = std::move(names);
        notify(c);
    }
}

LRESULT CALLBACK captureProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto c = reinterpret_cast<Coordinator *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        c = static_cast<Coordinator *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
    }
    if (!c)
        return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_TIMER || message == SettingsMessage)
    {
        tick(*c);
        return 0;
    }
    if (message == ReplyMessage)
    {
        std::unique_ptr<Reply> reply(reinterpret_cast<Reply *>(lparam));
        if (reply->tag == c->discoveryTag)
        {
            c->discoveryTag = 0;
            if (reply->error.empty())
            {
                try
                {
                    discovered(*c, reply->data, c->discoveryManual);
                }
                catch (const std::exception &)
                { /* Keep the last valid inventory. */
                }
            }
            c->discoveryManual = false;
            tick(*c);
            return 0;
        }
        auto pending = c->requests.find(reply->tag);
        if (pending == c->requests.end())
            return 0;
        const auto request = std::move(pending->second);
        c->requests.erase(pending);
        const auto found = c->distributions.find(request.distro);
        if (found == c->distributions.end() || found->second.pendingTag != reply->tag)
            return 0;
        auto &entry = found->second;
        auto &model = *entry.model;
        entry.pendingTag = 0;
        model.pending = false;
        if (!reply->error.empty())
        {
            model.failed = true;
            model.componentMissing = reply->componentMissing;
            model.error = wide(reply->error);
            resetCaptureBaseline(model);
        }
        else
        {
            try
            {
                updateCaptureSnapshot(model, reply->data, request.accumulate,
                                      std::max(1ul, WslHostIntegerSetting(L"SampleCount")));
                model.error.clear();
                model.failed = model.componentMissing = false;
            }
            catch (const std::exception &error)
            {
                model.failed = true;
                model.error = wide(error.what());
                resetCaptureBaseline(model);
            }
        }
        change(*c, model);
        tick(*c);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

void startCapture(HINSTANCE module, HWND host)
{
    if (coordinator)
        return;
    instance = module;
    auto c = std::make_unique<Coordinator>();
    c->host = host;
    c->background = readSetting(L"EnableBackgroundCapture", 1) != 0;
    c->automatic = WslHostRefreshAutomatically() != FALSE;
    c->forceDiscovery = c->background && c->automatic;
    WNDCLASSW cls{};
    cls.hInstance = module;
    cls.lpfnWndProc = captureProc;
    cls.lpszClassName = CaptureClass;
    RegisterClassW(&cls);
    c->window = CreateWindowExW(0, CaptureClass, L"WSL capture", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module,
                                c.get());
    if (!c->window)
        throw std::runtime_error("Could not create the WSL capture coordinator.");
    c->mailbox->window = c->window;
    setCapturePolicy(c->background, false, {}, c->automatic);
    try
    {
        startController();
        if (!SetTimer(c->window, 1, 250, nullptr))
            throw std::runtime_error("Could not start the WSL capture timer.");
    }
    catch (...)
    {
        c->mailbox->detach();
        DestroyWindow(c->window);
        stopController();
        throw;
    }
    captureWindow = c->window;
    coordinator = std::move(c);
    PostMessageW(coordinator->window, SettingsMessage, 0, 0);
}
void stopCapture()
{
    captureWindow = nullptr;
    if (!coordinator)
        return;
    auto c = std::move(coordinator);
    KillTimer(c->window, 1);
    c->mailbox->detach();
    stopController();
    MSG message{};
    while (PeekMessageW(&message, c->window, ReplyMessage, ReplyMessage, PM_REMOVE))
        delete reinterpret_cast<Reply *>(message.lParam);
    DestroyWindow(c->window);
}
std::vector<std::wstring> captureDistros()
{
    return coordinator ? coordinator->running : std::vector<std::wstring>{};
}
std::shared_ptr<CaptureModel> captureModel(const std::wstring &distro)
{
    if (coordinator && !distro.empty())
    {
        auto [it, added] = coordinator->distributions.try_emplace(distro);
        if (added)
            it->second.model->running = false;
        return it->second.model;
    }
    auto empty = std::make_shared<CaptureModel>();
    empty->running = false;
    return empty;
}
void setCaptureView(HWND window, const std::wstring &distro, bool shown, Json request)
{
    if (!coordinator)
        return;
    auto &c = *coordinator;
    const bool changed = c.selected != distro || c.visible != shown || c.visibleRequest != request;
    c.view = window;
    c.selected = distro;
    c.visible = shown;
    c.visibleRequest = std::move(request);
    reconcilePolicy(c);
    if (changed && visible(c))
    {
        if (auto found = c.distributions.find(distro); found != c.distributions.end())
            found->second.force = true;
        else
            c.forceDiscovery = true;
    }
    tick(c);
}
void detachCaptureView(HWND window)
{
    if (coordinator && coordinator->view == window)
    {
        coordinator->view = nullptr;
        coordinator->visible = false;
        reconcilePolicy(*coordinator);
    }
}
void refreshCapture(const std::wstring &distro, bool reconnect)
{
    if (!coordinator)
        return;
    auto &c = *coordinator;
    reconcilePolicy(c);
    auto found = c.distributions.find(distro);
    if (found == c.distributions.end() || !found->second.model->running)
    {
        c.forceDiscovery = true;
        c.forceNewOnDiscovery = true;
        if (c.discoveryTag)
            c.discoveryManual = true;
        tick(c);
        return;
    }
    auto &entry = found->second;
    if (reconnect)
    {
        cancelSnapshot(c, entry);
        disconnect(distro);
        entry.model->failed = entry.model->componentMissing = false;
        entry.model->error.clear();
        resetCaptureBaseline(*entry.model);
        change(c, *entry.model);
    }
    entry.force = true;
    tick(c);
}
void refreshCaptures(bool reconnect)
{
    if (!coordinator)
        return;
    auto &c = *coordinator;
    reconcilePolicy(c);
    c.forceDiscovery = true;
    c.forceNewOnDiscovery = true;
    // An automatic discovery may already have posted its reply. That reply
    // must honor F5 too, even if automatic refresh was just turned off.
    if (c.discoveryTag)
        c.discoveryManual = true;
    for (auto &[name, entry] : c.distributions)
    {
        if (reconnect && (entry.model->failed || entry.model->componentMissing))
        {
            cancelSnapshot(c, entry);
            disconnect(name);
            entry.model->failed = entry.model->componentMissing = false;
            entry.model->error.clear();
            resetCaptureBaseline(*entry.model);
            change(c, *entry.model);
        }
        entry.force = true;
    }
    tick(c);
}
void captureSettingsChanged()
{
    if (const HWND window = captureWindow.load())
        PostMessageW(window, SettingsMessage, 0, 0);
}
} // namespace wsl

extern "C" void WslStartCapture(HINSTANCE module, HWND host)
{
    try
    {
        wsl::startCapture(module, host);
    }
    catch (const std::exception &error)
    {
        wsl::errorBox(host, wsl::wide(error.what()));
    }
}
